#pragma once
#include <cstdint>
#include <span>
namespace pc {
// TLFS reference-TSC page, also used with the ARM64 virtual counter.
// Caller must stop every guest VP before configuring/updating page memory.
// This model alone does not advertise the privilege or bind a synthetic register.
class ArmHyperVReferenceClock {
  public:
    // A paired native counter/reference sample, with counter frequency in Hz.
    struct Sample {
        uint64_t counter, reference100ns, frequency;
    };
    // Bind guest RAM; leave the reference page disabled until registration.
    // IMPLEMENTATION NOTE: Host wiring around the TLFS reference TSC page.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#partition-reference-time-stamp-counter-page
    ArmHyperVReferenceClock(std::span<uint8_t> ram, uint64_t base) : ram(ram), base(base) {}
    // Invalid registration/sample leaves both prior state and guest memory intact.
    bool configure_reference_page(uint64_t value, Sample sample);
    // Rebase after native counter offset changes. Unrepresentable mappings
    // invalidate the active page so the guest must use the reference hypercall.
    bool update_reference_mapping(Sample sample);
    void invalidate_reference_page();
    // Inspect the last accepted synthetic-register value without touching guest RAM.
    // Microsoft TLFS Reference TSC Page Register.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#reference-time-stamp-counter-tsc-page-register
    uint64_t registration() const {
        return registered;
    }
    // Inspect the published generation, or zero when no valid mapping is available.
    // Microsoft TLFS Partition Reference TSC Mechanism.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#partition-reference-tsc-mechanism
    uint32_t sequence() const {
        return valid ? generation : 0;
    }

  private:
    std::span<uint8_t> ram;
    uint64_t base, registered = 0;
    uint32_t generation = 0;
    bool valid = false;
    static bool calculate_tsc_mapping(Sample sample, uint64_t &scale, uint64_t &offset);
    std::span<uint8_t> registered_page(uint64_t value) const;
    void publish_tsc_mapping(uint64_t scale, uint64_t offset);
};
} // namespace pc
