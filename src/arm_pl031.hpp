#pragma once
#include <cstdint>
#include <functional>
namespace pc {
// PL031 RTC, using ARM DDI 0224C (r1p3) as the reference:
// https://documentation-service.arm.com/static/5fa13c81b1a7c5445f29021e
// See docs/device-specification-style.md for scope and known deviations.
// AD-HOC: Guest writes adjust only this VM's clock, never the host clock.
// Host wall time includes sleep; the callback also permits deterministic tests.
class ArmPl031 {
  public:
    explicit ArmPl031(std::function<int64_t()> host_seconds = {});
    uint32_t read(unsigned offset);
    void write(unsigned offset, uint32_t value);
    bool irq();

  private:
    std::function<int64_t()> host_seconds;
    int64_t clock_adjustment = 0, previous_counter = 0;
    uint32_t match_register = 0, load_register = 0;
    bool interrupt_enabled = false, raw_interrupt = false;
    int64_t update_counter();
};
} // namespace pc
