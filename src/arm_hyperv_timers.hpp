#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
namespace pc {
// Four synthetic timers per VP, in partition reference-counter (100 ns) units.
// All service and register operations run with every guest VP stopped.
class ArmHyperVTimers {
  public:
    using Deliver = std::function<bool(unsigned vp, unsigned timer, uint64_t config,
                                       uint64_t expiration, uint64_t delivery)>;
    // Create timer storage for the configured VP count.
    explicit ArmHyperVTimers(unsigned cpus);
    // Bind host interrupt/message delivery; false keeps an expiration pending.
    // IMPLEMENTATION NOTE: Host delivery/diagnostics around TLFS synthetic timers.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#synthetic-timers
    void setDelivery(Deliver deliver) {
        delivery = std::move(deliver);
    }
    // Read one VP timer Config/Count; return Hyper-V errors for unknown register/VP.
    uint16_t read(uint32_t name, unsigned vp, uint64_t &value) const;
    // Apply Config/Count, auto-enable and rearm effects; invalidate deadline caches when needed.
    uint16_t write(uint32_t name, unsigned vp, uint64_t value, uint64_t now);
    // Attempt due deliveries, retain busy expirations and coalesce successful periodic catch-up.
    void service(uint64_t now);
    // Return the low 64-bit absolute deadline or no deadline when no selected timers are armed.
    std::optional<uint64_t> nextDeadline(bool includeLazy = true) const;
    // Return remaining 100 ns ticks, zero when due, or no delay when no timers are armed.
    std::optional<uint64_t> nextDelay(uint64_t now, bool includeLazy = true) const;
    // Inspect the number of successful backend deliveries.
    // IMPLEMENTATION NOTE: Host delivery/diagnostics around TLFS synthetic timers.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#synthetic-timers
    uint64_t delivered() const {
        return deliveries;
    }
    // The four low configuration flags produce sixteen host accounting buckets.
    static constexpr unsigned ConfigurationFlagCombinations = 16;
    struct Activity {
        uint64_t configWrites = 0, countWrites = 0;
        uint64_t repeatedConfigWrites = 0, repeatedCountWrites = 0;
        uint64_t zeroCountWrites = 0, autoEnableWrites = 0;
        uint64_t periodicRearms = 0, unchangedArmedDeadline = 0;
        std::array<uint64_t, ConfigurationFlagCombinations> configFlags{};
    };
    // Inspect host-only register write/rearm accounting.
    // IMPLEMENTATION NOTE: Host delivery/diagnostics around TLFS synthetic timers.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#synthetic-timers
    const Activity &activity() const {
        return writes;
    }
    // Inspect the number of host service passes.
    // IMPLEMENTATION NOTE: Host delivery/diagnostics around TLFS synthetic timers.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#synthetic-timers
    uint64_t serviceCalls() const {
        return services;
    }
    // Inspect individual timer visits during due-delivery scanning.
    // IMPLEMENTATION NOTE: Host delivery/diagnostics around TLFS synthetic timers.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#synthetic-timers
    uint64_t serviceTimerVisits() const {
        return visits;
    }
    // Inspect deadline/delay queries.
    // IMPLEMENTATION NOTE: Host delivery/diagnostics around TLFS synthetic timers.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#synthetic-timers
    uint64_t deadlineQueries() const {
        return queries;
    }
    // Inspect individual timer visits during cached-minimum rebuilding.
    // IMPLEMENTATION NOTE: Host delivery/diagnostics around TLFS synthetic timers.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#synthetic-timers
    uint64_t deadlineTimerVisits() const {
        return deadlineVisits;
    }
    // Inspect attempts deferred by a missing/busy backend.
    // IMPLEMENTATION NOTE: Host delivery/diagnostics around TLFS synthetic timers.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#synthetic-timers
    uint64_t deliveryRetries() const {
        return retries;
    }
    // Inspect minimum-deadline cache rebuilds.
    // IMPLEMENTATION NOTE: Host delivery/diagnostics around TLFS synthetic timers.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#synthetic-timers
    uint64_t deadlineRebuilds() const {
        return rebuilds;
    }
    // Inspect service passes skipped because the cached deadline is not yet due.
    // IMPLEMENTATION NOTE: Host delivery/diagnostics around TLFS synthetic timers.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#synthetic-timers
    uint64_t serviceFastReturns() const {
        return fastReturns;
    }

  private:
    // Four timers per VP are architectural; eight VPs are this backend's storage bound.
    static constexpr unsigned TimersPerVirtualProcessor = 4, MaximumVirtualProcessors = 8;
    using Time = __uint128_t;
    struct Timer {
        uint64_t config = 0, count = 0;
        Time deadline = 0;
        bool armed = false;
    };
    Time lastSample = 0;
    bool sampled = false;
    // Map a wire counter sample into the current extended wrap epoch without changing state.
    Time project(uint64_t now) const;
    // Record an ordered reference-time sample and extend its wrap epoch.
    Time observe(uint64_t now);
    // Query the cached extended deadline, optionally excluding lazy timers.
    std::optional<Time> deadline(bool includeLazy) const;
    // Rebuild cached all-timer/non-lazy minima only when their timer inputs changed.
    void refreshDeadlines() const;
    mutable bool deadlinesDirty = true;
    mutable std::optional<Time> earliest, earliestNonLazy;
    std::array<std::array<Timer, TimersPerVirtualProcessor>, MaximumVirtualProcessors> timers;
    unsigned cpus;
    Deliver delivery;
    uint64_t deliveries = 0;
    Activity writes;
    uint64_t services = 0, visits = 0, retries = 0, fastReturns = 0;
    mutable uint64_t queries = 0, deadlineVisits = 0, rebuilds = 0;
    // Apply Enable/Count/Periodic rules and derive an absolute internal expiration.
    static void arm(Timer &timer, Time now);
};
} // namespace pc
