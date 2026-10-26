#include "virtio_balloon.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>
namespace pc {
VirtioBalloon::VirtioBalloon(std::span<uint8_t> ram, uint64_t base, Interrupt irq, size_t granule,
                             Discard reclaim, bool pfn64)
    : VirtioDevice(ram, base, 5, 3, 3, std::move(irq)), ramBase(base),
      maximumPages(uint32_t(ram.size() / 4096)), hostPageSize(granule),
      pagesPerHostPage(granule / 4096), inflated(maximumPages),
      discarded(granule ? ram.size() / granule : 0), discard(std::move(reclaim)),
      legacyPfn64(pfn64) {
    if (granule < 4096 || (granule & (granule - 1)) || base % granule || ram.size() % granule ||
        ram.size() / 4096 > UINT32_MAX)
        throw std::runtime_error("Invalid balloon memory geometry");
}
void VirtioBalloon::setTargetPages(uint32_t pages) {
    if (pages > maximumPages)
        throw std::runtime_error("Balloon target exceeds guest RAM");
    if (target != pages) {
        target = pages;
        configurationChanged();
    }
}
void VirtioBalloon::requestStats() {
    if (!negotiated(1) || !running())
        return;
    wantStats = true;
    process(2);
}
void VirtioBalloon::deviceReset() {
    actual = inflatedCount = 0;
    std::fill(inflated.begin(), inflated.end(), false);
    std::fill(discarded.begin(), discarded.end(), false);
    wantStats = false;
    stats.clear();
}
uint64_t VirtioBalloon::configRead(unsigned offset, unsigned width) const {
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) {
        unsigned at = offset + i;
        if (at < 4)
            value |= uint64_t(uint8_t(target >> (at * 8))) << (i * 8);
        else if (at < 8)
            value |= uint64_t(uint8_t(actual >> ((at - 4) * 8))) << (i * 8);
    }
    return value;
}
void VirtioBalloon::configWrite(unsigned offset, uint64_t value, unsigned width) {
    uint32_t next = actual;
    for (unsigned i = 0; i < width; ++i) {
        unsigned at = offset + i;
        if (at >= 4 && at < 8) {
            auto shift = (at - 4) * 8;
            next = (next & ~(255u << shift)) | (uint32_t(uint8_t(value >> (i * 8))) << shift);
        }
    }
    if (next > maximumPages) {
        broken("Balloon actual exceeds guest RAM");
        return;
    }
    actual = next;
}
std::optional<uint32_t> VirtioBalloon::handle(unsigned queue, const Chain &chain) {
    if (chain.writable)
        throw std::runtime_error("Balloon buffers must be readable");
    if (queue == 2) {
        if (!negotiated(1))
            throw std::runtime_error("Balloon statistics not negotiated");
        auto bytes = chain.input(640);
        if (bytes.size() % 10)
            throw std::runtime_error("Invalid balloon statistics length");
        std::map<uint16_t, uint64_t> next;
        for (size_t at = 0; at < bytes.size(); at += 10) {
            uint16_t tag = bytes[at] | (uint16_t(bytes[at + 1]) << 8);
            uint64_t value = 0;
            for (unsigned i = 0; i < 8; ++i)
                value |= uint64_t(bytes[at + 2 + i]) << (i * 8);
            next[tag] = value;
        }
        stats = std::move(next);
        ++statsReports;
        if (!wantStats)
            return std::nullopt;
        wantStats = false;
        return 0;
    }
    auto bytes = chain.input(1024 * 1024);
    size_t entryWidth = legacyPfn64 ? 8 : 4;
    if (bytes.size() % entryWidth)
        throw std::runtime_error("Invalid balloon PFN buffer length");
    std::vector<uint32_t> pages;
    pages.reserve(bytes.size() / entryWidth);
    // Validate every PFN before changing accounting or advising the host VM.
    for (size_t at = 0; at < bytes.size(); at += entryWidth) {
        uint64_t pfn = 0;
        for (unsigned i = 0; i < entryWidth; ++i)
            pfn |= uint64_t(bytes[at + i]) << (i * 8);
        if (pfn > UINT32_MAX)
            throw std::runtime_error("Balloon PFN exceeds 32 bits");
        uint64_t address = uint64_t(pfn) * 4096;
        try {
            memory(address, 4096);
        } catch (const std::runtime_error &) {
            throw std::runtime_error("Invalid balloon PFN=" + std::to_string(pfn) +
                                     " entry=" + std::to_string(at / entryWidth) +
                                     " count=" + std::to_string(bytes.size() / entryWidth));
        }
        pages.push_back(uint32_t((address - ramBase) / 4096));
    }
    std::set<size_t> groups;
    for (auto page : pages) {
        auto group = page / pagesPerHostPage;
        groups.insert(group);
        if (queue == 0 && !inflated[page]) {
            inflated[page] = true;
            ++inflatedCount;
        }
        if (queue == 1 && inflated[page]) {
            inflated[page] = false;
            --inflatedCount;
        }
        if (queue == 1)
            discarded[group] = false;
    }
    if (queue == 0 && discard && negotiated(0))
        for (auto group : groups) {
            auto begin = group * pagesPerHostPage;
            if (!discarded[group] &&
                std::all_of(inflated.begin() + begin, inflated.begin() + begin + pagesPerHostPage,
                            [](bool value) { return value; })) {
                if (discard(ramBase + group * hostPageSize, hostPageSize)) {
                    discarded[group] = true;
                    discardBytes += hostPageSize;
                } else
                    ++discardFailures;
            }
        }
    if (queue == 0) {
        ++inflateRequests;
        inflatePages += pages.size();
    } else {
        ++deflateRequests;
        deflatePages += pages.size();
    }
    return 0;
}
} // namespace pc
