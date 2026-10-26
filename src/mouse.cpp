// PS/2 mouse byte protocol converts host relative input into motion reports and
// command responses delivered by the separate i8042 controller. Stream mode emits
// reports when enabled and a sampling deadline/button transition is due; remote
// mode waits for READ DATA. Wrap mode echoes commands except RESET/RESET WRAP.
// Replies and motion packets have separate queues so the controller can prioritize
// acknowledgements. X/Y counts include sign bits, buttons use low packet bits,
// and the common IntelliMouse rate handshake enables a fourth signed wheel byte.
//
// IBM's two-button PS/1 mouse reference defines the baseline commands, defaults,
// resolution/status/report and nonlinear 2:1 scaling. Three-button framing and
// wheel ID negotiation are compatibility extensions referenced against Linux's
// primary PS/2 driver; this is not a copy of that driver's implementation.
// IMPLEMENTATION NOTE: Host input is already in motion counts: resolution is
// reported/stored but does not rescale frontend input. Sampling uses instruction
// ticks; immediate button reports retain down/up ordering within one host batch.
// Accumulators saturate at signed 16-bit host bounds. There are no electrical
// clock/parity/timing or real self-test operations.
// COMPATIBILITY: Excess movement is retained for later packets instead of being
// cleared with overflow flags. RESEND returns lastSent byte rather than a prior
// whole packet; invalid input always asks for resend without IBM's second-error
// escalation. Mode changes preserve most pending movement. FIXME: Implement the
// specified counter-clearing/overflow/resend/error rules before claiming full IBM
// mouse behavior; preserve current guest compatibility in this migration.
// IBM PS/1 Technical Reference (1990), section 6; Linux intellimouse_detect.
// https://ftp.infania.net/pub/DOS%20Drivers/IBM%20PS1/Section%206.%20Mouse.pdf#page=3
// https://ftp.infania.net/pub/DOS%20Drivers/IBM%20PS1/Section%206.%20Mouse.pdf#page=10
// https://github.com/torvalds/linux/blob/master/drivers/input/mouse/psmouse-base.c#L532

