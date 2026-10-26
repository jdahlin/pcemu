// Hyper-V synthetic timers provide four deadlines per virtual processor (VP).
// Each has a configuration register and a count register in 100 ns partition
// reference-time units. A one-shot count is an absolute expiration; a periodic
// count is an interval. Enable arms the timer, AutoEnable permits a nonzero count
// write to enable it, and zero count disables it. DirectMode selects a native
// interrupt; otherwise SINTx selects a Synthetic Interrupt Controller message slot.
// The delivery callback owns actual interrupt/message construction and injection.
//
// Expirations are serviced with all VPs stopped and never delivered early. A busy
// or missing backend keeps an expiration pending for retry. Successful periodic
// delivery advances to the next phase-aligned period; missed periods are coalesced
// into one notification per service pass. Lazy timers may be omitted from wakeup
// queries, but are still considered when service runs. Cached minimum deadlines
// avoid rescanning all timers when register writes leave their inputs unchanged.
//
// IMPLEMENTATION NOTE: At most eight VPs are stored. An internal 128-bit timeline
// extends the wire's 64-bit counter; every decreasing observed sample is treated
// as one wrap, so callers must provide ordered monotonic samples modulo wrap.
// Direct vector fields are restricted to the five bits used by this ARM backend's
// private interrupts. Host activity counters/cache state do not appear in guest
// registers. Register writes retain rearming behavior even when values repeat.
// Microsoft Hyper-V TLFS, Timers: Synthetic Timers and Synthetic Timer Registers.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#synthetic-timers
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_register_name#synthetic-timers

