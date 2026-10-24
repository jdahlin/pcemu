#pragma once
#include "pic.hpp"
#include <array>
#include <cstddef>
#include <optional>
namespace pc {
// PC/AT's cascaded pair of 8259A devices. CPU readiness is supplied by the
// board; priority, cascade, acknowledge and compatibility mirrors live here.
class I8259PairDevice {
  public:
    struct State {
        std::array<PIC8259, 2> chips{};
        bool cascadeLine = false, timerPending = false, timerInService = false,
             keyboardInService = false, ataInService = false;
        uint8_t picBase[2]{8, 0x70}, picInit[2]{};
    };
    void reset() { state_ = State{}; }
    PIC8259 &operator[](size_t index) { return state_.chips[index]; }
    const PIC8259 &operator[](size_t index) const { return state_.chips[index]; }
    auto begin() { return state_.chips.begin(); }
    auto end() { return state_.chips.end(); }
    auto begin() const { return state_.chips.begin(); }
    auto end() const { return state_.chips.end(); }
    const State &state() const { return state_; }
    void raise(unsigned irq) { state_.chips[irq >= 8].raise(irq & 7); }
    uint8_t read(unsigned chip, bool data);
    void write(unsigned chip, bool data, uint8_t value);
    std::optional<uint8_t> nextVector(bool cpuReady);
  private:
    friend class SnapshotCodec;
    State state_;
    void updateMirrors();
};
} // namespace pc
