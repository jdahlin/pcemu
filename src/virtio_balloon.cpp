// The traditional Virtio memory balloon lets the host ask the guest to release
// RAM. num_pages is the host's target; actual is the guest's reported balloon size.
// The guest inflates by submitting physical page-frame numbers (PFNs) on inflateq,
// and deflates by submitting them on deflateq before reusing memory. Each PFN names
// a 4096-byte unit, even if the host uses larger physical pages. The shared Virtio
// core owns transport registers, descriptor validation, completion and interrupts.
//
// This model separately tracks the pages actually submitted, so guest-reported
// actual need not equal tracked inflation during updates. Repeated PFNs do not
// inflate accounting twice. Host memory is discarded only when every 4 KiB page
// in a host granule is inflated and MUST_TELL_HOST was negotiated; without it,
// the guest could reuse pages before deflation acknowledgement. A failed discard
// retains accounting and may be retried. Statistics use a retained readable buffer:
// the host returns that buffer to request the next guest statistics report.
//
// IMPLEMENTATION NOTE: RAM geometry must align to the host discard granule. PFNs
// are all validated before accounting/discard changes; request limits bound host
// work. Discard callback success is a host reclaim result, not a guest protocol field.
// Reset clears accounting/statistics but preserves the host's requested target.
// COMPATIBILITY: An explicit legacyPfn64 option accepts eight-byte little-endian
// PFN entries from existing guests, still requiring values to fit 32 bits. The
// standard inflation/deflation format uses four-byte entries. This extension has
// no negotiated feature and is enabled by board policy, not inferred from data.
// Only MUST_TELL_HOST and STATS_VQ are offered; no free-page hint/reporting queues.
// Virtio 1.2, 5.5 Traditional Memory Balloon Device.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3140005
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3240006