#include "arm_hyperv_timers.hpp"
#include <algorithm>
#include <stdexcept>
namespace pc {
namespace {
// ARM64 synthetic register identifiers: alternating config/count for STIMER0..3.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_register_name#synthetic-timers
enum Register : uint32_t {
    Timer0ConfigurationRegister = 0xb0000,
    Timer0CountRegister = 0xb0001,
    Timer1ConfigurationRegister = 0xb0002,
    Timer1CountRegister = 0xb0003,
    Timer2ConfigurationRegister = 0xb0004,
    Timer2CountRegister = 0xb0005,
    Timer3ConfigurationRegister = 0xb0006,
    Timer3CountRegister = 0xb0007,
};
constexpr unsigned RegistersPerTimer = 2;
constexpr uint32_t CountRegisterSelector = 1; // Low identifier bit chooses Count over Config.
// Configuration register bits, TLFS Synthetic Timer Configuration Register.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#synthetic-timer-registers
constexpr uint64_t Enable = 1ull << 0;      // Enable timer expiration processing.
constexpr uint64_t Periodic = 1ull << 1;    // Count is an interval instead of an absolute time.
constexpr uint64_t Lazy = 1ull << 2;        // Timer need not wake an otherwise unavailable VP.
constexpr uint64_t AutoEnable = 1ull << 3;  // Nonzero Count write sets Enable.
constexpr uint64_t DirectMode = 1ull << 12; // Deliver a native interrupt instead of a message.
constexpr uint64_t SyntheticInterruptMask = 0xfull << 16; // SINTx slot bits [19:16].
// IMPLEMENTATION NOTE: Restrict vector bits [11:4] to five-bit private GIC IDs.
// The public TLFS table describes an eight-bit x64 APIC vector in this field.
constexpr uint64_t ArmDirectVectorMask = 0x1full << 4;
constexpr uint64_t ConfigurationFlags = Enable | Periodic | Lazy | AutoEnable;
constexpr uint64_t WritableConfiguration =
    ConfigurationFlags | ArmDirectVectorMask | DirectMode | SyntheticInterruptMask;
// Hyper-V status values: success, invalid parameter/register and invalid VP index.
// https://raw.githubusercontent.com/Microsoft/Virtualization-Documentation/master/tlfs/Hypervisor%20Top%20Level%20Functional%20Specification%20v6.0b.pdf#page=214
constexpr uint16_t Success = 0, InvalidParameter = 5, InvalidVpIndex = 14;
// The wire reference counter/count are 64 bits; the internal epoch counts wraps.
constexpr unsigned ReferenceCounterBits = 64;
} // namespace
// Create disabled timer state; reject a VP count beyond host storage.
// Microsoft Hyper-V TLFS; extended epochs, host storage/cache and counters are host policy.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#synthetic-timers
ArmHyperVTimers::ArmHyperVTimers(unsigned count) : cpus(count) {
    if (!count || count > timers.size())
        throw std::runtime_error("Invalid synthetic timer VP count");
}
// Map a wire counter sample into the current extended wrap epoch without changing state.
// Microsoft Hyper-V TLFS; extended epochs, host storage/cache and counters are host policy.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#reference-counter
ArmHyperVTimers::Time ArmHyperVTimers::project(uint64_t now) const {
    auto epoch = lastSample & ~Time(UINT64_MAX);
    if (sampled && now < uint64_t(lastSample))
        epoch += Time(1) << ReferenceCounterBits;
    return epoch | now;
}
// Record an ordered reference-time sample and extend its wrap epoch.
// Microsoft Hyper-V TLFS; extended epochs, host storage/cache and counters are host policy.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#reference-counter
ArmHyperVTimers::Time ArmHyperVTimers::observe(uint64_t now) {
    lastSample = project(now);
    sampled = true;
    return lastSample;
}
// Apply Enable/Count/Periodic rules and derive an absolute internal expiration.
// Microsoft Hyper-V TLFS; extended epochs, host storage/cache and counters are host policy.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#synthetic-timer-registers
void ArmHyperVTimers::arm(Timer &timer, Time now) {
    if (!(timer.config & DirectMode) && !(timer.config & SyntheticInterruptMask))
        timer.config &= ~Enable;
    timer.armed = (timer.config & Enable) && timer.count;
    if (timer.armed) {
        if (timer.config & Periodic) {
            timer.deadline = now + timer.count;
        } else
            timer.deadline = (now & ~Time(UINT64_MAX)) | timer.count;
    }
}
// Read one VP timer Config/Count; return Hyper-V errors for unknown register/VP.
// Microsoft Hyper-V TLFS; extended epochs, host storage/cache and counters are host policy.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#synthetic-timer-registers
uint16_t ArmHyperVTimers::read(uint32_t name, unsigned vp, uint64_t &value) const {
    value = 0;
    if (vp >= cpus)
        return InvalidVpIndex;
    if (name < Register::Timer0ConfigurationRegister || name > Register::Timer3CountRegister)
        return InvalidParameter;
    auto &timer = timers[vp][(name - Register::Timer0ConfigurationRegister) / RegistersPerTimer];
    value = (name & CountRegisterSelector) ? timer.count : timer.config;
    return Success;
}
// Apply Config/Count, auto-enable and rearm effects; invalidate deadline caches when needed.
// Microsoft Hyper-V TLFS; extended epochs, host storage/cache and counters are host policy.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#synthetic-timer-registers
uint16_t ArmHyperVTimers::write(uint32_t name, unsigned vp, uint64_t value, uint64_t now) {
    if (vp >= cpus)
        return InvalidVpIndex;
    if (name < Register::Timer0ConfigurationRegister || name > Register::Timer3CountRegister)
        return InvalidParameter;
    auto &timer = timers[vp][(name - Register::Timer0ConfigurationRegister) / RegistersPerTimer];
    const bool wasArmed = timer.armed;
    const auto oldDeadline = timer.deadline;
    const bool wasLazy = timer.config & Lazy;
    if (name & CountRegisterSelector) {
        ++writes.countWrites;
        writes.repeatedCountWrites += timer.count == value;
        writes.zeroCountWrites += value == 0;
        writes.autoEnableWrites += value && (timer.config & AutoEnable) && !(timer.config & Enable);
        timer.count = value;
        if (!value)
            timer.config &= ~Enable;
        else if (timer.config & AutoEnable)
            timer.config |= Enable;
    } else {
        // This ARM backend rejects the upper three bits of the x64 vector field.
        if (value & ~WritableConfiguration)
            return InvalidParameter;
        ++writes.configWrites;
        writes.repeatedConfigWrites += timer.config == value;
        ++writes.configFlags[value & ConfigurationFlags];
        timer.config = value;
    }
    arm(timer, observe(now));
    writes.periodicRearms += timer.armed && (timer.config & Periodic);
    writes.unchangedArmedDeadline += wasArmed && timer.armed && oldDeadline == timer.deadline;
    // Preserve all register/rearm effects. Only the derived minimum deadlines
    // can be reused when their armed/deadline/lazy inputs did not change.
    if (wasArmed != timer.armed ||
        (timer.armed && (oldDeadline != timer.deadline || wasLazy != bool(timer.config & Lazy))))
        deadlinesDirty = true;
    return Success;
}
// Attempt due deliveries, retain busy expirations and coalesce successful periodic catch-up.
// Microsoft Hyper-V TLFS; extended epochs, host storage/cache and counters are host policy.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#periodic-timers
void ArmHyperVTimers::service(uint64_t now) {
    ++services;
    const auto current = observe(now);
    refreshDeadlines();
    if (!earliest || current < *earliest) {
        ++fastReturns;
        return;
    }
    deadlinesDirty = true; // Expiration/retry/periodic advance can change minima.
    for (unsigned vp = 0; vp < cpus; ++vp)
        for (unsigned index = 0; index < TimersPerVirtualProcessor; ++index) {
            auto &timer = timers[vp][index];
            ++visits;
            if (!timer.armed || current < timer.deadline)
                continue;
            if (!(timer.config & Periodic))
                timer.config &= ~Enable;
            // A missing/busy backend retains this expiration for a later attempt.
            if (!delivery || !delivery(vp, index, timer.config, uint64_t(timer.deadline), now)) {
                ++retries;
                continue;
            }
            ++deliveries;
            if (!(timer.config & Periodic)) {
                timer.config &= ~Enable;
                timer.armed = false;
            } else {
                // Coalesce missed periods, retaining the original phase. One
                // notification per service pass prevents an unbounded catch-up.
                auto remainder = (current - timer.deadline) % timer.count;
                auto advance = timer.count - remainder;
                timer.deadline = current + advance;
            }
        }
}
// Rebuild cached all-timer/non-lazy minima only when their timer inputs changed.
// Microsoft Hyper-V TLFS; extended epochs, host storage/cache and counters are host policy.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#periodic-timers
void ArmHyperVTimers::refreshDeadlines() const {
    if (!deadlinesDirty)
        return;
    earliest.reset();
    earliestNonLazy.reset();
    ++rebuilds;
    for (unsigned vp = 0; vp < cpus; ++vp)
        for (const auto &timer : timers[vp]) {
            ++deadlineVisits;
            if (!timer.armed)
                continue;
            if (!earliest || timer.deadline < *earliest)
                earliest = timer.deadline;
            if (!(timer.config & Lazy) && (!earliestNonLazy || timer.deadline < *earliestNonLazy))
                earliestNonLazy = timer.deadline;
        }
    deadlinesDirty = false;
}
// Query the cached extended deadline, optionally excluding lazy timers.
// Microsoft Hyper-V TLFS; extended epochs, host storage/cache and counters are host policy.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#periodic-timers
std::optional<ArmHyperVTimers::Time> ArmHyperVTimers::deadline(bool includeLazy) const {
    ++queries;
    refreshDeadlines();
    return includeLazy ? earliest : earliestNonLazy;
}
// Return the low 64-bit absolute deadline or no deadline when no selected timers are armed.
// Microsoft Hyper-V TLFS; extended epochs, host storage/cache and counters are host policy.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#periodic-timers
std::optional<uint64_t> ArmHyperVTimers::nextDeadline(bool includeLazy) const {
    auto value = deadline(includeLazy);
    return value ? std::optional<uint64_t>(uint64_t(*value)) : std::nullopt;
}
// Return remaining 100 ns ticks, zero when due, or no delay when no timers are armed.
// Microsoft Hyper-V TLFS; extended epochs, host storage/cache and counters are host policy.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#periodic-timers
std::optional<uint64_t> ArmHyperVTimers::nextDelay(uint64_t now, bool includeLazy) const {
    auto value = deadline(includeLazy);
    if (!value)
        return {};
    auto current = project(now);
    return *value > current ? uint64_t(*value - current) : 0;
}
} // namespace pc
