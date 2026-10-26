#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
namespace pc {
// PS/2 byte-protocol mouse with common three-button/IntelliMouse extensions.
// The i8042 controller owns byte transport and lastSent; state fields retain snapshot layout.
struct PS2Mouse {
    // IBM PS/1 Technical Reference (1990), section 6, Figure 6-1 command bytes.
    // https://ftp.infania.net/pub/DOS%20Drivers/IBM%20PS1/Section%206.%20Mouse.pdf#page=4
    enum Command : uint8_t {
        Reset = 0xff,  // Restore defaults and return self-test completion/device type.
        Resend = 0xfe, // Request retransmission of prior data (model resends one byte).
        SetDefault = 0xf6,
        Disable = 0xf5,
        Enable = 0xf4,
        SetSamplingRate = 0xf3,
        ReadDeviceType = 0xf2,
        SetRemoteMode = 0xf0,
        SetWrapMode = 0xee,
        ResetWrapMode = 0xec,
        ReadData = 0xeb,
        SetStreamMode = 0xea,
        StatusRequest = 0xe9,
        SetResolution = 0xe8,
        SetScaling2To1 = 0xe7,
        ResetScaling = 0xe6,
    };
    // Byte responses: ACK accepts a command/parameter; FE asks for retransmission;
    // AA means the basic assurance self-test completed successfully.
    static constexpr uint8_t Acknowledge = 0xfa, RequestResend = 0xfe, SelfTestPassed = 0xaa;
    // Resolution selector 2 means four counts/mm; power-on sampling is 100 reports/s.
    // https://ftp.infania.net/pub/DOS%20Drivers/IBM%20PS1/Section%206.%20Mouse.pdf#page=3
    static constexpr uint8_t DefaultResolution = 2, MaximumResolution = 3, DefaultSampleRate = 100,
                             StandardMouseId = 0;
    // IntelliMouse extension: sample rates 200,100,80 switch ID to 3 and add a
    // signed wheel byte. Reference is Linux's primary driver implementation,
    // not an IBM-defined command or a claim to emulate every IntelliMouse feature.
    // https://github.com/torvalds/linux/blob/master/drivers/input/mouse/psmouse-base.c#L532
    static constexpr uint8_t WheelMouseId = 3;
    static constexpr std::array<uint8_t, 3> WheelEnableSequence{200, 100, 80};
    // Button data mask: left/right/middle at bits 0/1/2 (middle is an extension).
    static constexpr uint8_t LeftButton = 1, RightButton = 2, MiddleButton = 4,
                             ButtonMask = LeftButton | RightButton | MiddleButton;
    // Status report bits differ from packet bits: remote[6], enabled[5], scale[4],
    // left[2], middle[1] (extension), right[0]. IBM Figure 6-3 reserves bit 1.
    // https://ftp.infania.net/pub/DOS%20Drivers/IBM%20PS1/Section%206.%20Mouse.pdf#page=7
    static constexpr uint8_t RemoteStatus = 0x40, EnabledStatus = 0x20, Scaling2Status = 0x10;
    // Packet bit 3 is the common PS/2 framing marker (IBM reserves it), and sign
    // bits 4/5 extend each eight-bit coordinate to a signed nine-bit value.
    // https://ftp.infania.net/pub/DOS%20Drivers/IBM%20PS1/Section%206.%20Mouse.pdf#page=9
    static constexpr uint8_t PacketSynchronization = 0x08, NegativeX = 0x10, NegativeY = 0x20;
    // IMPLEMENTATION NOTE: Keep excess motion for later packets instead of setting
    // overflow bits. Scale-2 input cap 127 keeps its doubled output within 255.
    static constexpr int UnscaledReportLimit = 255, ScaledInputLimit = 127, WheelReportLimit = 127;
    // IMPLEMENTATION NOTE: Approximate sample deadlines in retired-instruction ticks,
    // not physical mouse clock cycles. Immediate button reports stop at this backlog threshold.
    static constexpr uint64_t InstructionTicksPerSecond = 4772728;
    static constexpr size_t ImmediateReportBacklogThreshold = 512;

    bool enabled = false, remote = false, wrap = false, scaling2 = false;
    uint8_t resolution = DefaultResolution, rate = DefaultSampleRate, id = StandardMouseId,
            parameter = 0, buttons = 0, reportedButtons = 0, lastSent = 0;
    std::array<uint8_t, 3> rates{};
    int32_t dx = 0, dy = 0, wheel = 0;
    uint64_t nextAt = 0;
    std::deque<uint8_t> replies, packets;
    // Accept a protocol command/parameter and queue response bytes.
    void write_command(uint8_t value);
    // Accumulate host relative counts/buttons/wheel and preserve button transitions.
    void accumulate_motion(int x, int y, uint8_t held, int scroll = 0);
    // Check stream mode, sample deadline and pending motion/button changes without mutation.
    // IMPLEMENTATION NOTE: Host scheduling around IBM stream reports.
    // https://ftp.infania.net/pub/DOS%20Drivers/IBM%20PS1/Section%206.%20Mouse.pdf#page=9
    bool packet_due(uint64_t instructions) const {
        return enabled && !remote && !wrap && packets.empty() && instructions >= nextAt &&
               (dx || dy || wheel || buttons != reportedButtons);
    }
    // Append a due stream report and advance its instruction-derived deadline.
    void poll_due_report(uint64_t instructions);
    // Consume bounded motion into a three/four-byte report.
    void append_data_report(std::deque<uint8_t> &destination);
    // Read mode/enable/scaling/buttons in status-report bit order.
    uint8_t read_status() const;
    // Restore reset defaults and clear pending report state; retain physical button state.
    void set_defaults();
};
} // namespace pc
