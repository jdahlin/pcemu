#include "arm_vmbus_ring.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <stdexcept>
namespace pc {
namespace {
uint32_t get(std::span<const uint8_t> b, unsigned at, unsigned count = 4) {
    uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= uint32_t(b[at + i]) << (8 * i);
    return value;
}
}
ArmVmbusRing::ArmVmbusRing(ArmGpadl &m, uint32_t ch, uint32_t handle, uint64_t start, uint32_t bytes)
    : memory(m), channel(ch), id(handle), capacity(bytes >= 4096 ? bytes - 4096 : 0), offset(start), lease(0) {
    if (!memory.ringMapping(ch, handle, start, bytes)) throw std::runtime_error("Invalid VMBus ring mapping");
    lease = *memory.generation(ch, handle);
}
bool ArmVmbusRing::header(Header &out) const {
    if (memory.generation(channel, id) != lease) return false;
    std::array<uint8_t, 68> bytes{};
    if (!memory.read(channel, id, offset, bytes)) return false;
    out = {get(bytes, 0), get(bytes, 4), get(bytes, 8), get(bytes, 12), get(bytes, 64)};
    return out.write < capacity && out.read < capacity && !(out.write & 7) && !(out.read & 7);
}
bool ArmVmbusRing::copy(uint32_t at, std::span<uint8_t> bytes) const {
    const auto first = std::min<size_t>(bytes.size(), capacity - at);
    return memory.read(channel, id, offset + 4096 + at, bytes.first(first)) &&
           memory.read(channel, id, offset + 4096, bytes.subspan(first));
}
bool ArmVmbusRing::store(uint32_t at, std::span<const uint8_t> bytes) {
    const auto first = std::min<size_t>(bytes.size(), capacity - at);
    return memory.write(channel, id, offset + 4096 + at, bytes.first(first)) &&
           memory.write(channel, id, offset + 4096, bytes.subspan(first));
}
bool ArmVmbusRing::field(unsigned at, uint32_t value) {
    std::array<uint8_t, 4> bytes{};
    for (unsigned i = 0; i < 4; ++i) bytes[i] = uint8_t(value >> (8 * i));
    return memory.write(channel, id, offset + at, bytes);
}
bool ArmVmbusRing::validPacket(std::span<const uint8_t> packet) {
    if (packet.size() < 16 || (packet.size() & 7)) return false;
    const auto dataOffset = get(packet, 2, 2) * 8, length = get(packet, 4, 2) * 8;
    return dataOffset >= 16 && dataOffset <= length && length == packet.size();
}
ArmVmbusRing::Result ArmVmbusRing::push(std::span<const uint8_t> packet, bool &notify) {
    notify = false;
    Header h;
    if (!header(h) || !validPacket(packet) || packet.size() + 8 >= capacity) return Result::Invalid;
    const auto used = h.write >= h.read ? h.write - h.read : capacity - h.read + h.write;
    const auto total = uint32_t(packet.size() + 8);
    if (capacity - used <= total) {
        if ((h.features & 1) && !field(12, total)) return Result::Invalid;
        return Result::Full;
    }
    // Snapshot caller memory before any ring write, including aliases of RAM.
    std::vector<uint8_t> bytes(packet.begin(), packet.end());
    // The trailer is the old write index in the high half, low half reserved.
    bytes.resize(total);
    for (unsigned i = 0; i < 4; ++i) bytes[total - 4 + i] = uint8_t(h.write >> (8 * i));
    if (!store(h.write, bytes)) return Result::Invalid;
    std::atomic_thread_fence(std::memory_order_release);
    if (!field(0, (h.write + uint64_t(total)) % capacity)) return Result::Invalid;
    if ((h.features & 1) && !field(12, 0)) return Result::Invalid;
    notify = !h.mask && !used;
    return Result::Ok;
}
ArmVmbusRing::Result ArmVmbusRing::pop(std::vector<uint8_t> &packet, bool &notify) {
    notify = false;
    Header h;
    if (!header(h)) return Result::Invalid;
    const auto used = h.write >= h.read ? h.write - h.read : capacity - h.read + h.write;
    if (!used) return Result::Empty;
    if (used < 24) return Result::Invalid;
    std::atomic_thread_fence(std::memory_order_acquire);
    std::array<uint8_t, 16> descriptor{};
    if (!copy(h.read, descriptor)) return Result::Invalid;
    const auto length = get(descriptor, 4, 2) * 8;
    if (length < 16 || length + 8 > used) return Result::Invalid;
    std::vector<uint8_t> bytes(length);
    if (!copy(h.read, bytes) || !validPacket(bytes)) return Result::Invalid;
    std::atomic_thread_fence(std::memory_order_release);
    if (!field(4, (h.read + uint64_t(length) + 8) % capacity)) return Result::Invalid;
    const auto before = capacity - used, after = before + length + 8;
    notify = (h.features & 1) && h.pending && before <= h.pending && after > h.pending;
    packet = std::move(bytes);
    return Result::Ok;
}
}
