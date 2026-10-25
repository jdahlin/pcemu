#include "arm_vmbus_ring.hpp"
#include <algorithm>
#include <deque>
#include <iostream>
#include <random>
#include <source_location>
#include <stdexcept>
using pc::ArmGpadl;
using pc::ArmVmbusRing;
using R = ArmVmbusRing::Result;
static unsigned checks;
void check(bool ok, std::source_location at = std::source_location::current()) {
    if (!ok) throw std::runtime_error("Ring check at line " + std::to_string(at.line()));
    ++checks;
}
void put(std::span<uint8_t> bytes, size_t at, uint64_t value, unsigned count = 4) {
    for (unsigned i = 0; i < count; ++i) bytes[at + i] = uint8_t(value >> (8 * i));
}
uint32_t get(std::span<const uint8_t> b, size_t at) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= uint32_t(b[at + i]) << (8 * i);
    return value;
}
int main() {
    constexpr uint64_t base = 0x40000000;
    std::vector<uint8_t> ram(8 * 4096), descriptor(32);
    put(descriptor, 0, 12288);
    put(descriptor, 8, (base >> 12) + 3, 8);
    put(descriptor, 16, base >> 12, 8);
    put(descriptor, 24, (base >> 12) + 5, 8);
    ArmGpadl table(ram, base);
    check(table.begin(7, 42, 32, 1, descriptor) == ArmGpadl::Result::Ok);
    check(table.ringMapping(7, 42, 0, 12288));
    check(!table.ringMapping(8, 42, 0, 12288));
    check(!table.ringMapping(7, 42, 1, 8192));
    check(!table.ringMapping(7, 42, 4096, 12288));
    auto header = std::span(ram).subspan(3 * 4096, 4096);
    ArmVmbusRing ring(table, 7, 42, 0, 12288);
    std::vector<uint8_t> packet(32), output{0xaa};
    put(packet, 0, 6, 2); put(packet, 2, 2, 2); put(packet, 4, 4, 2);
    put(packet, 8, 0x123456789abcdef0ull, 8);
    for (unsigned i = 16; i < 32; ++i) packet[i] = uint8_t(i);
    bool notify = true;
    check(ring.pop(output, notify) == R::Empty && !notify && output == std::vector<uint8_t>{0xaa});
    put(header, 0, 8184); put(header, 4, 8184);
    check(ring.push(packet, notify) == R::Ok && notify && get(header, 0) == 32);
    check(std::equal(packet.begin(), packet.begin() + 8, ram.begin() + 5 * 4096 + 4088));
    check(std::equal(packet.begin() + 8, packet.end(), ram.begin()));
    check(get(ram, 24) == 0 && get(ram, 28) == 8184); // Independent physical trailer oracle.
    check(ring.pop(output, notify) == R::Ok && !notify && output == packet && get(header, 4) == 32);
    put(header, 8, 1);
    check(ring.push(packet, notify) == R::Ok && !notify);
    check(ring.pop(output, notify) == R::Ok && !notify && output == packet);
    put(header, 0, 4088); put(header, 4, 4088); put(header, 8, 0);
    check(ring.push(packet, notify) == R::Ok && notify);
    check(std::equal(packet.begin(), packet.begin() + 8, ram.begin() + 4088));
    check(std::equal(packet.begin() + 8, packet.end(), ram.begin() + 5 * 4096));
    check(ring.pop(output, notify) == R::Ok && output == packet);
    put(header, 0, 0); put(header, 4, 40); put(header, 12, 77); put(header, 64, 0);
    auto noFeature = ram;
    check(ring.push(packet, notify) == R::Full && !notify && ram == noFeature);
    put(header, 8, 1);
    // Exactly enough nominal free space is full: keep the empty/full distinction.
    put(header, 0, 0); put(header, 4, 40); put(header, 64, 1);
    const auto beforeFull = ram;
    check(ring.push(packet, notify) == R::Full && !notify && get(header, 12) == 40);
    auto expectedFull = beforeFull; put(expectedFull, 3 * 4096 + 12, 40);
    check(ram == expectedFull);
    check(table.write(7, 42, 4096 + 40, packet));
    check(ring.pop(output, notify) == R::Ok && notify && output == packet && get(header, 4) == 80);
    check(ring.push(packet, notify) == R::Ok && !notify && get(header, 12) == 0);
    // Malformed packet/index input never consumes data or modifies output.
    for (unsigned at : {2u, 4u}) {
        auto bad = packet; put(bad, at, at == 2 ? 1 : 3, 2);
        const auto before = ram;
        check(ring.push(bad, notify) == R::Invalid && !notify && ram == before);
    }
    for (auto index : {1u, 8192u, 0xffffffffu}) {
        put(header, 0, index); auto before = ram; auto old = output;
        check(ring.pop(output, notify) == R::Invalid && !notify && ram == before && output == old);
        check(ring.push(packet, notify) == R::Invalid && !notify && ram == before);
    }
    put(header, 0, 8); put(header, 4, 0);
    auto before = ram;
    check(ring.pop(output, notify) == R::Invalid && ram == before);
    put(header, 0, 40); put(header, 4, 0);
    auto bad = packet; put(bad, 2, 5, 2);
    check(table.write(7, 42, 4096, bad)); before = ram;
    check(ring.pop(output, notify) == R::Invalid && ram == before);
    // Exercise repeated wraps/backpressure against an independent FIFO oracle.
    std::fill(ram.begin(), ram.end(), 0); put(header, 64, 1);
    std::mt19937 random(0x485652);
    std::deque<std::vector<uint8_t>> fifo;
    for (unsigned i = 0; i < 3000; ++i) {
        if (random() % 3) {
            std::vector<uint8_t> next(16 + (1 + random() % 96) * 8);
            put(next, 0, 6, 2); put(next, 2, 2, 2); put(next, 4, next.size() / 8, 2);
            for (size_t n = 8; n < next.size(); ++n) next[n] = uint8_t(random());
            size_t occupied = 0;
            for (const auto &entry : fifo) occupied += entry.size() + 8;
            const bool full = 8192 - occupied <= next.size() + 8;
            check(ring.push(next, notify) == (full ? R::Full : R::Ok));
            if (!full) fifo.push_back(next);
        } else {
            const auto result = ring.pop(output, notify);
            check(result == (fifo.empty() ? R::Empty : R::Ok));
            if (!fifo.empty()) { check(output == fifo.front()); fifo.pop_front(); }
        }
    }
    // Teardown and ID reuse invalidate existing rings, even for the same PFNs.
    const auto generation = table.generation(7, 42);
    check(table.remove(7, 42) == ArmGpadl::Result::Ok);
    check(table.begin(7, 42, 32, 1, descriptor) == ArmGpadl::Result::Ok);
    check(table.generation(7, 42) != generation); before = ram;
    check(ring.push(packet, notify) == R::Invalid && ram == before);
    check(ring.pop(output, notify) == R::Invalid && ram == before);
    auto duplicate = descriptor; put(duplicate, 24, base >> 12, 8);
    check(table.begin(7, 43, 32, 1, duplicate) == ArmGpadl::Result::Ok);
    check(!table.ringMapping(7, 43, 0, 8192));
    bool rejected = false;
    try { ArmVmbusRing alias(table, 7, 43, 0, 8192); } catch (const std::runtime_error &) { rejected = true; }
    check(rejected);
    check(table.begin(7, 44, 32, 1, std::span(descriptor).first(16)) == ArmGpadl::Result::Ok);
    check(!table.generation(7, 44) && !table.ringMapping(7, 44, 0, 8192));
    auto partial = descriptor; put(partial, 0, 8192); put(partial, 4, 8);
    check(table.begin(7, 45, 32, 1, partial) == ArmGpadl::Result::Ok);
    check(!table.ringMapping(7, 45, 0, 8192));
    std::vector<uint8_t> multi(40);
    put(multi, 0, 8192); put(multi, 8, (base >> 12) + 3, 8); put(multi, 16, base >> 12, 8);
    put(multi, 24, 4096); put(multi, 32, (base >> 12) + 5, 8);
    check(table.begin(7, 46, 40, 2, multi) == ArmGpadl::Result::Ok);
    check(table.ringMapping(7, 46, 0, 12288));
    ArmVmbusRing multiRing(table, 7, 46, 0, 12288);
    put(header, 0, 0); put(header, 4, 0);
    check(multiRing.push(packet, notify) == R::Ok);
    check(multiRing.pop(output, notify) == R::Ok && output == packet);
    for (unsigned i = 0; i < 128; ++i) {
        put(header, 0, 40); put(header, 4, 0);
        auto corrupt = packet;
        if (i & 1) put(corrupt, 2, i & 2 ? 0 : 1, 2);
        else put(corrupt, 4, i & 2 ? 0 : 65535, 2);
        check(table.write(7, 46, 4096, corrupt));
        const auto previous = ram, oldPacket = output;
        check(multiRing.pop(output, notify) == R::Invalid && !notify && ram == previous && output == oldPacket);
    }
    std::cout << checks << " VMBus ring checks passed\n";
}
