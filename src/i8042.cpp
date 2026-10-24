#include "i8042.hpp"
namespace pc {
I8042Device::Irqs I8042Device::poll(uint64_t now, IrqState inputs) {
    Irqs result;
    // No output-buffer/IRQ work is possible until queued input is ready or
    // the mouse sample deadline arrives. Inspect live state so guest I/O,
    // host input and restored snapshots need no cached-deadline invalidation.
    if (!state_.keyboardFull && !state_.mouse.packetDue(now) &&
        (now < state_.keyboardNextAt ||
         (state_.keyboardReplies.empty() && state_.mouse.replies.empty() && state_.scanCodes.empty() &&
          state_.mouse.packets.empty())))
        return result;
    state_.mouse.poll(now);
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
        } else if (state_.keyboardEnabled && !state_.keyboardParameter && !(state_.keyboardCommandByte & 0x10) &&
                   !inputs.keyboardInService && (!(state_.keyboardCommandByte & 1) || !inputs.keyboardMasked) &&
                   !state_.scanCodes.empty())
            queue = &state_.scanCodes;
        else if (!(state_.keyboardCommandByte & 0x20) && !inputs.mouseInService &&
                 (!(state_.keyboardCommandByte & 2) || !inputs.mouseMasked) && !state_.mouse.packets.empty()) {
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
    if (state_.keyboardFull && state_.keyboardAux && (state_.keyboardCommandByte & 2) && !state_.mouseLine) {
        result.mouse = true;
        state_.mouseLine = true;
    } else if (state_.keyboardFull && !state_.keyboardAux && (state_.keyboardCommandByte & 1) && !state_.keyLine) {
        result.keyboard = true;
        state_.keyLine = true;
    }
    return result;
}
I8042Device::Effects I8042Device::command(uint8_t value, bool a20) {
    Effects result;
    state_.keyboardLastCommand = true;
    state_.keyboardControllerPending = 0;
    // AT controller F0..FF pulse low the output bits whose command bits are
    // zero. Output bit 0 is RESET#, so every even pulse command resets the CPU.
    if ((value & 0xf0) == 0xf0 && !(value & 1)) {
        result.reset = true;
        return result;
    }
    switch (value) {
    case 0x20:
        state_.keyboardReplies.push_back(state_.keyboardCommandByte);
        break;
    case 0x60:
    case 0xd1:
    case 0xd2:
    case 0xd3:
    case 0xd4:
        state_.keyboardControllerPending = value;
        break;
    case 0xaa:
        state_.keyboardCommandByte |= 4;
        state_.keyboardReplies.push_back(0x55);
        break;
    case 0xab:
        state_.keyboardReplies.push_back(0);
        break;
    case 0xad:
        state_.keyboardCommandByte |= 0x10;
        break;
    case 0xae:
        state_.keyboardCommandByte &= ~0x10;
        break;
    case 0xa7:
        state_.keyboardCommandByte |= 0x20;
        break;
    case 0xa8:
        state_.keyboardCommandByte &= ~0x20;
        break;
    case 0xa9:
        state_.keyboardReplies.push_back(0);
        break;
    case 0xc0:
        state_.keyboardReplies.push_back(0x80);
        break;
    case 0xd0:
        state_.keyboardReplies.push_back((state_.keyboardOutputPort & ~2) | (a20 ? 2 : 0));
        break;
    default:
        break;
    }
    return result;
}
I8042Device::Effects I8042Device::write(uint8_t value) {
    Effects result;
    state_.keyboardLastCommand = false;
    if (state_.keyboardControllerPending) {
        auto command = state_.keyboardControllerPending;
        state_.keyboardControllerPending = 0;
        if (command == 0x60) {
            state_.keyboardCommandByte = value;
            if (!(value & 1))
                state_.keyLine = false;
            if (!(value & 2))
                state_.mouseLine = false;
        } else if (command == 0xd1) {
            state_.keyboardOutputPort = value;
            result.a20 = bool(value & 2);
            if (!(value & 1))
                result.reset = true;
        } else if (command == 0xd2)
            state_.keyboardReplies.push_back(value);
        else if (command == 0xd3)
            state_.mouse.replies.push_back(value);
        else if (command == 0xd4)
            state_.mouse.command(value);
        return result;
    }
    if (state_.keyboardParameter) {
        auto parameter = state_.keyboardParameter;
        state_.keyboardParameter = 0;
        state_.keyboardReplies.push_back(0xfa);
        if (parameter == 0xed)
            state_.keyboardLEDs = value & 7;
        if (parameter == 0xf0) {
            if (!value)
                state_.keyboardReplies.push_back(state_.keyboardScanSet);
            else if (value <= 3)
                state_.keyboardScanSet = value;
        }
        return result;
    }
    switch (value) {
    case 0xed:
    case 0xf0:
    case 0xf3:
        state_.keyboardParameter = value;
        state_.keyboardReplies.push_back(0xfa);
        break;
    case 0xee:
        state_.keyboardReplies.push_back(0xee);
        break;
    case 0xf2:
        state_.keyboardReplies.insert(state_.keyboardReplies.end(), {0xfa, 0xab, 0x83});
        break;
    case 0xf4:
        state_.keyboardEnabled = true;
        state_.keyboardReplies.push_back(0xfa);
        break;
    case 0xf5:
        state_.keyboardEnabled = false;
        state_.keyboardLEDs = 0;
        state_.keyboardScanSet = 2;
        state_.keyboardReplies.push_back(0xfa);
        break;
    case 0xf6:
        state_.keyboardLEDs = 0;
        state_.keyboardScanSet = 2;
        state_.keyboardReplies.push_back(0xfa);
        break;
    case 0xff:
        state_.keyboardLEDs = 0;
        state_.keyboardScanSet = 2;
        state_.keyboardEnabled = true;
        state_.keyboardReplies.insert(state_.keyboardReplies.end(), {0xfa, 0xaa});
        break;
    case 0xfe:
        state_.keyboardReplies.push_back(state_.keyboardData);
        break;
    default:
        state_.keyboardReplies.push_back(value >= 0xf7 ? 0xfa : 0xfe);
        break;
    }
    return result;
}
I8042Device::Read I8042Device::readStatus(uint64_t now, IrqState inputs) {
    auto irqs = poll(now, inputs);
    return {uint8_t((state_.keyboardFull ? 1 : 0) |
                   (state_.keyboardFull && state_.keyboardAux ? 0x20 : 0) |
                   (state_.keyboardCommandByte & 4) | (state_.keyboardLastCommand ? 8 : 0)),
            irqs, state_.keyboardFull};
}
I8042Device::Read I8042Device::readData(uint64_t now, IrqState inputs) {
    auto irqs = poll(now, inputs);
    bool full = state_.keyboardFull;
    if (full) {
        if (state_.keyboardAux) {
            state_.mouse.lastSent = state_.keyboardData;
            state_.mouseLine = false;
        } else state_.keyLine = false;
        state_.keyboardFull = false;
        state_.keyboardNextAt = now + 4000;
    }
    return {state_.keyboardData, irqs, full};
}
} // namespace pc