#include "virtio_balloon.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>
namespace pc {
namespace {
// Virtio 1.2, 5.5.1/5.5.2: traditional balloon type and queue indices.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3140005
constexpr uint32_t BalloonDeviceId = 5;
enum QueueIndex : unsigned {
    InflateQueue = 0,    // Guest gives unused physical pages to the balloon.
    DeflateQueue = 1,    // Guest requests pages back before reusing them.
    StatisticsQueue = 2, // Optional device-driven memory statistics exchange.
};
constexpr unsigned QueueCount = 3; // Inflate, deflate and the offered statistics queue.
// Feature bit positions, 5.5.3. The host may reclaim inflated pages only when it
// knows the guest will notify before reuse; STATS_VQ enables the third queue.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3170003
constexpr unsigned MustTellHostFeatureBit = 0, StatisticsQueueFeatureBit = 1;
constexpr uint64_t OfferedFeatures =
    (1ull << MustTellHostFeatureBit) | (1ull << StatisticsQueueFeatureBit);
// Device configuration byte offsets, 5.5.4; both fields are little-endian u32.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3210004
enum ConfigurationField : unsigned {
    RequestedPages = 0,   // num_pages: host-written target, read-only to the guest.
    ActualPages = 4,      // actual: guest-written size after inflation/deflation.
    ConfigurationEnd = 8, // Optional hint/poison fields are not offered.
};
// Inflation/deflation PFNs represent fixed 4096-byte units, regardless of host pages.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3240006
constexpr unsigned BalloonPageBytes = 4096;
// Standard PFNs are u32; the explicitly enabled compatibility format uses u64.
constexpr unsigned StandardPfnBytes = 4, CompatibilityPfnBytes = 8, BitsPerByte = 8;
// Statistics are packed tag:u16,value:u64 records, not naturally padded structs.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3280003
constexpr unsigned StatisticRecordBytes = 10, StatisticValueOffset = 2, StatisticValueBytes = 8;
// IMPLEMENTATION NOTE: Accept at most 64 statistics records or 1 MiB of PFNs.
constexpr size_t MaximumStatisticsBytes = 64 * StatisticRecordBytes;
constexpr size_t MaximumPfnBufferBytes = 1024 * 1024;
} // namespace

// Bind RAM/reclaim geometry and select standard or compatibility PFN width.
// Virtio 1.2, 5.5; RAM/discard bounds and compatibility PFN width are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3240006
VirtioBalloon::VirtioBalloon(std::span<uint8_t> ram, uint64_t base, Interrupt irq, size_t granule,
                             Discard reclaim, bool pfn64)
    : VirtioDevice(ram, base, BalloonDeviceId, QueueCount, OfferedFeatures, std::move(irq)),
      ramBase(base), maximumPages(uint32_t(ram.size() / BalloonPageBytes)), hostPageSize(granule),
      pagesPerHostPage(granule / BalloonPageBytes), inflated(maximumPages),
      discarded(granule ? ram.size() / granule : 0), discard(std::move(reclaim)),
      legacyPfn64(pfn64) {
    if (granule < BalloonPageBytes || (granule & (granule - 1)) || base % granule ||
        ram.size() % granule || ram.size() / BalloonPageBytes > UINT32_MAX)
        throw std::runtime_error("Invalid balloon memory geometry");
}
// Set num_pages and notify the driver when the host target changes; reject excess RAM.
// Virtio 1.2, 5.5; RAM/discard bounds and compatibility PFN width are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3210004
void VirtioBalloon::set_requested_pages(uint32_t pages) {
    if (pages > maximumPages)
        throw std::runtime_error("Balloon target exceeds guest RAM");
    if (target != pages) {
        target = pages;
        configuration_changed();
    }
}
// Return the retained stats buffer to request another report, when negotiated and running.
// Virtio 1.2, 5.5; RAM/discard bounds and compatibility PFN width are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3280003
void VirtioBalloon::request_statistics() {
    if (!negotiated(StatisticsQueueFeatureBit) || !running())
        return;
    wantStats = true;
    process_queue(QueueIndex::StatisticsQueue);
}
// Clear tracked inflation and guest actual/statistics while retaining the host target.
// Virtio 1.2, 5.5; RAM/discard bounds and compatibility PFN width are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3230005
void VirtioBalloon::reset_device() {
    actual = inflatedCount = 0;
    std::fill(inflated.begin(), inflated.end(), false);
    std::fill(discarded.begin(), discarded.end(), false);
    wantStats = false;
    stats.clear();
}
// Read little-endian num_pages/actual bytes; unsupported configuration reads zero.
// Virtio 1.2, 5.5; RAM/discard bounds and compatibility PFN width are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3210004
uint64_t VirtioBalloon::read_device_configuration(unsigned offset, unsigned width) const {
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) {
        unsigned at = offset + i;
        if (at < ConfigurationField::ActualPages)
            value |= uint64_t(uint8_t(target >> (at * BitsPerByte))) << (i * BitsPerByte);
        else if (at < ConfigurationField::ConfigurationEnd)
            value |=
                uint64_t(uint8_t(actual >> ((at - ConfigurationField::ActualPages) * BitsPerByte)))
                << (i * BitsPerByte);
    }
    return value;
}
// Update guest actual bytes; reject sizes beyond RAM and ignore writes to num_pages.
// Virtio 1.2, 5.5; RAM/discard bounds and compatibility PFN width are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3210004
void VirtioBalloon::write_device_configuration(unsigned offset, uint64_t value, unsigned width) {
    uint32_t next = actual;
    for (unsigned i = 0; i < width; ++i) {
        unsigned at = offset + i;
        if (at >= ConfigurationField::ActualPages && at < ConfigurationField::ConfigurationEnd) {
            auto shift = (at - ConfigurationField::ActualPages) * BitsPerByte;
            next = (next & ~(uint32_t(UINT8_MAX) << shift)) |
                   (uint32_t(uint8_t(value >> (i * BitsPerByte))) << shift);
        }
    }
    if (next > maximumPages) {
        require_device_reset("Balloon actual exceeds guest RAM");
        return;
    }
    actual = next;
}
// Validate and account inflate/deflate PFNs or retain/return a statistics report buffer.
// Virtio 1.2, 5.5; RAM/discard bounds and compatibility PFN width are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3240006
std::optional<uint32_t> VirtioBalloon::handle(unsigned queue, const Chain &chain) {
    if (chain.writable)
        throw std::runtime_error("Balloon buffers must be readable");
    if (queue == QueueIndex::StatisticsQueue) {
        if (!negotiated(StatisticsQueueFeatureBit))
            throw std::runtime_error("Balloon statistics not negotiated");
        auto bytes = chain.input(MaximumStatisticsBytes);
        if (bytes.size() % StatisticRecordBytes)
            throw std::runtime_error("Invalid balloon statistics length");
        std::map<uint16_t, uint64_t> next;
        for (size_t at = 0; at < bytes.size(); at += StatisticRecordBytes) {
            uint16_t tag = bytes[at] | (uint16_t(bytes[at + 1]) << BitsPerByte);
            uint64_t value = 0;
            for (unsigned i = 0; i < StatisticValueBytes; ++i)
                value |= uint64_t(bytes[at + StatisticValueOffset + i]) << (i * BitsPerByte);
            next[tag] = value;
        }
        stats = std::move(next);
        ++statsReports;
        if (!wantStats)
            return std::nullopt;
        wantStats = false;
        return 0;
    }
    auto bytes = chain.input(MaximumPfnBufferBytes);
    size_t entryWidth = legacyPfn64 ? CompatibilityPfnBytes : StandardPfnBytes;
    if (bytes.size() % entryWidth)
        throw std::runtime_error("Invalid balloon PFN buffer length");
    std::vector<uint32_t> pages;
    pages.reserve(bytes.size() / entryWidth);
    // Validate every PFN before changing accounting or advising the host VM.
    for (size_t at = 0; at < bytes.size(); at += entryWidth) {
        uint64_t pfn = 0;
        for (unsigned i = 0; i < entryWidth; ++i)
            pfn |= uint64_t(bytes[at + i]) << (i * BitsPerByte);
        if (pfn > UINT32_MAX)
            throw std::runtime_error("Balloon PFN exceeds 32 bits");
        uint64_t address = uint64_t(pfn) * BalloonPageBytes;
        try {
            memory(address, BalloonPageBytes);
        } catch (const std::runtime_error &) {
            throw std::runtime_error("Invalid balloon PFN=" + std::to_string(pfn) +
                                     " entry=" + std::to_string(at / entryWidth) +
                                     " count=" + std::to_string(bytes.size() / entryWidth));
        }
        pages.push_back(uint32_t((address - ramBase) / BalloonPageBytes));
    }
    // Map absolute wire PFNs to RAM-relative 4 KiB accounting slots and then
    // host discard granules. Deflating any member makes the group reclaimable again
    // only after a later inflation has supplied all members.
    std::set<size_t> groups;
    for (auto page : pages) {
        auto group = page / pagesPerHostPage;
        groups.insert(group);
        if (queue == QueueIndex::InflateQueue && !inflated[page]) {
            inflated[page] = true;
            ++inflatedCount;
        }
        if (queue == QueueIndex::DeflateQueue && inflated[page]) {
            inflated[page] = false;
            --inflatedCount;
        }
        if (queue == QueueIndex::DeflateQueue)
            discarded[group] = false;
    }
    if (queue == QueueIndex::InflateQueue && discard && negotiated(MustTellHostFeatureBit))
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
    if (queue == QueueIndex::InflateQueue) {
        ++inflateRequests;
        inflatePages += pages.size();
    } else {
        ++deflateRequests;
        deflatePages += pages.size();
    }
    return 0;
}
} // namespace pc
