#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
namespace pc {
// ARM64 TLFS discovery and synthetic registers. No x86 CPUID/MSR emulation.
// Called only with all vCPUs stopped, just like the other ARM device models.
class ArmHyperV {
  public:
    using Registers = std::array<uint64_t, 18>;
    ArmHyperV(bool enabled, std::span<uint8_t> ram, uint64_t base, unsigned cpus);
    // False leaves the call to PSCI/the platform's unsupported-call fallback.
    bool call(unsigned immediate, Registers &registers, unsigned vp);
    std::string report() const;
  private:
    struct Counts { uint64_t reads = 0, writes = 0, failures = 0; };
    bool enabled;
    std::span<uint8_t> ram;
    uint64_t base, guestId = 0;
    unsigned cpus;
    uint64_t uidQueries = 0, versionQueries = 0, smcccCalls = 0, legacyCalls = 0;
    uint64_t disabledCalls = 0, failures = 0, omittedCounters = 0;
    uint64_t lastControl = 0, lastResult = 0;
    uint32_t lastRegister = 0;
    std::map<uint32_t, Counts> registers;
    std::map<uint16_t, uint64_t> calls;
    uint64_t invoke(Registers &r, bool smccc, unsigned vp);
    uint16_t readRegister(uint32_t name, unsigned vp, std::array<uint64_t, 2> &value);
    uint16_t writeRegister(uint32_t name, unsigned vp, std::array<uint64_t, 2> value);
    Counts *count(uint32_t name);
};
} // namespace pc
