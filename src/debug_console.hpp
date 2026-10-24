#pragma once
#include <cstdint>
#include <functional>
#include <utility>
namespace pc {
// Byte debug port. Its host output sink is runtime wiring, not guest state.
class DebugConsoleDevice {
  public:
    using Output = std::function<void(uint8_t)>;
    void bind(Output output) { output_ = std::move(output); }
    void reset() { latch_ = 0; }
    void restore(uint8_t value) { latch_ = value; }
    uint8_t read() const { return latch_; }
    void write(uint8_t value) { latch_ = value; if (output_) output_(value); }
  private:
    uint8_t latch_ = 0;
    Output output_;
};
} // namespace pc