#include "mouse.hpp"
#include <algorithm>
namespace pc {
// Restore command defaults and clear pending motion/report data, retaining current button state.
// IBM mouse section 6; host bounds and compatibility extensions are described above.
// https://ftp.infania.net/pub/DOS%20Drivers/IBM%20PS1/Section%206.%20Mouse.pdf#page=3
void PS2Mouse::set_defaults() {
    enabled = remote = wrap = scaling2 = false;
    resolution = DefaultResolution;
    rate = DefaultSampleRate;
    id = StandardMouseId;
    parameter = 0;
    rates = {};
    dx = dy = wheel = 0;
    nextAt = 0;
    reportedButtons = buttons;
    packets.clear();
}
// Encode mode, enable, scaling and button state in status-response bit order.
// IBM mouse section 6; host bounds and compatibility extensions are described above.
// https://ftp.infania.net/pub/DOS%20Drivers/IBM%20PS1/Section%206.%20Mouse.pdf#page=7
uint8_t PS2Mouse::read_status() const {
    return (remote ? RemoteStatus : 0) | (enabled ? EnabledStatus : 0) |
           (scaling2 ? Scaling2Status : 0) | ((buttons & LeftButton) << 2) |
           ((buttons & RightButton) >> 1) | ((buttons & MiddleButton) >> 1);
}
// Handle commands/parameters/wrap echo and append acknowledgement/status/report responses.
// IBM mouse section 6; host bounds and compatibility extensions are described above.
// https://ftp.infania.net/pub/DOS%20Drivers/IBM%20PS1/Section%206.%20Mouse.pdf#page=4
void PS2Mouse::write_command(uint8_t value) {
    if (wrap && value != Command::ResetWrapMode && value != Command::Reset) {
        replies.push_back(value);
        return;
    }
    if (parameter) {
        if (parameter == Command::SetResolution) {
            if (value > MaximumResolution) {
                replies.push_back(RequestResend);
                return;
            }
            resolution = value;
        } else {
            // IBM Figure 6-2 defines exactly these report rates (reports per second).
            // https://ftp.infania.net/pub/DOS%20Drivers/IBM%20PS1/Section%206.%20Mouse.pdf#page=6
            if (value != 10 && value != 20 && value != 40 && value != 60 && value != 80 &&
                value != 100 && value != 200) {
                replies.push_back(RequestResend);
                return;
            }
            rate = value;
            rates = {rates[1], rates[2], value};
            if (rates == WheelEnableSequence)
                id = WheelMouseId;
        }
        parameter = 0;
        replies.push_back(Acknowledge);
        return;
    }
    if (value == Command::Resend) {
        replies.push_back(lastSent);
        return;
    }
    switch (value) {
    case Command::Reset:
        set_defaults();
        replies.clear();
        replies.insert(replies.end(), {Acknowledge, SelfTestPassed, StandardMouseId});
        break;
    case Command::SetDefault:
        set_defaults();
        replies.push_back(Acknowledge);
        break;
    case Command::Enable:
        enabled = true;
        replies.push_back(Acknowledge);
        break;
    case Command::Disable:
        enabled = false;
        packets.clear();
        replies.push_back(Acknowledge);
        break;
    case Command::SetSamplingRate:
    case Command::SetResolution:
        parameter = value;
        replies.push_back(Acknowledge);
        break;
    case Command::ReadDeviceType:
        replies.insert(replies.end(), {Acknowledge, id});
        break;
    case Command::SetRemoteMode:
        remote = true;
        packets.clear();
        replies.push_back(Acknowledge);
        break;
    case Command::SetStreamMode:
        remote = false;
        replies.push_back(Acknowledge);
        break;
    case Command::SetWrapMode:
        wrap = true;
        packets.clear();
        replies.push_back(Acknowledge);
        break;
    case Command::ResetWrapMode:
        wrap = false;
        replies.push_back(Acknowledge);
        break;
    case Command::ReadData:
        replies.push_back(Acknowledge);
        append_data_report(replies);
        break;
    case Command::StatusRequest:
        replies.insert(replies.end(), {Acknowledge, read_status(), resolution, rate});
        break;
    case Command::ResetScaling:
    case Command::SetScaling2To1:
        scaling2 = value == Command::SetScaling2To1;
        replies.push_back(Acknowledge);
        break;
    default:
        replies.push_back(RequestResend);
        break;
    }
}
// Emit bounded signed coordinate bytes, button bits and optional wheel; retain excess counts.
// IBM mouse section 6; host bounds and compatibility extensions are described above.
// https://ftp.infania.net/pub/DOS%20Drivers/IBM%20PS1/Section%206.%20Mouse.pdf#page=9
void PS2Mouse::append_data_report(std::deque<uint8_t> &destination) {
    int limit = scaling2 ? ScaledInputLimit : UnscaledReportLimit;
    int x = std::clamp(dx, -limit, limit), y = std::clamp(dy, -limit, limit);
    dx -= x;
    dy -= y;
    // Apply IBM Set Scaling 2:1 to one bounded coordinate, preserving its sign.
    // https://ftp.infania.net/pub/DOS%20Drivers/IBM%20PS1/Section%206.%20Mouse.pdf#page=8
    auto scale_coordinate = [&](int value) {
        if (!scaling2)
            return value;
        int magnitude = value < 0 ? -value : value;
        // Nonlinear 2:1 mapping for magnitudes 0..5; larger values double.
        // IBM Figure 6-5; the sign is restored after scaling the magnitude.
        // https://ftp.infania.net/pub/DOS%20Drivers/IBM%20PS1/Section%206.%20Mouse.pdf#page=8
        const int SmallMagnitudeOutput[] = {0, 1, 1, 3, 6, 9};
        int result = magnitude < 6 ? SmallMagnitudeOutput[magnitude] : magnitude * 2;
        return value < 0 ? -result : result;
    };
    x = scale_coordinate(x);
    y = scale_coordinate(y);
    destination.push_back(PacketSynchronization | buttons | (x < 0 ? NegativeX : 0) |
                          (y < 0 ? NegativeY : 0));
    destination.push_back(uint8_t(x));
    destination.push_back(uint8_t(y));
    if (id == WheelMouseId) {
        int z = std::clamp(wheel, -WheelReportLimit, WheelReportLimit);
        wheel -= z;
        destination.push_back(uint8_t(z));
    } else
        wheel = 0;
    reportedButtons = buttons;
}
// Accumulate enabled host input and queue immediate button transitions in stream mode.
// IBM mouse section 6; host bounds and compatibility extensions are described above.
// https://ftp.infania.net/pub/DOS%20Drivers/IBM%20PS1/Section%206.%20Mouse.pdf#page=9
void PS2Mouse::accumulate_motion(int x, int y, uint8_t held, int scroll) {
    bool changed = buttons != (held & ButtonMask);
    buttons = held & ButtonMask;
    if (!enabled)
        return;
    dx = int32_t(std::clamp(int64_t(dx) + x, int64_t(INT16_MIN), int64_t(INT16_MAX)));
    dy = int32_t(std::clamp(int64_t(dy) + y, int64_t(INT16_MIN), int64_t(INT16_MAX)));
    if (id == WheelMouseId)
        wheel =
            int32_t(std::clamp(int64_t(wheel) + scroll, int64_t(INT16_MIN), int64_t(INT16_MAX)));
    // Keep down/up transitions even when both arrive in one frontend batch.
    if (changed && !remote && !wrap && packets.size() < ImmediateReportBacklogThreshold)
        append_data_report(packets);
}
// Append a due stream report and schedule the next instruction-derived sampling deadline.
// IBM mouse section 6; host bounds and compatibility extensions are described above.
// https://ftp.infania.net/pub/DOS%20Drivers/IBM%20PS1/Section%206.%20Mouse.pdf#page=9
void PS2Mouse::poll_due_report(uint64_t instructions) {
    if (packet_due(instructions)) {
        append_data_report(packets);
        nextAt = instructions + InstructionTicksPerSecond / rate;
    }
}
} // namespace pc
