// The i8042-style controller is the PC's keyboard/auxiliary byte gateway. Port
// 60h carries device data and command parameters; port 64h writes controller
// commands and reads status. A single output latch tags bytes as keyboard or
// auxiliary data and signals IRQ1 or IRQ12 when enabled. The attached mouse owns
// its protocol; this component also supplies a minimal 101/102-key keyboard
// command responder. Host input supplies scan bytes; the board applies A20/reset
// effects and routes newly asserted IRQs into its interrupt controllers.
//
// Controller Command byte, input/output ports and keyboard device commands are
// different interfaces even when byte values overlap. Data writes first satisfy
// a pending controller command, then a keyboard parameter, otherwise a keyboard
// command. Replies outrank injected scan codes and mouse motion packets. Reading
// the output latch releases its IRQ; Status/Data reads may first make queued input
// available. An empty Data read retains the last latched byte for compatibility.
//
// IMPLEMENTATION NOTE: Board PIC masking/in-service state backpressures injected
// scan/motion streams; command replies bypass that gate. A 4000-instruction read
// spacing prevents flooding, not a physical PS/2 timing model. Startup represents
// firmware-initialized state. Real parity/timeouts, passwords, electrical clocks,
// full controller RAM and diagnostics are not implemented; tests report success.
// COMPATIBILITY: Host scan codes are already translated; requested keyboard scan
// sets/translation and typematic/key-type controls do not re-encode or repeat them.
// Input-port command C0 returns an AT-style 80h value rather than PS/2 Type 1 data
// lines. Unknown F7h..FDh key-type commands are acknowledged without applying them.
// FIXME: Implement those documented command/status effects before claiming full
// controller/keyboard conformance. Preserve current behavior during this migration.
// IBM PS/2 Hardware Interface Technical Reference, May 1988: Type 1 Controller
// (printed pages 1-11) and 101/102-Key Keyboard commands (printed pages 21-27).
// https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=334
// https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=530

