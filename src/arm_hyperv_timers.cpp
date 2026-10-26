#include "arm_hyperv_timers.hpp"
#include <algorithm>
#include <stdexcept>
namespace pc {
ArmHyperVTimers::ArmHyperVTimers(unsigned count) : cpus(count) {
    if (!count || count > timers.size()) throw std::runtime_error("Invalid synthetic timer VP count");
}
ArmHyperVTimers::Time ArmHyperVTimers::project(uint64_t now) const {
    auto epoch = lastSample & ~Time(UINT64_MAX);
    if (sampled && now < uint64_t(lastSample)) epoch += Time(1) << 64;
    return epoch | now;
}
ArmHyperVTimers::Time ArmHyperVTimers::observe(uint64_t now) {
    lastSample = project(now); sampled = true;
    return lastSample;
}
void ArmHyperVTimers::arm(Timer &timer, Time now) {
    if (!(timer.config & 0x1000) && !(timer.config & 0xf0000)) timer.config &= ~uint64_t(1);
    timer.armed = (timer.config & 1) && timer.count;
    if (timer.armed) {
        if (timer.config & 2) {
            timer.deadline = now + timer.count;
        } else timer.deadline = (now & ~Time(UINT64_MAX)) | timer.count;
    }
}
uint16_t ArmHyperVTimers::read(uint32_t name, unsigned vp, uint64_t &value) const {
    value = 0;
    if (vp >= cpus) return 14;
    if (name < 0xb0000 || name > 0xb0007) return 5;
    auto &timer = timers[vp][(name - 0xb0000) / 2];
    value = (name & 1) ? timer.count : timer.config;
    return 0;
}
uint16_t ArmHyperVTimers::write(uint32_t name, unsigned vp, uint64_t value, uint64_t now) {
    if (vp >= cpus) return 14;
    if (name < 0xb0000 || name > 0xb0007) return 5;
    auto &timer = timers[vp][(name - 0xb0000) / 2];
    const bool wasArmed = timer.armed;
    const auto oldDeadline = timer.deadline;
    const bool wasLazy = timer.config & 4;
    if (name & 1) {
        ++writes.countWrites;
        writes.repeatedCountWrites += timer.count == value;
        writes.zeroCountWrites += value == 0;
        writes.autoEnableWrites += value && (timer.config & 8) && !(timer.config & 1);
        timer.count = value;
        if (!value) timer.config &= ~uint64_t(1);
        else if (timer.config & 8) timer.config |= 1;
    } else {
        // ARM64 reserves the upper three bits of the eight-bit vector field.
        if (value & ~uint64_t(0xf11ff)) return 5;
        ++writes.configWrites;
        writes.repeatedConfigWrites += timer.config == value;
        ++writes.configFlags[value & 15];
        timer.config = value;
    }
    arm(timer, observe(now));
    writes.periodicRearms += timer.armed && (timer.config & 2);
    writes.unchangedArmedDeadline += wasArmed && timer.armed && oldDeadline == timer.deadline;
    // Preserve all register/rearm effects. Only the derived minimum deadlines
    // can be reused when their armed/deadline/lazy inputs did not change.
    if (wasArmed != timer.armed || (timer.armed &&
        (oldDeadline != timer.deadline || wasLazy != bool(timer.config & 4))))
        deadlinesDirty = true;
    return 0;
}
void ArmHyperVTimers::service(uint64_t now) {
    ++services;
    const auto current = observe(now);
    refreshDeadlines();
    if (!earliest || current < *earliest) { ++fastReturns; return; }
    deadlinesDirty = true; // Expiration/retry/periodic advance can change minima.
    for (unsigned vp = 0; vp < cpus; ++vp) for (unsigned index = 0; index < 4; ++index) {
        auto &timer = timers[vp][index];
        ++visits;
        if (!timer.armed || current < timer.deadline) continue;
        if (!(timer.config & 2)) timer.config &= ~uint64_t(1);
        // A missing/busy backend retains this expiration for a later attempt.
        if (!delivery || !delivery(vp, index, timer.config, uint64_t(timer.deadline), now)) { ++retries; continue; }
        ++deliveries;
        if (!(timer.config & 2)) { timer.config &= ~uint64_t(1); timer.armed = false; }
        else {
            // Coalesce missed periods, retaining the original phase. One
            // notification per service pass prevents an unbounded catch-up.
            auto remainder = (current - timer.deadline) % timer.count;
            auto advance = timer.count - remainder;
            timer.deadline = current + advance;
        }
    }
}
void ArmHyperVTimers::refreshDeadlines() const {
    if (!deadlinesDirty) return;
    earliest.reset(); earliestNonLazy.reset();
    ++rebuilds;
    for (unsigned vp = 0; vp < cpus; ++vp) for (const auto &timer : timers[vp]) {
        ++deadlineVisits;
        if (!timer.armed) continue;
        if (!earliest || timer.deadline < *earliest) earliest = timer.deadline;
        if (!(timer.config & 4) && (!earliestNonLazy || timer.deadline < *earliestNonLazy))
            earliestNonLazy = timer.deadline;
    }
    deadlinesDirty = false;
}
std::optional<ArmHyperVTimers::Time> ArmHyperVTimers::deadline(bool includeLazy) const {
    ++queries;
    refreshDeadlines();
    return includeLazy ? earliest : earliestNonLazy;
}
std::optional<uint64_t> ArmHyperVTimers::nextDeadline(bool includeLazy) const {
    auto value = deadline(includeLazy);
    return value ? std::optional<uint64_t>(uint64_t(*value)) : std::nullopt;
}
std::optional<uint64_t> ArmHyperVTimers::nextDelay(uint64_t now, bool includeLazy) const {
    auto value = deadline(includeLazy);
    if (!value) return {};
    auto current = project(now);
    return *value > current ? uint64_t(*value - current) : 0;
}
}
