#include "arm_gpadl.hpp"
#include <algorithm>
#include <stdexcept>
#include <set>
namespace pc {
namespace {
uint64_t get(std::span<const uint8_t> bytes, size_t at, unsigned count) {
    uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= uint64_t(bytes[at + i]) << (8 * i);
    return value;
}
}
ArmGpadl::ArmGpadl(std::span<uint8_t> memory, uint64_t start) : ram(memory), base(start) {
    if (base & 4095) throw std::runtime_error("GPADL RAM base must be 4 KiB aligned");
}
ArmGpadl::Result ArmGpadl::begin(uint32_t channel, uint32_t id, uint16_t bytes, uint16_t ranges,
                               std::span<const uint8_t> fragment) {
    if (!channel || !id || !ranges || !bytes || (bytes & 7) || bytes < uint32_t(ranges) * 16 ||
        fragment.size() > bytes || (fragment.size() & 7)) return Result::Invalid;
    if (descriptors.contains(id)) return Result::InUse;
    if (descriptors.size() >= maxDescriptors) return Result::Limit;
    Descriptor candidate{channel, bytes, ranges, {fragment.begin(), fragment.end()}, {}, 0, false};
    if (candidate.encoded.size() == bytes && !finish(candidate)) return Result::Invalid;
    if (nextGeneration == UINT64_MAX) return Result::Limit;
    candidate.generation = ++nextGeneration;
    descriptors.emplace(id, std::move(candidate));
    return Result::Ok;
}
ArmGpadl::Result ArmGpadl::append(uint32_t id, std::span<const uint8_t> fragment) {
    auto it = descriptors.find(id);
    if (it == descriptors.end()) return Result::Missing;
    const auto &existing = it->second;
    if (existing.ready) return Result::InUse;
    if (fragment.empty() || (fragment.size() & 7) ||
        fragment.size() > existing.expectedBytes - existing.encoded.size()) return Result::Invalid;
    if (fragment.size() < existing.expectedBytes - existing.encoded.size()) {
        it->second.encoded.insert(it->second.encoded.end(), fragment.begin(), fragment.end());
        return Result::Ok;
    }
    // Reject malformed completion without consuming the previous fragments.
    auto candidate = existing;
    candidate.encoded.insert(candidate.encoded.end(), fragment.begin(), fragment.end());
    if (candidate.encoded.size() == candidate.expectedBytes && !finish(candidate)) return Result::Invalid;
    it->second = std::move(candidate);
    return Result::Ok;
}
bool ArmGpadl::finish(Descriptor &descriptor) const {
    std::vector<Range> ranges;
    uint64_t total = 0;
    size_t at = 0;
    const auto &bytes = descriptor.encoded;
    for (unsigned i = 0; i < descriptor.expectedRanges; ++i) {
        if (at > bytes.size() || bytes.size() - at < 8) return false;
        const auto count = uint32_t(get(bytes, at, 4)), offset = uint32_t(get(bytes, at + 4, 4));
        if (!count || offset >= 4096) return false;
        const uint64_t pages = (uint64_t(offset) + count + 4095) / 4096;
        at += 8;
        if (pages > (bytes.size() - at) / 8) return false;
        Range range{offset, count, {}};
        range.pages.reserve(size_t(pages));
        for (uint64_t page = 0; page < pages; ++page) {
            auto pfn = get(bytes, at, 8); at += 8;
            if (pfn > (UINT64_MAX >> 12)) return false;
            auto gpa = pfn << 12;
            if (gpa < base || gpa - base > ram.size() || 4096 > ram.size() - (gpa - base)) return false;
            range.pages.push_back(gpa);
        }
        total += count;
        ranges.push_back(std::move(range));
    }
    if (at != bytes.size()) return false;
    descriptor.ranges = std::move(ranges); descriptor.bytes = total; descriptor.ready = true;
    return true;
}
const ArmGpadl::Descriptor *ArmGpadl::find(uint32_t channel, uint32_t id) const {
    auto it = descriptors.find(id);
    return it != descriptors.end() && it->second.channel == channel && it->second.ready ? &it->second : nullptr;
}
std::optional<size_t> ArmGpadl::remainingBytes(uint32_t id) const {
    auto it = descriptors.find(id);
    if (it == descriptors.end()) return {};
    return it->second.expectedBytes - it->second.encoded.size();
}
std::optional<uint32_t> ArmGpadl::owner(uint32_t id) const {
    auto it = descriptors.find(id);
    if (it == descriptors.end()) return {};
    return it->second.channel;
}
std::optional<uint64_t> ArmGpadl::byteSize(uint32_t channel, uint32_t id) const {
    auto descriptor = find(channel, id);
    if (!descriptor) return {};
    return descriptor->bytes;
}
std::optional<uint64_t> ArmGpadl::generation(uint32_t channel, uint32_t id) const {
    auto descriptor = find(channel, id);
    if (!descriptor) return {};
    return descriptor->generation;
}
bool ArmGpadl::ringMapping(uint32_t channel, uint32_t id, uint64_t offset, uint64_t bytes) const {
    const auto descriptor = find(channel, id);
    if (!descriptor || (offset & 4095) || (bytes & 4095) || bytes < 8192 ||
        offset > descriptor->bytes || bytes > descriptor->bytes - offset) return false;
    std::set<uint64_t> pages;
    for (const auto &range : descriptor->ranges) {
        if (range.offset || (range.bytes & 4095)) return false;
        for (auto page : range.pages) if (!pages.insert(page).second) return false;
    }
    return true;
}
std::span<uint8_t> ArmGpadl::piece(const Descriptor &descriptor, uint64_t offset, size_t maximum) const {
    for (const auto &range : descriptor.ranges) {
        if (offset >= range.bytes) { offset -= range.bytes; continue; }
        const uint64_t physical = range.offset + offset;
        const auto inPage = size_t(physical & 4095);
        const auto length = size_t(std::min<uint64_t>(std::min<size_t>(maximum, 4096 - inPage), range.bytes - offset));
        return ram.subspan(size_t(range.pages[size_t(physical / 4096)] - base) + inPage, length);
    }
    return {};
}
bool ArmGpadl::aliasesRam(std::span<const uint8_t> bytes) const {
    if (bytes.empty() || ram.empty()) return false;
    const auto start = reinterpret_cast<uintptr_t>(bytes.data());
    const auto memory = reinterpret_cast<uintptr_t>(ram.data());
    return start >= memory ? start - memory < ram.size() : bytes.size() > memory - start;
}
bool ArmGpadl::read(uint32_t channel, uint32_t id, uint64_t offset, std::span<uint8_t> out) const {
    auto descriptor = find(channel, id);
    if (!descriptor || offset > descriptor->bytes || out.size() > descriptor->bytes - offset || aliasesRam(out)) return false;
    while (!out.empty()) {
        auto part = piece(*descriptor, offset, out.size());
        std::copy(part.begin(), part.end(), out.begin());
        offset += part.size(); out = out.subspan(part.size());
    }
    return true;
}
bool ArmGpadl::write(uint32_t channel, uint32_t id, uint64_t offset, std::span<const uint8_t> in) {
    auto descriptor = find(channel, id);
    if (!descriptor || offset > descriptor->bytes || in.size() > descriptor->bytes - offset || aliasesRam(in)) return false;
    while (!in.empty()) {
        auto part = piece(*descriptor, offset, in.size());
        std::copy_n(in.begin(), part.size(), part.begin());
        offset += part.size(); in = in.subspan(part.size());
    }
    return true;
}
ArmGpadl::Result ArmGpadl::remove(uint32_t channel, uint32_t id) {
    auto it = descriptors.find(id);
    if (it == descriptors.end()) return Result::Missing;
    if (it->second.channel != channel) return Result::WrongChannel;
    descriptors.erase(it); return Result::Ok;
}
void ArmGpadl::clearChannel(uint32_t channel) {
    for (auto it = descriptors.begin(); it != descriptors.end();) {
        if (it->second.channel == channel) it = descriptors.erase(it); else ++it;
    }
}
}