#include "i8042.hpp"
namespace pc {
// Service output priority/backpressure and return newly asserted keyboard/mouse IRQs.
// IBM Type 1 Controller; board gating/spacing and keyboard compatibility are described above.
// https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=338
I8042Device::Irqs I8042Device::poll_output_buffer(uint64_t now, IrqState inputs) {
    Irqs result;
    // No output-buffer/IRQ work is possible until queued input is ready or
    // the mouse sample deadline arrives. Inspect live state so guest I/O,
    // host input and restored snapshots need no cached-deadline invalidation.
    if (!state_.keyboardFull && !state_.mouse.packet_due(now) &&
        (now < state_.keyboardNextAt ||
         (state_.keyboardReplies.empty() && state_.mouse.replies.empty() &&
          state_.scanCodes.empty() && state_.mouse.packets.empty())))
        return result;
    state_.mouse.poll_due_report(now);
    // Backpressure the injected scan stream until the current IRQ service ends.
    // Command replies can still arrive while a handler is waiting for an ACK.
    if (!state_.keyboardFull && now >= state_.keyboardNextAt) {
        std::deque<uint8_t> *queue = nullptr;
        bool auxiliary = false;
        if (!state_.keyboardReplies.empty())
            queue = &state_.keyboardReplies;
        else if (!state_.mouse.replies.empty()) {
            queue = &state_.mouse.replies;
            auxiliary = true;
        } else if (state_.keyboardEnabled && !state_.keyboardParameter &&
                   !(state_.keyboardCommandByte & KeyboardInterfaceDisable) &&
                   !inputs.keyboardInService &&
                   (!(state_.keyboardCommandByte & KeyboardInterruptEnable) ||
                    !inputs.keyboardMasked) &&
                   !state_.scanCodes.empty())
            queue = &state_.scanCodes;
        else if (!(state_.keyboardCommandByte & AuxiliaryInterfaceDisable) &&
                 !inputs.mouseInService &&
                 (!(state_.keyboardCommandByte & AuxiliaryInterruptEnable) ||
                  !inputs.mouseMasked) &&
                 !state_.mouse.packets.empty()) {
            queue = &state_.mouse.packets;
            auxiliary = true;
        }
        if (queue) {
            state_.keyboardData = queue->front();
            queue->pop_front();
            state_.keyboardAux = auxiliary;
            state_.keyboardFull = true;
        }
    }
    if (state_.keyboardFull && state_.keyboardAux &&
        (state_.keyboardCommandByte & AuxiliaryInterruptEnable) && !state_.mouseLine) {
        result.mouse = true;
        state_.mouseLine = true;
    } else if (state_.keyboardFull && !state_.keyboardAux &&
               (state_.keyboardCommandByte & KeyboardInterruptEnable) && !state_.keyLine) {
        result.keyboard = true;
        state_.keyLine = true;
    }
    return result;
}
// Decode a Controller Command write and return board reset/A20 effects.
// IBM Type 1 Controller; board gating/spacing and keyboard compatibility are described above.
// https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=340
I8042Device::Effects I8042Device::write_command_register(uint8_t value, bool a20) {
    Effects result;
    state_.keyboardLastCommand = true;
    state_.keyboardControllerPending = 0;
    // AT controller F0..FF pulse low the output bits whose command bits are
    // zero. Output bit 0 is RESET#, so every even pulse command resets the CPU.
    if ((value & PulseOutputCommandMask) == PulseOutputCommandMask && !(value & ResetDeasserted)) {
        result.reset = true;
        return result;
    }
    switch (value) {
    case ControllerCommand::ReadCommandByte:
        state_.keyboardReplies.push_back(state_.keyboardCommandByte);
        break;
    case ControllerCommand::WriteCommandByte:
    case ControllerCommand::WriteOutputPort:
    case ControllerCommand::WriteKeyboardOutputBuffer:
    case ControllerCommand::WriteAuxiliaryOutputBuffer:
    case ControllerCommand::WriteAuxiliaryDevice:
        state_.keyboardControllerPending = value;
        break;
    case ControllerCommand::ControllerSelfTest:
        state_.keyboardCommandByte |= SystemFlag;
        state_.keyboardReplies.push_back(ControllerTestPassed);
        break;
    case ControllerCommand::TestKeyboardInterface:
        state_.keyboardReplies.push_back(InterfaceTestPassed);
        break;
    case ControllerCommand::DisableKeyboardInterface:
        state_.keyboardCommandByte |= KeyboardInterfaceDisable;
        break;
    case ControllerCommand::EnableKeyboardInterface:
        state_.keyboardCommandByte &= ~KeyboardInterfaceDisable;
        break;
    case ControllerCommand::DisableAuxiliaryInterface:
        state_.keyboardCommandByte |= AuxiliaryInterfaceDisable;
        break;
    case ControllerCommand::EnableAuxiliaryInterface:
        state_.keyboardCommandByte &= ~AuxiliaryInterfaceDisable;
        break;
    case ControllerCommand::TestAuxiliaryInterface:
        state_.keyboardReplies.push_back(InterfaceTestPassed);
        break;
    case ControllerCommand::ReadInputPort:
        state_.keyboardReplies.push_back(AtKeyboardUnlockedInput);
        break;
    case ControllerCommand::ReadOutputPort:
        state_.keyboardReplies.push_back((state_.keyboardOutputPort & ~GateAddress20) |
                                         (a20 ? GateAddress20 : 0));
        break;
    default:
        break;
    }
    return result;
}
// Apply a pending controller parameter or attached-keyboard command/data byte.
// IBM Type 1 Controller; board gating/spacing and keyboard compatibility are described above.
// https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=343
I8042Device::Effects I8042Device::write_data_register(uint8_t value) {
    Effects result;
    state_.keyboardLastCommand = false;
    if (state_.keyboardControllerPending) {
        auto command = state_.keyboardControllerPending;
        state_.keyboardControllerPending = 0;
        if (command == ControllerCommand::WriteCommandByte) {
            state_.keyboardCommandByte = value;
            if (!(value & KeyboardInterruptEnable))
                state_.keyLine = false;
            if (!(value & AuxiliaryInterruptEnable))
                state_.mouseLine = false;
        } else if (command == ControllerCommand::WriteOutputPort) {
            state_.keyboardOutputPort = value;
            result.a20 = bool(value & GateAddress20);
            if (!(value & ResetDeasserted))
                result.reset = true;
        } else if (command == ControllerCommand::WriteKeyboardOutputBuffer)
            state_.keyboardReplies.push_back(value);
        else if (command == ControllerCommand::WriteAuxiliaryOutputBuffer)
            state_.mouse.replies.push_back(value);
        else if (command == ControllerCommand::WriteAuxiliaryDevice)
            state_.mouse.write_command(value);
        return result;
    }
    if (state_.keyboardParameter) {
        auto parameter = state_.keyboardParameter;
        state_.keyboardParameter = 0;
        state_.keyboardReplies.push_back(KeyboardAcknowledge);
        if (parameter == KeyboardCommand::SetStatusIndicators)
            state_.keyboardLEDs = value & StatusIndicatorMask;
        if (parameter == KeyboardCommand::SelectScanCodeSet) {
            if (!value)
                state_.keyboardReplies.push_back(state_.keyboardScanSet);
            else if (value <= MaximumScanCodeSet)
                state_.keyboardScanSet = value;
        }
        return result;
    }
    switch (value) {
    case KeyboardCommand::SetStatusIndicators:
    case KeyboardCommand::SelectScanCodeSet:
    case KeyboardCommand::SetTypematicRateDelay:
        state_.keyboardParameter = value;
        state_.keyboardReplies.push_back(KeyboardAcknowledge);
        break;
    case KeyboardCommand::Echo:
        state_.keyboardReplies.push_back(KeyboardCommand::Echo);
        break;
    case KeyboardCommand::ReadKeyboardId:
        state_.keyboardReplies.insert(state_.keyboardReplies.end(),
                                      {KeyboardAcknowledge, uint8_t(KeyboardIdentification >> 8),
                                       uint8_t(KeyboardIdentification)});
        break;
    case KeyboardCommand::EnableScanning:
        state_.keyboardEnabled = true;
        state_.keyboardReplies.push_back(KeyboardAcknowledge);
        break;
    case KeyboardCommand::DefaultDisable:
        state_.keyboardEnabled = false;
        state_.keyboardLEDs = 0;
        state_.keyboardScanSet = DefaultScanCodeSet;
        state_.keyboardReplies.push_back(KeyboardAcknowledge);
        break;
    case KeyboardCommand::SetKeyboardDefault:
        state_.keyboardLEDs = 0;
        state_.keyboardScanSet = DefaultScanCodeSet;
        state_.keyboardReplies.push_back(KeyboardAcknowledge);
        break;
    case KeyboardCommand::ResetKeyboard:
        state_.keyboardLEDs = 0;
        state_.keyboardScanSet = DefaultScanCodeSet;
        state_.keyboardEnabled = true;
        state_.keyboardReplies.insert(state_.keyboardReplies.end(),
                                      {KeyboardAcknowledge, KeyboardSelfTestPassed});
        break;
    case KeyboardCommand::ResendKeyboardByte:
        state_.keyboardReplies.push_back(state_.keyboardData);
        break;
    default:
        state_.keyboardReplies.push_back(
            value >= KeyboardCommand::SetAllKeysTypematic ? KeyboardAcknowledge : KeyboardResend);
        break;
    }
    return result;
}
// Service pending input and read status bits plus newly asserted IRQs.
// IBM Type 1 Controller; board gating/spacing and keyboard compatibility are described above.
// https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=336
I8042Device::Read I8042Device::read_status_register(uint64_t now, IrqState inputs) {
    auto irqs = poll_output_buffer(now, inputs);
    return {uint8_t((state_.keyboardFull ? OutputBufferFull : 0) |
                    (state_.keyboardFull && state_.keyboardAux ? AuxiliaryOutputBufferFull : 0) |
                    (state_.keyboardCommandByte & SystemFlag) |
                    (state_.keyboardLastCommand ? LastWriteWasCommand : 0)),
            irqs, state_.keyboardFull};
}
// Service/read the output latch, acknowledge its IRQ and apply host read spacing.
// IBM Type 1 Controller; board gating/spacing and keyboard compatibility are described above.
// https://wiki.retrotechcollection.com/images/0/0d/IBM_PS2_Hardware_Interface_Technical_Reference_May88.pdf#page=338
I8042Device::Read I8042Device::read_data_register(uint64_t now, IrqState inputs) {
    auto irqs = poll_output_buffer(now, inputs);
    bool full = state_.keyboardFull;
    if (full) {
        if (state_.keyboardAux) {
            state_.mouse.lastSent = state_.keyboardData;
            state_.mouseLine = false;
        } else
            state_.keyLine = false;
        state_.keyboardFull = false;
        state_.keyboardNextAt = now + OutputReadSpacingTicks;
    }
    return {state_.keyboardData, irqs, full};
}
} // namespace pc
