#include "arm_vmbus.hpp"
#include <sstream>
#include <stdexcept>
namespace pc {
namespace {
uint64_t get(std::span<const uint8_t> bytes, size_t at, unsigned size) {
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= uint64_t(bytes[at + i]) << (8 * i);
    return value;
}
void put(std::span<uint8_t> bytes, size_t at, uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i) bytes[at + i] = uint8_t(value >> (8 * i));
}
bool supported(uint32_t version) {
    return (version >= 0x50000 && version <= 0x50003) || version == 0x60000;
}
}
ArmVmbus::ArmVmbus(std::span<const uint8_t> memory, uint64_t start, unsigned count)
    : ram(memory), base(start), cpus(count) {
    if (!count || count > 8) throw std::runtime_error("Invalid VMBus VP count");
}
bool ArmVmbus::validPage(uint64_t gpa) const {
    return !gpa || (!(gpa & 4095) && gpa >= base && gpa - base <= ram.size() &&
                    4096 <= ram.size() - (gpa - base));
}
uint16_t ArmVmbus::reject(uint16_t status) { ++rejected; return status; }
uint16_t ArmVmbus::receive(uint32_t messageType, std::span<const uint8_t> input, bool multiclient) {
    if (messageType != 1 || input.size() < 8 || input.size() > 240 || get(input, 4, 4)) return reject(5);
    auto kind = get(input, 0, 4);
    if (kind != 14 && kind != 3 && kind != 16) return reject(5);
    if (replies.size() >= 16) return reject(0x13);
    if (kind == 14) {
        if (connected) return reject(0x15);
        if (input.size() < 40) return reject(5);
        auto requested = uint32_t(get(input, 8, 4));
        auto vp = get(input, 12, 4);
        // The ordinary connection always uses VTL0/SINT2. Only connection 4
        // makes the lower target-info fields meaningful; Windows sends zero
        // there on connection 1 even though its active message source is SINT2.
        auto sint = multiclient ? get(input, 16, 1) : 2;
        auto vtl = multiclient ? get(input, 17, 1) : 0;
        auto flags = requested >= 0x60000 ? get(input, 20, 4) : 0;
        if (vp >= cpus) return reject(14);
        if (sint >= 16 || (multiclient && get(input, 18, 2))) return reject(5);
        if (vtl) return reject(6);
        if (requested < 0x50000) return reject(5); // Legacy interrupt-page ABI is not implemented.
        if ((flags & 8) && input.size() < 56) return reject(5); // Optional client GUID.
        auto monitor1 = get(input, 24, 8), monitor2 = get(input, 32, 8);
        if (!validPage(monitor1) || !validPage(monitor2) || (monitor1 && monitor1 == monitor2)) return reject(5);
        bool accept = supported(requested);
        // Copper adds SupportedFeatures. No optional channel extensions are
        // negotiated: the current bus has no offered channels or monitor use.
        std::vector<uint8_t> response(accept && requested >= 0x60000 ? 20 : 16);
        put(response, 0, 15, 4); response[8] = accept;
        if (accept) put(response, 12, 1, 4); // Plain message connection ID 1.
        replies.push_back({unsigned(vp), unsigned(sint), std::move(response)});
        targetVp = unsigned(vp); targetSint = unsigned(sint);
        version = accept ? requested : 0; connected = accept; ++contacts;
    } else {
        if (!connected) return reject(0x15);
        if (input.size() != 8) return reject(5);
        std::vector<uint8_t> response(8);
        put(response, 0, kind == 3 ? 4 : 17, 4);
        replies.push_back({targetVp, targetSint, std::move(response)});
        if (kind == 3) ++offers; // Accurate completion for the current empty offer inventory.
        else { connected = false; version = 0; ++unloads; }
    }
    return 0;
}
void ArmVmbus::service() {
    while (send && !replies.empty()) {
        auto &reply = replies.front();
        if (send(reply.vp, reply.sint, reply.payload)) break;
        replies.pop_front(); ++delivered;
    }
}
std::string ArmVmbus::report() const {
    std::ostringstream out;
    out << "{\"connected\":" << (connected ? "true" : "false") << ",\"version\":" << version
        << ",\"targetVp\":" << targetVp << ",\"targetSint\":" << targetSint
        << ",\"contacts\":" << contacts << ",\"offerRequests\":" << offers << ",\"unloads\":" << unloads
        << ",\"deliveredReplies\":" << delivered << ",\"pendingReplies\":" << replies.size()
        << ",\"rejectedMessages\":" << rejected << ",\"offeredChannels\":0}";
    return out.str();
}
}
