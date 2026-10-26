// Hyper-V's reference TSC page lets a guest compute partition elapsed time
// without a hypercall on each read. TSC means timestamp counter; this ARM64 model
// maps the native virtual counter into Hyper-V's 100-nanosecond reference units.
// A shared guest RAM page carries a 64-bit fractional scale, signed offset and
// sequence number. The guest reads sequence/counter/scale/offset/sequence and
// retries if the sequence changed; sequence zero tells it to use the reference
// counter register instead. This time is elapsed partition time, not UTC.
//
// The enclosing Hyper-V model owns privilege advertisement and synthetic-register
// access. This component validates page registration and publishes mappings only.
// All virtual processors must be stopped while page/backend counter state changes;
// sequence is invalidated first and a nonzero new sequence is published last.
// IMPLEMENTATION NOTE: The scale format represents only ratios below one, so
// frequencies at or below 10 MHz are rejected. Signed offset overflow also rejects
// a mapping. Failed registration preserves the previous page/state; failed rebase
// invalidates the registered page so the guest falls back. Reserved registration
// bits are required zero here, although TLFS describes them as preserve-on-write.
// Microsoft Hyper-V TLFS, Timers: Partition Reference Time Enlightenment,
// Reference TSC Page Register and Partition Reference TSC Mechanism.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#partition-reference-time-enlightenment

#include "arm_hyperv_reference_clock.hpp"
#include <algorithm>
#include <limits>
namespace pc {
namespace {
// HV_REFERENCE_TSC_PAGE fits one 4 KiB guest physical page; register bits [63:12]
// hold its page number, bit 0 enables it and bits [11:1] are reserved/preserved.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#reference-time-stamp-counter-tsc-page-register
constexpr uint64_t ReferencePageBytes = 4096, PageOffsetMask = ReferencePageBytes - 1;
constexpr uint64_t PageEnable = 1, ReservedRegistrationBits = 0xffe;
// TLFS reference counter ticks are 100 ns, therefore 10,000,000 ticks per second.
constexpr uint64_t ReferenceTicksPerSecond = 10000000;
// TscScale is a 64-bit fractional multiplier; a 128-bit product's high half is used.
constexpr unsigned ScaleFractionBits = 64, BitsPerByte = 8;
// HV_REFERENCE_TSC_PAGE byte offsets. Multi-byte fields use little-endian encoding.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#partition-reference-time-stamp-counter-page
enum PageField : unsigned {
    TscSequence = 0, // u32 generation; zero means the mapping is unavailable.
    Reserved = 4,    // u32 reserved plus the unused tail are cleared during publication.
    TscScale = 8,    // u64 multiplier converting virtual counter ticks to 100 ns units.
    TscOffset = 16,  // i64 additive correction after the high-half product.
};
// Store one page field little-endian while all guest virtual processors are stopped.
// IMPLEMENTATION NOTE: Publication helper for the TLFS reference page layout.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#partition-reference-time-stamp-counter-page
void write_page_field(std::span<uint8_t> bytes, size_t at, uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i)
        bytes[at + i] = uint8_t(value >> (BitsPerByte * i));
}
} // namespace
// Resolve the registered page GPA into bound RAM; return empty for an invalid page.
// Microsoft Hyper-V TLFS, Timers; stopped-VP publication/bounds are host policy.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#reference-time-stamp-counter-tsc-page-register
std::span<uint8_t> ArmHyperVReferenceClock::registered_page(uint64_t value) const {
    auto address = value & ~PageOffsetMask;
    if (address < base || address - base > ram.size() ||
        ram.size() - (address - base) < ReferencePageBytes)
        return {};
    return ram.subspan(size_t(address - base), ReferencePageBytes);
}
// Compute scale and signed offset matching a counter/reference sample; reject unrepresentable
// values. Microsoft Hyper-V TLFS, Timers; stopped-VP publication/bounds are host policy.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#partition-reference-tsc-mechanism
bool ArmHyperVReferenceClock::calculate_tsc_mapping(Sample s, uint64_t &scale, uint64_t &offset) {
    // A 64-bit fixed-point multiplier cannot represent ratios >= 1.
    if (s.frequency <= ReferenceTicksPerSecond)
        return false;
    scale = uint64_t((__uint128_t(ReferenceTicksPerSecond) << ScaleFractionBits) / s.frequency);
    auto normalized = uint64_t((__uint128_t(s.counter) * scale) >> ScaleFractionBits);
    __int128_t difference = __int128_t(s.reference100ns) - normalized;
    if (difference < std::numeric_limits<int64_t>::min() ||
        difference > std::numeric_limits<int64_t>::max())
        return false;
    offset = uint64_t(int64_t(difference));
    return true;
}
// Invalidate sequence, clear reserved bytes, publish scale/offset, then a new nonzero sequence.
// Microsoft Hyper-V TLFS, Timers; stopped-VP publication/bounds are host policy.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#partition-reference-tsc-mechanism
void ArmHyperVReferenceClock::publish_tsc_mapping(uint64_t scale, uint64_t offset) {
    auto p = registered_page(registered);
    // All VPs are stopped; invalidate first and publish sequence last. The
    // caller must finish any backend counter changes before resuming a VP.
    write_page_field(p, PageField::TscSequence, 0, sizeof(uint32_t));
    std::fill(p.begin() + PageField::Reserved, p.end(), 0);
    write_page_field(p, PageField::TscScale, scale, sizeof(uint64_t));
    write_page_field(p, PageField::TscOffset, offset, sizeof(int64_t));
    if (!++generation)
        ++generation;
    write_page_field(p, PageField::TscSequence, generation, sizeof(uint32_t));
    valid = true;
}
// Mark the enabled page unreliable with sequence zero and clear host validity.
// Microsoft Hyper-V TLFS, Timers; stopped-VP publication/bounds are host policy.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#partition-reference-tsc-mechanism
void ArmHyperVReferenceClock::invalidate_reference_page() {
    if (registered & PageEnable)
        write_page_field(registered_page(registered), PageField::TscSequence, 0, sizeof(uint32_t));
    valid = false;
}
// Validate registration and initial mapping before replacing prior state; publish when enabled.
// Microsoft Hyper-V TLFS, Timers; stopped-VP publication/bounds are host policy.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#reference-time-stamp-counter-tsc-page-register
bool ArmHyperVReferenceClock::configure_reference_page(uint64_t value, Sample sample) {
    if (value & ReservedRegistrationBits)
        return false;
    uint64_t scale = 0, offset = 0;
    if ((value & PageEnable) &&
        (registered_page(value).empty() || !calculate_tsc_mapping(sample, scale, offset)))
        return false;
    invalidate_reference_page();
    registered = value;
    if (value & PageEnable)
        publish_tsc_mapping(scale, offset);
    return true;
}
// Rebase an enabled page from a fresh counter sample; invalidate on mapping failure.
// Microsoft Hyper-V TLFS, Timers; stopped-VP publication/bounds are host policy.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#partition-reference-tsc-mechanism
bool ArmHyperVReferenceClock::update_reference_mapping(Sample sample) {
    if (!(registered & PageEnable))
        return false;
    uint64_t scale = 0, offset = 0;
    if (!calculate_tsc_mapping(sample, scale, offset)) {
        invalidate_reference_page();
        return false;
    }
    publish_tsc_mapping(scale, offset);
    return true;
}
} // namespace pc
