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
    // IBM PS/2 Hardware Interface Technical Reference (May 1988), Type 1
    // Controller: Data at 0060h, Command/Status at 0064h.
    // https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=334
    enum Register : uint16_t { DataRegister = 0x60, CommandRegister = 0x64, StatusRegister = 0x64 };
    // Controller command bytes; parameter-bearing commands consume the next Data write.
    // https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=340
    // https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=343
    enum ControllerCommand : uint8_t {
        ReadCommandByte = 0x20,
        WriteCommandByte = 0x60,
        DisableAuxiliaryInterface = 0xa7,
        EnableAuxiliaryInterface = 0xa8,
        TestAuxiliaryInterface = 0xa9,
        ControllerSelfTest = 0xaa,
        TestKeyboardInterface = 0xab,
        DisableKeyboardInterface = 0xad,
        EnableKeyboardInterface = 0xae,
        ReadInputPort = 0xc0,
        ReadOutputPort = 0xd0,
        WriteOutputPort = 0xd1,
        WriteKeyboardOutputBuffer = 0xd2,
        WriteAuxiliaryOutputBuffer = 0xd3,
        WriteAuxiliaryDevice = 0xd4,
    };
    // Controller Command byte: IRQ enables, system flag, interface inhibits,
    // and scan-set-2 to scan-set-1 translation request (stored here).
    // https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=340
    static constexpr uint8_t KeyboardInterruptEnable = 1, AuxiliaryInterruptEnable = 2,
                             SystemFlag = 4, KeyboardInterfaceDisable = 0x10,
                             AuxiliaryInterfaceDisable = 0x20, KeyboardTranslation = 0x40;
    // Status register: OBF[0], system[2], command/data[3], auxiliary output[5].
    // Input-full, inhibit, parity and timeout indications are not modeled.
    // https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=336
    static constexpr uint8_t OutputBufferFull = 1, LastWriteWasCommand = 8,
                             AuxiliaryOutputBufferFull = 0x20;
    // Output port bit 0 is RESET# (one deasserts), bit 1 is gate-A20.
    // F0h..FFh pulses each selected low output bit; zero bit 0 requests reset.
    // https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=339
    // https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=343
    static constexpr uint8_t ResetDeasserted = 1, GateAddress20 = 2, PulseOutputCommandMask = 0xf0;
    // Controller test replies: 55h self-test success, 00h interface-line test success.
    static constexpr uint8_t ControllerTestPassed = 0x55, InterfaceTestPassed = 0;
    // COMPATIBILITY: C0 returns an AT-style unlocked-keyboard input indication,
    // not the PS/2 Type 1 input-port data-line image described in the reference.
    static constexpr uint8_t AtKeyboardUnlockedInput = 0x80;
    // Attached 101/102-key keyboard commands. F7h..FDh key-type commands are ACK-only.
    // https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=530
    enum KeyboardCommand : uint8_t {
        SetStatusIndicators = 0xed,
        Echo = 0xee,
        SelectScanCodeSet = 0xf0,
        ReadKeyboardId = 0xf2,
        SetTypematicRateDelay = 0xf3,
        EnableScanning = 0xf4,
        DefaultDisable = 0xf5,
        SetKeyboardDefault = 0xf6,
        SetAllKeysTypematic = 0xf7,
        ResendKeyboardByte = 0xfe,
        ResetKeyboard = 0xff,
    };
    static constexpr uint8_t KeyboardAcknowledge = 0xfa, KeyboardResend = 0xfe,
                             KeyboardSelfTestPassed = 0xaa;
    // MF2 keyboard identity is sent ABh then 83h; code combines the wire bytes
    // for a named identity constant, not a little-endian register value.
    static constexpr uint16_t KeyboardIdentification = 0xab83;
    // LED parameter bits [2:0] are Caps Lock, Num Lock and Scroll Lock.
    // https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=533
    static constexpr uint8_t StatusIndicatorMask = 7, DefaultScanCodeSet = 2,
                             MaximumScanCodeSet = 3;
    // IMPLEMENTATION NOTE: Board starts after firmware setup: keyboard IRQ and
    // system/translation flags set (45h), reset deasserted and A20 enabled (03h).
    static constexpr uint8_t InitialCommandByte =
                                 KeyboardInterruptEnable | SystemFlag | KeyboardTranslation,
                             InitialOutputPort = ResetDeasserted | GateAddress20;
    // IMPLEMENTATION NOTE: Backpressure delay measured in instruction ticks, not
    // the physical PS/2 serial clock rate or a hardware-specified response delay.
    static constexpr uint64_t OutputReadSpacingTicks = 4000;
    struct State {
        bool keyLine = false, mouseLine = false, keyboardAux = false, keyboardFull = false;
        uint8_t keyboardData = 0;
        uint64_t keyboardNextAt = 0;
        uint8_t keyboardCommandByte = InitialCommandByte, keyboardControllerPending = 0,
                keyboardParameter = 0, keyboardLEDs = 0, keyboardScanSet = DefaultScanCodeSet,
                keyboardOutputPort = InitialOutputPort;
        bool keyboardEnabled = true, keyboardLastCommand = false;
        std::deque<uint8_t> keyboardReplies, scanCodes;
        PS2Mouse mouse;
    };
    // Preserve the current board's delivery/backpressure policy without reading
    // PIC registers from inside the controller.
    struct IrqState {
        bool keyboardInService, keyboardMasked, mouseInService, mouseMasked;
    };
    struct Irqs {
        bool keyboard = false, mouse = false;
    };
    struct Effects {
        bool reset = false;
        std::optional<bool> a20;
    };
    struct Read {
        uint8_t value;
        Irqs irqs;
        bool full;
    };
    // Restore firmware-initialized controller/keyboard/mouse state and clear all queues.
    // IMPLEMENTATION NOTE: Host lifecycle/input access around the IBM controller interface.
    // https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=338
    void reset() {
        state_ = State{};
    }
    // Conservative board-side gate: false means poll cannot change state or
    // raise an IRQ. Read live state so I/O, input and restore need no cache.
    // Conservatively report whether polling can change output/IRQ state using live
    // queues/deadlines. IMPLEMENTATION NOTE: Host lifecycle/input access around the IBM controller
    // interface.
    // https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=338
    bool poll_due(uint64_t now) const {
        if (state_.mouse.packet_due(now))
            return true;
        if (state_.keyboardFull)
            return state_.keyboardAux
                       ? (state_.keyboardCommandByte & AuxiliaryInterruptEnable) &&
                             !state_.mouseLine
                       : (state_.keyboardCommandByte & KeyboardInterruptEnable) && !state_.keyLine;
        return now >= state_.keyboardNextAt &&
               (!state_.keyboardReplies.empty() || !state_.mouse.replies.empty() ||
                !state_.scanCodes.empty() || !state_.mouse.packets.empty());
    }
    // Service output priority/backpressure and return newly asserted keyboard/mouse IRQs.
    Irqs poll_output_buffer(uint64_t now, IrqState inputs);
    // Service pending input and read status bits plus newly asserted IRQs.
    Read read_status_register(uint64_t now, IrqState inputs);
    // Service/read the output latch, acknowledge its IRQ and apply host read spacing.
    Read read_data_register(uint64_t now, IrqState inputs);
    // Decode a Controller Command write and return board reset/A20 effects.
    Effects write_command_register(uint8_t value, bool a20);
    // Apply a pending controller parameter or attached-keyboard command/data byte.
    Effects write_data_register(uint8_t value);
    // Queue a host-translated scan byte for later output-buffer delivery.
    // IMPLEMENTATION NOTE: Host lifecycle/input access around the IBM controller interface.
    // https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=338
    void enqueue_scan_code(uint8_t byte) {
        state_.scanCodes.push_back(byte);
    }
    // Forward host relative counts/buttons/wheel to the attached mouse.
    // IMPLEMENTATION NOTE: Frontend input is already expressed as PS/2 motion counts.
    // https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=334
    void accumulate_motion(int x, int y, uint8_t buttons, int wheel = 0) {
        state_.mouse.accumulate_motion(x, y, buttons, wheel);
    }
    // Expose attached mouse state for board BIOS wiring and inspection.
    // IMPLEMENTATION NOTE: Host lifecycle/input access around the IBM controller interface.
    // https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=338
    PS2Mouse &mouse() {
        return state_.mouse;
    }
    // Expose attached mouse state for board BIOS wiring and inspection.
    // IMPLEMENTATION NOTE: Host lifecycle/input access around the IBM controller interface.
    // https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=338
    const PS2Mouse &mouse() const {
        return state_.mouse;
    }
    // Enable auxiliary IRQ/interface directly for the board BIOS helper.
    // IMPLEMENTATION NOTE: Host lifecycle/input access around the IBM controller interface.
    // https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=338
    void enable_auxiliary_interface() {
        state_.keyboardCommandByte =
            (state_.keyboardCommandByte & ~AuxiliaryInterfaceDisable) | AuxiliaryInterruptEnable;
    }
    // Inspect serialized controller/keyboard/mouse state without mutation.
    // IMPLEMENTATION NOTE: Host lifecycle/input access around the IBM controller interface.
    // https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=338
    const State &state() const {
        return state_;
    }

  private:
    friend class SnapshotCodec;
    friend class Ps2TestAccess;
    State state_;
};
} // namespace pc
