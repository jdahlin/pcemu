#pragma once
#include "mouse.hpp"
#include <cstdint>
#include <deque>
#include <optional>
namespace pc {
class SnapshotCodec;
class Ps2TestAccess;
// AT PS/2 controller and attached keyboard/mouse. No CPU, PIC, BIOS or host
// frontend dependency: the board supplies time/gating and applies effects.
class I8042Device {
  public:
    struct State {
        bool keyLine = false, mouseLine = false, keyboardAux = false, keyboardFull = false;
        uint8_t keyboardData = 0;
        uint64_t keyboardNextAt = 0;
        uint8_t keyboardCommandByte = 0x45, keyboardControllerPending = 0, keyboardParameter = 0,
                keyboardLEDs = 0, keyboardScanSet = 2, keyboardOutputPort = 3;
        bool keyboardEnabled = true, keyboardLastCommand = false;
        std::deque<uint8_t> keyboardReplies, scanCodes;
        PS2Mouse mouse;
    };
    // Preserve the current board's delivery/backpressure policy without reading
    // PIC registers from inside the controller.
    struct IrqState { bool keyboardInService, keyboardMasked, mouseInService, mouseMasked; };
    struct Irqs { bool keyboard = false, mouse = false; };
    struct Effects { bool reset = false; std::optional<bool> a20; };
    struct Read { uint8_t value; Irqs irqs; bool full; };
    void reset() { state_ = State{}; }
    Irqs poll(uint64_t now, IrqState inputs);
    Read readStatus(uint64_t now, IrqState inputs);
    Read readData(uint64_t now, IrqState inputs);
    Effects command(uint8_t value, bool a20);
    Effects write(uint8_t value);
    void enqueueScan(uint8_t byte) { state_.scanCodes.push_back(byte); }
    void motion(int x, int y, uint8_t buttons, int wheel = 0) { state_.mouse.motion(x, y, buttons, wheel); }
    PS2Mouse &mouse() { return state_.mouse; }
    const PS2Mouse &mouse() const { return state_.mouse; }
    void enableAuxiliaryInterface() { state_.keyboardCommandByte = (state_.keyboardCommandByte & ~0x20) | 2; }
    const State &state() const { return state_; }
  private:
    friend class SnapshotCodec;
    friend class Ps2TestAccess;
    State state_;
};
} // namespace pc
