#include "arm_hyperv_timers.hpp"
#include <algorithm>
#include <stdexcept>
namespace pc {
ArmHyperVTimers::ArmHyperVTimers(unsigned count) : cpus(count) {
    if (!count || count > timers.size()) throw std::runtime_error("Invalid synthetic timer VP count");
}
void ArmHyperVTimers::arm(Timer &timer, uint64_t now) {
    if (!(timer.config & 0x1000) && !(timer.config & 0xf0000)) timer.config &= ~uint64_t(1);
    timer.armed = (timer.config & 1) && timer.count;
    if (timer.armed) {
        if (timer.config & 2) {
            // Never shorten an unrepresentable future interval to UINT64_MAX.
            // Counter-rollover scheduling requires a separate extended epoch.
            if (now > UINT64_MAX - timer.count) { timer.armed = false; return; }
            timer.deadline = now + timer.count;
        } else timer.deadline = timer.count;
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
    if (name & 1) {
        timer.count = value;
        if (!value) timer.config &= ~uint64_t(1);
        else if (timer.config & 8) timer.config |= 1;
    } else {
        // ARM64 reserves the upper three bits of the eight-bit vector field.
        if (value & ~uint64_t(0xf11ff)) return 5;
        timer.config = value;
    }
    arm(timer, now);
    return 0;
}
void ArmHyperVTimers::service(uint64_t now) {
    for (unsigned vp = 0; vp < cpus; ++vp) for (unsigned index = 0; index < 4; ++index) {
        auto &timer = timers[vp][index];
        if (!timer.armed || now < timer.deadline) continue;
        if (!(timer.config & 2)) timer.config &= ~uint64_t(1);
        // A missing/busy backend retains this expiration for a later attempt.
        if (!delivery || !delivery(vp, index, timer.config, timer.deadline, now)) continue;
        ++deliveries;
        if (!(timer.config & 2)) { timer.config &= ~uint64_t(1); timer.armed = false; }
        else {
            // Coalesce missed periods, retaining the original phase. One
            // notification per service pass prevents an unbounded catch-up.
            auto remainder = (now - timer.deadline) % timer.count;
            auto advance = timer.count - remainder;
            if (now > UINT64_MAX - advance) timer.armed = false;
            else timer.deadline = now + advance;
        }
    }
}
std::optional<uint64_t> ArmHyperVTimers::nextDeadline(bool includeLazy) const {
    std::optional<uint64_t> deadline;
    for (unsigned vp = 0; vp < cpus; ++vp) for (auto &timer : timers[vp])
        if (timer.armed && (includeLazy || !(timer.config & 4)) && (!deadline || timer.deadline < *deadline)) deadline = timer.deadline;
    return deadline;
}
}
