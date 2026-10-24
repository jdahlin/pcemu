#pragma once
#include <cstdint>
#include <functional>
namespace pc {
// PL031 RTC. Guest writes adjust only this VM's clock, never the host clock.
// Host wall time includes sleep; the callback also permits deterministic tests.
class ArmPl031 {
  public:
    explicit ArmPl031(std::function<int64_t()> seconds = {});
    uint32_t read(unsigned offset);
    void write(unsigned offset, uint32_t value);
    bool irq();
  private:
    std::function<int64_t()> seconds;
    int64_t adjustment = 0, previous = 0;
    uint32_t match = 0, loaded = 0;
    bool masked = false, pending = false;
    int64_t update();
};
} // namespace pc
