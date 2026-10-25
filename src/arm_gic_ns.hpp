#pragma once
#include <cstdint>
#include <functional>
namespace pc {
// Nonsecure, affinity-routed distributor view over an EL1-only native GIC.
// The host owns group assignment: every exposed interrupt must be Group 1.
// No secure-world or nested interrupt controller is exposed to the guest.
class ArmGicNonsecureView {
  public:
    static constexpr uint64_t guestBase = 0x08000000, nativeBase = 0xf00000000ull;
    using Read = std::function<uint64_t(uint32_t)>;
    using Write = std::function<void(uint32_t, uint64_t)>;
    ArmGicNonsecureView(Read read, Write write);
    uint64_t read(uint64_t offset, unsigned width) const;
    void write(uint64_t offset, uint64_t value, unsigned width);
    unsigned interruptCount() const { return interrupts; }
  private:
    Read load;
    Write store;
    uint32_t typer;
    unsigned interrupts;
    static void validate(uint64_t offset, unsigned width);
    uint32_t bitmapMask(unsigned first) const;
};
}
