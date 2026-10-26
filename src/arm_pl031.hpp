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
    // Bind a seconds source; the default is host wall time, including sleep.
    explicit ArmPl031(std::function<int64_t()> host_seconds = {});
    // Read a register from the programmers model (3.2).
    uint32_t read_register(unsigned offset);
    // Write a register from the programmers model (3.2).
    void write_register(unsigned offset, uint32_t value);
    // Sample the active-high RTCINTR output (3.6).
    bool rtcintr_asserted();

  private:
    friend struct ArmSnapshotAccess; // Host checkpoint fields; never serialize host bindings.
    std::function<int64_t()> host_seconds;
    int64_t clock_adjustment = 0, previous_counter = 0;
    uint32_t match_register = 0, load_register = 0;
    bool interrupt_enabled = false, raw_interrupt = false;
    // Advance the RTC value using the Update block and Counter block model.
    int64_t update_rtc_value();
    // Program RTCMR, the Match Register (3.3.2).
    void write_match_register(uint32_t value);
    // Apply RTCLR through the Update block (3.3.3).
    void write_load_register(uint32_t value);
    // Program RTCIMSC, the Interrupt Mask Set or Clear register (3.3.5).
    void write_interrupt_mask(uint32_t value);
    // Acknowledge an interrupt through RTCICR (3.3.8).
    void clear_interrupt(uint32_t value);
};
} // namespace pc
