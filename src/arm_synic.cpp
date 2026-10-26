// Hyper-V's Synthetic Interrupt Controller (SynIC) delivers small messages and
// event bits to each virtual processor (VP). The guest enables SCONTROL, registers
// two RAM pages and configures sixteen synthetic interrupt sources (SINTs). Each
// source owns one 256-byte message slot and 2048 event flags. Messages carry a
// typed payload; events simply mark that work is available elsewhere, such as a
// VMBus ring. A zero message type means a slot is free. The guest consumes data,
// clears the type/event bits, and writes End Of Message (EOM) when more is pending.
//
// On ARM64 these are hypercall VP-register identifiers, not memory-mapped offsets
// or x64 model-specific registers. This component stores/validates the registers,
// queues messages, publishes RAM and requests notifications. The board supplies a
// Generic Interrupt Controller (GIC) callback and manages actual interrupt state.
// Masking suppresses message interrupts; polling additionally allows event updates
// without an interrupt. Queued messages never overwrite an occupied guest slot.
//
// IMPLEMENTATION NOTE: Every call requires all VPs stopped. RAM overlays are
// validated on access, newly installed addresses are zeroed, same-address rewrites
// retain pending data. A delivery failure restores changed RAM so callers can
// retry. Native GIC end-of-interrupt handling need not exit to the host, so the
// board also requests queue scans between stopped execution slices. Host limits
// are eight VPs and sixteen queued messages per VP/SINT; this is not a complete
// implementation of partition ports or the TLFS per-port buffer-allocation model.
// Only virtual trust level zero is supported. The ARM backend accepts private GIC
// IDs 0..31 and Masked/Polling; AutoEOI, intercept, proxy and resend-page support
// are rejected. Reserved control/page bits must be zero in this subset. Diagnostics
// expose bounded metadata, not arbitrary guest message payloads.
// Microsoft Hypervisor Top-Level Functional Specification (online), Inter-Partition
// Communication; HV_REGISTER_NAME; HV_MESSAGE; HV_SYNIC_EVENT_FLAGS.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#synic-registers
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_message
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_register_name#synthetic-interrupt-controller-synic--primary

