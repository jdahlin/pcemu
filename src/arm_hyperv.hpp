#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
namespace pc {
// ARM64 TLFS discovery and synthetic registers. No x86 CPUID/MSR emulation.
// Called only with all vCPUs stopped, just like the other ARM device models.
class ArmHyperV {
  public:
    using Registers = std::array<uint64_t, 18>;
    // Clock returns monotonic host time in 100 ns units; injectable for rate tests.
    using Clock = std::function<uint64_t()>;
    ArmHyperV(bool enabled, std::span<uint8_t> ram, uint64_t base, unsigned cpus, Clock clock = {});
    // False leaves the call to PSCI/the platform's unsupported-call fallback.
    bool call(unsigned immediate, Registers &registers, unsigned vp);
    std::string report() const;
    // One bounded last-event snapshot, drained by the platform after each HVC.
    std::string takeCrashReport();
  private:
    struct Counts { uint64_t reads = 0, writes = 0, failures = 0; };
    bool enabled;
    std::span<uint8_t> ram;
    uint64_t base, guestId = 0, partitionInfo = 0;
    unsigned cpus;
    Clock clock;
    uint64_t clockOrigin, referenceTime = 0;
    std::array<uint64_t, 8> tlbiControl{}, syntheticVectors{};
    std::array<std::array<uint64_t, 5>, 8> crashParameters{};
    std::array<uint64_t, 5> notifiedParameters{};
    uint64_t crashNotifications = 0, crashControl = 0;
    unsigned crashVp = 0;
    std::string crashMessageHex;
    bool crashPending = false;
    std::string crashReport() const;
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
