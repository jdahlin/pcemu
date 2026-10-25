#pragma once
#include <cstdint>
#include <span>
namespace pc {
// TLFS reference-TSC page, also used with the ARM64 virtual counter.
// Caller must stop every guest VP before configuring/updating page memory.
// This model alone does not advertise the privilege or bind a synthetic register.
class ArmHyperVReferenceClock {
  public:
    struct Sample { uint64_t counter, reference100ns, frequency; };
    ArmHyperVReferenceClock(std::span<uint8_t> ram, uint64_t base) : ram(ram), base(base) {}
    // Invalid registration/sample leaves both prior state and guest memory intact.
    bool configure(uint64_t value, Sample sample);
    // Rebase after native counter offset changes. Unrepresentable mappings
    // invalidate the active page so the guest must use the reference hypercall.
    bool update(Sample sample);
    void invalidate();
    uint64_t registration() const { return registered; }
    uint32_t sequence() const { return valid ? generation : 0; }
  private:
    std::span<uint8_t> ram;
    uint64_t base, registered = 0;
    uint32_t generation = 0;
    bool valid = false;
    static bool mapping(Sample sample, uint64_t &scale, uint64_t &offset);
    std::span<uint8_t> page(uint64_t value) const;
    void publish(uint64_t scale, uint64_t offset);
};
}