#include "arm_synic.hpp"
#include <algorithm>
#include <sstream>
#include <stdexcept>
namespace pc {
namespace {
// Hypercall status codes: invalid arguments/VP, unavailable message storage,
// disabled SynIC and delivery failure. NotAcknowledged is used internally for a
// busy dedicated timer slot; it does not assert that a GIC interrupt was acknowledged.
// TLFS v6.0b Appendix B (PDF pp. 214-216), with 0013h confirmed by Linux's
// primary Hyper-V ABI header (the PDF's insufficient-buffers entry has a typo).
// https://raw.githubusercontent.com/Microsoft/Virtualization-Documentation/master/tlfs/Hypervisor%20Top%20Level%20Functional%20Specification%20v6.0b.pdf#page=214
// https://github.com/torvalds/linux/blob/master/include/hyperv/hvgdk_mini.h
constexpr uint16_t InvalidParameter = 5, InvalidVirtualProcessor = 14, InsufficientBuffers = 0x13,
                   InvalidSynicState = 0x18, OperationFailed = 0x71, NotAcknowledged = 0x14;
// SCONTROL and overlay registers use bit zero as enable. Bits [11:1] must remain
// zero in this subset; address bits above 11 select a 4096-byte guest RAM page.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#scontrol-register
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#simp-register
constexpr uint64_t Enabled = 1, OverlayPageBytes = 4096,
                   OverlayPageOffsetMask = OverlayPageBytes - 1,
                   OverlayReservedMask = OverlayPageOffsetMask & ~Enabled;
// IMPLEMENTATION NOTE: This ARM backend supports SGI/PPI IDs 0..31, not x64 APIC
// vector 16..255 semantics. Reject AutoEOI, proxy and all other unsupported flags.
constexpr uint64_t PrivateInterruptIdMask = 31;
// Version 1 identifies this SynIC interface, not a Windows release.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#sversion-register
constexpr uint64_t SynicVersion = 1;
// TLFS HV_MESSAGE header: type u32, payload length u8, flags u8, reserved u16,
// then sender/port/origination u64 and up to 240 payload bytes. Zero type means empty.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_message
enum MessageField : size_t { Type = 0, PayloadSize = 4, Flags = 5, Sender = 8, Payload = 16 };
constexpr size_t MessageTypeBytes = 4, MessageSenderBytes = 8, MaximumPayloadBytes = 240;
constexpr uint8_t MessagePending = 1;
// IMPLEMENTATION NOTE: One bounded queue per VP/SINT, not a global/per-port pool.
constexpr size_t MaximumQueuedMessages = 16;
// Diagnostics decode only VMBus VERSION_RESPONSE metadata, never arbitrary payload.
// Linux's VMBus wire layout is the primary implementation reference:
// https://github.com/torvalds/linux/blob/master/include/linux/hyperv.h
constexpr uint32_t VmbusMessageType = 1, VmbusVersionResponseType = 15;
constexpr size_t VersionReplyMinimumBytes = 16, VersionReplyWithFeaturesBytes = 20,
                 VersionReplySupportedOffset = MessageField::Payload + 8,
                 VersionReplyStateOffset = MessageField::Payload + 9,
                 VersionReplyConnectionOffset = MessageField::Payload + 12,
                 VersionReplyFeaturesOffset = MessageField::Payload + 16;
// Encode a bounded little-endian header field into caller-validated storage.
// Microsoft TLFS, SynIC shared-memory/register interface.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_message
void write_little_endian(std::span<uint8_t> bytes, size_t at, uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        bytes[at + i] = uint8_t(value >> (8 * i));
}
} // namespace
// Create disabled SynIC state; reject a zero or unsupported host VP count.
// Microsoft TLFS, SynIC shared-memory/register interface.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#synic-registers
ArmSynic::ArmSynic(std::span<uint8_t> memory, uint64_t start, unsigned count)
    : ram(memory), base(start), cpus(count) {
    if (!count || count > state.size())
        throw std::runtime_error("Invalid SynIC VP count");
}
// Return an enabled overlay only when the whole 4 KiB page lies in host RAM.
// IMPLEMENTATION NOTE: MMIO and unmapped addresses are rejected by this RAM-only backend.
// Microsoft TLFS, SynIC shared-memory/register interface.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#simp-register
std::span<uint8_t> ArmSynic::registered_page(uint64_t reg) const {
    auto address = reg & ~OverlayPageOffsetMask;
    if (!(reg & Enabled) || address < base || address - base > ram.size() ||
        OverlayPageBytes > ram.size() - (address - base))
        return {};
    return ram.subspan(size_t(address - base), OverlayPageBytes);
}
// Report queued messages on enabled VP message pages without changing memory.
// IMPLEMENTATION NOTE: The execution loop uses this conservative predicate to schedule a rescan.
// Microsoft TLFS, SynIC shared-memory/register interface.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#messages
bool ArmSynic::needs_service() const {
    for (unsigned vp = 0; vp < cpus; ++vp) {
        const auto &v = state[vp];
        if (!(v.control & Enabled) || registered_page(v.messages).empty())
            continue;
        for (const auto &queue : v.queue)
            if (!queue.empty())
                return true;
    }
    return false;
}
// Attempt one delivery per VP/SINT and return the first error while servicing other sources.
// IMPLEMENTATION NOTE: Slice-boundary rescans compensate for native GIC EOI without a host exit.
// Microsoft TLFS, SynIC shared-memory/register interface.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#messages
uint16_t ArmSynic::service_message_queues() {
    uint16_t firstError = 0;
    for (unsigned vp = 0; vp < cpus; ++vp)
        for (unsigned sint = 0; sint < SyntheticInterruptCount; ++sint) {
            auto status = deliver_queued_message(vp, sint);
            if (status && !firstError)
                firstError = status;
        }
    return firstError;
}
// Read a VP register; version is one, EOM is zero, unknown registers/VPs return status.
// TLFS SynIC Registers / ARM64 HV_REGISTER_NAME.
// Microsoft TLFS, SynIC shared-memory/register interface.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#synic-registers
uint16_t ArmSynic::read_vp_register(uint32_t name, unsigned vp, uint64_t &value) const {
    value = 0;
    if (vp >= cpus)
        return InvalidVirtualProcessor;
    const auto &v = state[vp];
    if (name < Register::ControlRegister && name >= Register::SyntheticInterruptFirstRegister)
        value = v.sint[name - Register::SyntheticInterruptFirstRegister];
    else
        switch (name) {
        case Register::ControlRegister:
            value = v.control;
            break;
        case Register::VersionRegister:
            value = SynicVersion;
            break;
        case Register::EventFlagsPageRegister:
            value = v.events;
            break;
        case Register::MessagePageRegister:
            value = v.messages;
            break;
        case Register::EndOfMessageRegister:
            break;
        default:
            return InvalidParameter;
        }
    return 0;
}
// Validate/store supported SynIC state, map RAM overlays or trigger EOM delivery.
// IMPLEMENTATION NOTE: Only enabled overlays wholly in RAM and supported private INTIDs are
// accepted. Microsoft TLFS, SynIC shared-memory/register interface.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#synic-registers
uint16_t ArmSynic::write_vp_register(uint32_t name, unsigned vp, uint64_t value) {
    if (vp >= cpus)
        return InvalidVirtualProcessor;
    auto &v = state[vp];
    if (name >= Register::SyntheticInterruptFirstRegister && name < Register::ControlRegister) {
        // ARM uses a private interrupt identifier (INTID), either software-generated
        // (SGI) or a private peripheral interrupt (PPI). AutoEOI/proxy are unsupported; never
        // silently accept the x64 APIC behavior on the virtual GIC.
        if (value & ~(PrivateInterruptIdMask | InterruptMasked | InterruptPolling))
            return InvalidParameter;
        v.sint[name - Register::SyntheticInterruptFirstRegister] = value;
        return 0;
    }
    switch (name) {
    case Register::ControlRegister:
        if (value & ~Enabled)
            return InvalidParameter;
        v.control = value;
        return 0;
    case Register::EventFlagsPageRegister:
    case Register::MessagePageRegister: {
        if ((value & OverlayReservedMask) || ((value & Enabled) && registered_page(value).empty()))
            return InvalidParameter;
        auto &reg = name == Register::EventFlagsPageRegister ? v.events : v.messages;
        // Clear a newly installed overlay. Rewriting the same register must
        // not destroy notifications already deposited in guest memory.
        if ((value & Enabled) &&
            (!reg || (reg & ~OverlayPageOffsetMask) != (value & ~OverlayPageOffsetMask)))
            std::fill(registered_page(value).begin(), registered_page(value).end(), uint8_t(0));
        reg = value;
        return 0;
    }
    case Register::EndOfMessageRegister:
        if (value)
            return InvalidParameter;
        for (unsigned sint = 0; sint < SyntheticInterruptCount; ++sint) {
            auto status = deliver_queued_message(vp, sint);
            if (status)
                return status;
        }
        return 0;
    default:
        return InvalidParameter;
    }
}
// Suppress delivery for polling/masked sources; otherwise request the real private GIC interrupt.
// A false callback result lets the caller restore RAM and retry safely.
// Microsoft TLFS, SynIC shared-memory/register interface.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#sintx-registers
bool ArmSynic::notify_synthetic_interrupt(unsigned vp, unsigned sint) {
    auto config = state[vp].sint[sint];
    if ((config & InterruptPolling) || (config & InterruptMasked))
        return true;
    return interrupt && interrupt(1ull << vp, unsigned(config & PrivateInterruptIdMask));
}
// Set an event bit only once and notify on a clear-to-set transition; rollback on injection
// failure. IMPLEMENTATION NOTE: All VPs stopped makes RAM byte updates mutually exclusive with the
// guest. Microsoft TLFS, SynIC shared-memory/register interface.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#event-flag-delivery
uint16_t ArmSynic::signal_event(unsigned vp, unsigned sint, unsigned flag, bool &newlySignaled) {
    newlySignaled = false;
    if (vp >= cpus)
        return InvalidVirtualProcessor;
    if (sint >= SyntheticInterruptCount || flag >= EventFlagsCount)
        return InvalidParameter;
    auto &v = state[vp];
    auto memory = registered_page(v.events);
    if (!(v.control & Enabled) || memory.empty() ||
        ((v.sint[sint] & InterruptMasked) && !(v.sint[sint] & InterruptPolling)))
        return InvalidSynicState;
    auto &byte = memory[sint * EventFlagsBytes + flag / 8];
    auto bit = uint8_t(1u << (flag % 8));
    if (byte & bit)
        return 0;
    byte |= bit;
    if (!notify_synthetic_interrupt(vp, sint)) {
        byte &= uint8_t(~bit);
        return OperationFailed;
    }
    newlySignaled = true;
    return 0;
}
// Publish one queued message only when its slot type is None; set pending for remaining work.
// On failed GIC delivery restore the complete prior slot and retain the queue.
// Microsoft TLFS, SynIC shared-memory/register interface.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#messages
uint16_t ArmSynic::deliver_queued_message(unsigned vp, unsigned sint) {
    auto &v = state[vp];
    auto &queue = v.queue[sint];
    auto memory = registered_page(v.messages);
    if (queue.empty() || !(v.control & Enabled) || memory.empty())
        return 0;
    auto slot = memory.subspan(sint * MessageBytes, MessageBytes);
    if (slot[MessageField::Type] || slot[MessageField::Type + 1] || slot[MessageField::Type + 2] ||
        slot[MessageField::Type + 3]) {
        slot[MessageField::Flags] |= MessagePending; // More messages await this occupied slot.
        return 0;
    }
    Message previous;
    std::copy(slot.begin(), slot.end(), previous.begin());
    std::copy(queue.front().begin(), queue.front().end(), slot.begin());
    slot[MessageField::Flags] = queue.size() > 1 ? MessagePending : 0;
    if (!notify_synthetic_interrupt(vp, sint)) {
        std::copy(previous.begin(), previous.end(), slot.begin());
        return OperationFailed;
    }
    queue.pop_front();
    return 0;
}
// Try immediate dedicated-buffer publication; busy slots return internal NotAcknowledged.
// The timer keeps ownership until publication, avoiding an extra queued timer message.
// Microsoft TLFS, SynIC shared-memory/register interface.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#message-sources
uint16_t ArmSynic::try_post_message(unsigned vp, unsigned sint, uint32_t type, uint64_t sender,
                                    std::span<const uint8_t> payload) {
    if (vp >= cpus)
        return InvalidVirtualProcessor;
    if (sint >= SyntheticInterruptCount || !type || payload.size() > MaximumPayloadBytes)
        return InvalidParameter;
    auto &v = state[vp];
    auto memory = registered_page(v.messages);
    if (!(v.control & Enabled) || memory.empty())
        return InvalidSynicState;
    auto slot = memory.subspan(sint * MessageBytes, MessageBytes);
    if (!v.queue[sint].empty() || slot[MessageField::Type] || slot[MessageField::Type + 1] ||
        slot[MessageField::Type + 2] || slot[MessageField::Type + 3]) {
        slot[MessageField::Flags] |= MessagePending;
        return NotAcknowledged; // NotAcknowledged: caller retains its dedicated buffer.
    }
    return post_message(vp, sint, type, sender, payload);
}
// Queue a valid message in host storage and attempt delivery; return an HV_STATUS.
// IMPLEMENTATION NOTE: The host queue cap is per VP/SINT and a failed injection undoes enqueue.
// Microsoft TLFS, SynIC shared-memory/register interface.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/inter-partition-communication#message-buffer-queues
uint16_t ArmSynic::post_message(unsigned vp, unsigned sint, uint32_t type, uint64_t sender,
                                std::span<const uint8_t> payload) {
    if (vp >= cpus)
        return InvalidVirtualProcessor;
    if (sint >= SyntheticInterruptCount || !type || payload.size() > MaximumPayloadBytes)
        return InvalidParameter;
    auto &v = state[vp];
    if (!(v.control & Enabled) || registered_page(v.messages).empty())
        return InvalidSynicState;
    auto &queue = v.queue[sint];
    if (queue.size() >= MaximumQueuedMessages)
        return InsufficientBuffers;
    Message message{};
    write_little_endian(message, MessageField::Type, type, MessageTypeBytes);
    message[MessageField::PayloadSize] = uint8_t(payload.size());
    write_little_endian(message, MessageField::Sender, sender, MessageSenderBytes);
    std::copy(payload.begin(), payload.end(), message.begin() + MessageField::Payload);
    queue.push_back(message);
    auto status = deliver_queued_message(vp, sint);
    if (status)
        queue.pop_back(); // Failed posting is safe for a caller to retry.
    return status;
}
// Serialize bounded register/queue/slot metadata for host diagnostics, excluding payload contents.
// IMPLEMENTATION NOTE: Only recognized VMBus version-response fields are decoded after bounds
// checks. Microsoft TLFS, SynIC shared-memory/register interface.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/datatypes/hv_message
std::string ArmSynic::report() const {
    std::ostringstream out;
    out << "{\"vps\":[";
    for (unsigned vp = 0; vp < cpus; ++vp) {
        if (vp)
            out << ',';
        const auto &v = state[vp];
        const auto memory = registered_page(v.messages);
        out << "{\"index\":" << vp << ",\"control\":" << v.control << ",\"eventPage\":" << v.events
            << ",\"messagePage\":" << v.messages << ",\"sints\":[";
        for (unsigned sint = 0; sint < SyntheticInterruptCount; ++sint) {
            if (sint)
                out << ',';
            out << "{\"index\":" << sint << ",\"config\":" << v.sint[sint]
                << ",\"queued\":" << v.queue[sint].size();
            if (!memory.empty()) {
                const auto slot = memory.subspan(sint * MessageBytes, MessageBytes);
                auto u32 = [&](size_t at) {
                    uint32_t value = 0;
                    for (unsigned i = 0; i < 4; ++i)
                        value |= uint32_t(slot[at + i]) << (8 * i);
                    return value;
                };
                out << ",\"slotType\":" << u32(MessageField::Type)
                    << ",\"payloadBytes\":" << unsigned(slot[MessageField::PayloadSize])
                    << ",\"messageFlags\":" << unsigned(slot[MessageField::Flags]);
                // Decode only our version response metadata, bounded by the
                // declared payload. Stale bytes in a guest-cleared slot are ignored.
                if (u32(MessageField::Type) == VmbusMessageType &&
                    slot[MessageField::PayloadSize] >= VersionReplyMinimumBytes &&
                    slot[MessageField::PayloadSize] <= MaximumPayloadBytes &&
                    u32(MessageField::Payload) == VmbusVersionResponseType) {
                    out << ",\"vmbusVersionReply\":{\"supported\":"
                        << unsigned(slot[VersionReplySupportedOffset])
                        << ",\"connectionState\":" << unsigned(slot[VersionReplyStateOffset])
                        << ",\"connectionId\":" << u32(VersionReplyConnectionOffset);
                    if (slot[MessageField::PayloadSize] >= VersionReplyWithFeaturesBytes)
                        out << ",\"features\":" << u32(VersionReplyFeaturesOffset);
                    out << '}';
                }
            }
            out << '}';
        }
        out << "]}";
    }
    return out.str() + "]}";
}

} // namespace pc
