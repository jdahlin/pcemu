#include "arm_vmbus.hpp"
#include <algorithm>
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
ArmVmbus::ArmVmbus(std::span<uint8_t> memory, uint64_t start, unsigned count)
    : ram(memory), base(start), cpus(count), gpadls(memory, start) {
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
    if (kind != 14 && kind != 3 && kind != 16) {
        if (!connected) return reject(0x15);
        if (replies.size() >= 16) return reject(0x13);
        return channelMessage(uint32_t(kind), input);
    }
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
        // Copper adds SupportedFeatures. Optional channel extensions and
        // monitor-page signaling are not negotiated by this implementation.
        std::vector<uint8_t> response(accept && requested >= 0x60000 ? 20 : 16);
        put(response, 0, 15, 4); response[8] = accept;
        if (accept) put(response, 12, 1, 4); // Plain message connection ID 1.
        replies.push_back({unsigned(vp), unsigned(sint), std::move(response)});
        targetVp = unsigned(vp); targetSint = unsigned(sint);
        version = accept ? requested : 0; connected = accept; ++contacts;
    } else {
        if (!connected) return reject(0x15);
        if (input.size() != 8) return reject(5);
        if (kind == 3) {
            if (channels.size() + 1 > 16 - replies.size()) return reject(0x13);
            for (auto &[id, channel] : channels) {
                (void)id;
                replies.push_back({targetVp, targetSint, offerMessage(channel.offer)});
                channel.offered = true; ++channelOffers;
            }
            ++offers;
        } else {
            for (auto &[id, channel] : channels) {
                close(channel); gpadls.clearChannel(id); channel.offered = false;
            }
            connected = false; version = 0; ++unloads;
        }
        std::vector<uint8_t> response(8);
        put(response, 0, kind == 3 ? 4 : 17, 4);
        replies.push_back({targetVp, targetSint, std::move(response)});
    }
    return 0;
}
bool ArmVmbus::addChannel(const Offer &offer, PacketHandler handler, std::function<void()> closed,
                          std::function<void()> opened) {
    if (connected || !event || !handler || channels.size() >= 8 || !offer.channel || offer.channel >= 2048 ||
        offer.connection < 16 || offer.connection > 0xffffff || (offer.flags & ~uint16_t(0x11)) ||
        channels.contains(offer.channel) ||
        std::all_of(offer.interfaceId.begin(), offer.interfaceId.end(), [](auto b) { return !b; }) ||
        std::all_of(offer.instanceId.begin(), offer.instanceId.end(), [](auto b) { return !b; })) return false;
    for (const auto &[id, channel] : channels) {
        (void)id;
        if (channel.offer.connection == offer.connection ||
            (channel.offer.interfaceId == offer.interfaceId && channel.offer.instanceId == offer.instanceId)) return false;
    }
    Channel channel;
    channel.offer = offer; channel.handler = std::move(handler); channel.closed = std::move(closed); channel.opened = std::move(opened);
    channels.emplace(offer.channel, std::move(channel));
    return true;
}
std::vector<uint8_t> ArmVmbus::offerMessage(const Offer &offer) const {
    std::vector<uint8_t> message(196);
    put(message, 0, 1, 4);
    std::copy(offer.interfaceId.begin(), offer.interfaceId.end(), message.begin() + 8);
    std::copy(offer.instanceId.begin(), offer.instanceId.end(), message.begin() + 24);
    put(message, 56, offer.flags, 2);
    std::copy(offer.userData.begin(), offer.userData.end(), message.begin() + 60);
    put(message, 184, offer.channel, 4); put(message, 190, 1, 2); // Dedicated event port.
    put(message, 192, offer.connection, 4);
    return message;
}
void ArmVmbus::response(uint32_t kind, uint32_t channel, uint32_t id, uint32_t status) {
    std::vector<uint8_t> message(20);
    put(message, 0, kind, 4); put(message, 8, channel, 4);
    put(message, 12, id, 4); put(message, 16, status, 4);
    replies.push_back({targetVp, targetSint, std::move(message)});
}
void ArmVmbus::close(Channel &channel) {
    const bool wasOpen = bool(channel.rx);
    channel.rx.reset(); channel.tx.reset(); channel.pending.clear();
    channel.notification = channel.work = false; channel.ringGpadl = 0;
    if (wasOpen) { ++closes; if (channel.closed) channel.closed(); }
}
uint16_t ArmVmbus::signalChannel(uint32_t id) {
    auto it = channels.find(id);
    if (!connected || it == channels.end() || !it->second.rx) return 0x15;
    it->second.work = true;
    return 0;
}
uint16_t ArmVmbus::sendChannelPacket(uint32_t id, std::span<const uint8_t> packet) {
    auto it=channels.find(id);
    if(!connected || it==channels.end() || !it->second.tx) return 0x15;
    auto &channel=it->second;
    if(!channel.pending.empty()) return 0x13;
    bool notify=false;
    auto result=channel.tx->push(packet,notify);
    if(result==ArmVmbusRing::Result::Full) return 0x13;
    if(result!=ArmVmbusRing::Result::Ok) { ++faults; close(channel); return 5; }
    channel.notification |= notify; ++packetsOut; return 0;
}
bool ArmVmbus::needsService() const {
    if (!replies.empty()) return true;
    for (const auto &[id, channel] : channels) {
        (void)id;
        if (channel.work || channel.notification || !channel.pending.empty()) return true;
    }
    return false;
}
uint16_t ArmVmbus::channelMessage(uint32_t kind, std::span<const uint8_t> input) {
    if (kind != 5 && kind != 7 && kind != 8 && kind != 9 && kind != 11) return reject(5);
    if (input.size() < 12) return reject(5);
    // Windows posts full 240-byte channel envelopes, including dirty unused
    // bytes. Decode only declared data; keep compact-message validation strict.
    if (input.size() == 240) {
        if (kind == 5) input = input.first(148);
        else if (kind == 7) input = input.first(12);
        else if (kind == 11) input = input.first(16);
        else if (kind == 8) {
            size_t bytes = size_t(get(input, 16, 2));
            input = input.first(20 + std::min(bytes, size_t(216)));
        } else if (kind == 9) {
            auto remaining = gpadls.remainingBytes(uint32_t(get(input, 12, 4)));
            if (remaining && *remaining) input = input.first(16 + std::min(*remaining, size_t(224)));
        }
    }
    uint32_t id = uint32_t(get(input, 8, 4));
    if (kind == 9) {
        if (input.size() <= 16 || ((input.size() - 16) & 7)) return reject(5);
        const uint32_t gpadl = uint32_t(get(input, 12, 4));
        const auto owner = gpadls.owner(gpadl);
        if (!owner) return reject(5);
        id = *owner;
        auto result = gpadls.append(gpadl, input.subspan(16));
        if (result == ArmGpadl::Result::InUse) return reject(0x15);
        if (result != ArmGpadl::Result::Ok) {
            gpadls.remove(id, gpadl); response(10, id, gpadl, 0xc000000d);
        } else if (gpadls.byteSize(id, gpadl)) response(10, id, gpadl, 0);
        return 0;
    }
    auto it = channels.find(id);
    if (it == channels.end() || !it->second.offered) return reject(5);
    auto &channel = it->second;
    if (kind == 7) {
        if (input.size() != 12) return reject(5);
        close(channel); return 0;
    }
    if (kind == 8) {
        if (input.size() < 20 || ((input.size() - 20) & 7)) return reject(5);
        const uint32_t gpadl = uint32_t(get(input, 12, 4));
        auto result = gpadls.begin(id, gpadl, uint16_t(get(input, 16, 2)),
                                   uint16_t(get(input, 18, 2)), input.subspan(20));
        if (result != ArmGpadl::Result::Ok)
            response(10, id, gpadl, result == ArmGpadl::Result::Limit ? 0xc000009a : 0xc000000d);
        else if (gpadls.byteSize(id, gpadl)) response(10, id, gpadl, 0);
        return 0;
    }
    if (kind == 11) {
        if (input.size() != 16) return reject(5);
        const uint32_t gpadl = uint32_t(get(input, 12, 4));
        if (!gpadl) return reject(5);
        if (channel.rx && channel.ringGpadl == gpadl) return reject(0x15);
        auto result = gpadls.remove(id, gpadl);
        if (result == ArmGpadl::Result::WrongChannel) return reject(5);
        std::vector<uint8_t> message(12);
        put(message, 0, 12, 4); put(message, 8, gpadl, 4);
        replies.push_back({targetVp, targetSint, std::move(message)}); return 0;
    }
    if (input.size() != 148) return reject(5);
    const uint32_t openId = uint32_t(get(input, 12, 4)), gpadl = uint32_t(get(input, 16, 4));
    const uint32_t vp = uint32_t(get(input, 20, 4));
    const uint64_t split = get(input, 24, 4) * 4096;
    const auto size = gpadls.byteSize(id, gpadl);
    if (channel.rx || !size || *size > UINT32_MAX || split > *size ||
        (vp != UINT32_MAX && vp >= cpus)) {
        response(6, id, openId, 0xc000000d); return 0;
    }
    try {
        auto rx = std::make_unique<ArmVmbusRing>(gpadls, id, gpadl, 0, uint32_t(split));
        auto tx = std::make_unique<ArmVmbusRing>(gpadls, id, gpadl, split, uint32_t(*size - split));
        channel.rx = std::move(rx); channel.tx = std::move(tx);
    } catch (const std::runtime_error &) { response(6, id, openId, 0xc000000d); return 0; }
    channel.vp = vp; channel.ringGpadl = gpadl; channel.work = true; ++opens;
    response(6, id, openId, 0);
    if(channel.opened) channel.opened();
    return 0;
}
void ArmVmbus::service() {
    while (send && !replies.empty()) {
        auto &reply = replies.front();
        if (send(reply.vp, reply.sint, reply.payload)) break;
        replies.pop_front(); ++delivered;
    }
    for (auto &[id, channel] : channels) {
        if (!channel.rx) continue;
        channel.work = false;
        for (unsigned budget = 0; budget < 16; ++budget) {
            if (!channel.pending.empty()) {
                bool notify = false;
                auto result = channel.tx->push(channel.pending, notify);
                if (result == ArmVmbusRing::Result::Full) break;
                if (result != ArmVmbusRing::Result::Ok) { ++faults; close(channel); break; }
                channel.notification |= notify; channel.pending.clear(); ++packetsOut;
            }
            std::vector<uint8_t> packet;
            bool notify = false;
            auto result = channel.rx->pop(packet, notify);
            if (result == ArmVmbusRing::Result::Empty) { channel.work = false; break; }
            if (result != ArmVmbusRing::Result::Ok) { ++faults; close(channel); break; }
            channel.notification |= notify; ++packetsIn;
            channel.pending = channel.handler(packet);
            if (channel.pending.size() > 65535 * 8) { ++faults; close(channel); break; }
            channel.work = true; // A bounded follow-up also drains no-response streams.
        }
        if (channel.notification) {
            if (channel.vp == UINT32_MAX || (event && !event(channel.vp, targetSint, id)))
                channel.notification = false;
        }
    }
}
std::string ArmVmbus::report() const {
    std::ostringstream out;
    size_t offered = 0, open = 0;
    for (const auto &[id, channel] : channels) {
        (void)id; offered += channel.offered; open += bool(channel.rx);
    }
    out << "{\"connected\":" << (connected ? "true" : "false") << ",\"version\":" << version
        << ",\"targetVp\":" << targetVp << ",\"targetSint\":" << targetSint
        << ",\"contacts\":" << contacts << ",\"offerRequests\":" << offers << ",\"unloads\":" << unloads
        << ",\"deliveredReplies\":" << delivered << ",\"pendingReplies\":" << replies.size()
        << ",\"rejectedMessages\":" << rejected << ",\"offeredChannels\":" << offered << ",\"openChannels\":" << open
        << ",\"channelOffers\":" << channelOffers << ",\"channelOpens\":" << opens << ",\"channelCloses\":" << closes
        << ",\"gpadls\":" << gpadls.count() << ",\"packetsReceived\":" << packetsIn
        << ",\"packetsSent\":" << packetsOut << ",\"channelFaults\":" << faults << '}';
    return out.str();
}
}
