#include "arm_gpadl.hpp"
#include <algorithm>
#include <iostream>
#include <random>
#include <source_location>
#include <stdexcept>
using pc::ArmGpadl;
using R = ArmGpadl::Result;
static unsigned checks;
static void check(bool valid, std::source_location at = std::source_location::current()) {
    if (!valid) throw std::runtime_error("GPADL check at line " + std::to_string(at.line()));
    ++checks;
}
static void put(std::vector<uint8_t> &out, size_t at, uint64_t value, unsigned count = 8) {
    for (unsigned i = 0; i < count; ++i) out.at(at + i) = uint8_t(value >> (8 * i));
}
int main() {
    constexpr uint64_t base = 0x140000000ull;
    std::vector<uint8_t> ram(8 * 4096);
    for (size_t i = 0; i < ram.size(); ++i) ram[i] = uint8_t((i / 4096) * 31 + i * 7);
    const auto original = ram;
    ArmGpadl table(ram, base);
    std::vector<uint8_t> descriptor(48);
    put(descriptor, 0, 5000, 4); put(descriptor, 4, 4090, 4);
    put(descriptor, 8, (base >> 12) + 3); put(descriptor, 16, base >> 12);
    put(descriptor, 24, (base >> 12) + 5);
    put(descriptor, 32, 64, 4); put(descriptor, 36, 17, 4); put(descriptor, 40, (base >> 12) + 6);
    check(table.begin_descriptor(7, 42, 48, 2, std::span(descriptor).first(8)) == R::Ok);
    check(table.descriptor_owner(42) == 7 && !table.mapped_byte_count(7, 42));
    std::vector<uint8_t> output(5064, 0xa5);
    check(!table.read_mapped_bytes(7, 42, 0, output) && std::all_of(output.begin(), output.end(), [](uint8_t b) { return b == 0xa5; }));
    check(!table.write_mapped_bytes(7, 42, 0, output) && ram == original);
    check(table.append_descriptor(42, std::span(descriptor).subspan(8, 16)) == R::Ok);
    auto badTail = std::vector<uint8_t>(descriptor.begin() + 24, descriptor.end());
    put(badTail, 16, UINT64_MAX);
    check(table.append_descriptor(42, badTail) == R::Invalid && !table.mapped_byte_count(7, 42));
    check(table.append_descriptor(42, std::span(descriptor).subspan(24)) == R::Ok);
    check(table.mapped_byte_count(7, 42) == 5064 && !table.mapped_byte_count(8, 42));
    check(table.append_descriptor(42, {}) == R::InUse && table.append_descriptor(100, descriptor) == R::Missing);
    check(table.begin_descriptor(7, 42, 48, 2, descriptor) == R::InUse);
    // Independent oracle: a range starts in page 3's last six bytes, continues
    // through page 0 and page 5, followed by a separate subrange of page 6.
    std::vector<size_t> physical;
    for (size_t i = 4090; i < 4096; ++i) physical.push_back(3 * 4096 + i);
    for (size_t i = 0; i < 4096; ++i) physical.push_back(i);
    for (size_t i = 0; i < 898; ++i) physical.push_back(5 * 4096 + i);
    for (size_t i = 17; i < 81; ++i) physical.push_back(6 * 4096 + i);
    std::vector<uint8_t> expected;
    for (auto offset : physical) expected.push_back(ram[offset]);
    check(table.read_mapped_bytes(7, 42, 0, output) && output == expected);
    std::vector<uint8_t> replacement(4200);
    for (size_t i = 0; i < replacement.size(); ++i) replacement[i] = uint8_t(i * 13 + 5);
    auto expectedRam = ram;
    for (size_t i = 0; i < replacement.size(); ++i) expectedRam[physical[4 + i]] = replacement[i];
    check(table.write_mapped_bytes(7, 42, 4, replacement) && ram == expectedRam);
    check(!table.read_mapped_bytes(8, 42, 0, output) && !table.write_mapped_bytes(8, 42, 0, replacement));
    auto before = ram;
    check(!table.write_mapped_bytes(7, 42, 5000, replacement) && ram == before);
    check(!table.read_mapped_bytes(7, 42, 0, std::span(ram).first(16)) && ram == before);
    check(!table.write_mapped_bytes(7, 42, 0, std::span(ram).subspan(4, 16)) && ram == before);
    check(!table.read_mapped_bytes(7, 42, UINT64_MAX, output));
    check(table.read_mapped_bytes(7, 42, 5064, {}) && table.write_mapped_bytes(7, 42, 5064, {}));
    check(!table.read_mapped_bytes(7, 42, 5065, {}) && !table.write_mapped_bytes(7, 42, 5065, {}));
    check(table.teardown_descriptor(8, 42) == R::WrongChannel && table.mapped_byte_count(7, 42) == 5064);
    check(table.teardown_descriptor(7, 42) == R::Ok && table.teardown_descriptor(7, 42) == R::Missing && !table.descriptor_owner(42));
    for (unsigned kind = 0; kind < 7; ++kind) {
        auto bad = descriptor;
        if (kind == 0) put(bad, 0, 0, 4);
        if (kind == 1) put(bad, 4, 4096, 4);
        if (kind == 2) put(bad, 0, UINT32_MAX, 4);
        if (kind == 3) put(bad, 8, (base >> 12) - 1);
        if (kind == 4) put(bad, 8, (base >> 12) + 8);
        if (kind == 5) put(bad, 8, UINT64_MAX);
        if (kind == 6) bad.resize(40);
        check(table.begin_descriptor(7, 1, uint16_t(bad.size()), 2, bad) == R::Invalid && table.descriptor_count() == 0);
    }
    check(table.begin_descriptor(0, 1, 48, 2, descriptor) == R::Invalid);
    check(table.begin_descriptor(7, 0, 48, 2, descriptor) == R::Invalid);
    check(table.begin_descriptor(7, 1, 47, 2, {}) == R::Invalid);
    check(table.begin_descriptor(7, 1, 48, 4, {}) == R::Invalid);
    check(table.begin_descriptor(7, 1, 48, 0, {}) == R::Invalid);
    check(table.begin_descriptor(7, 1, 48, 2, std::span(descriptor).first(7)) == R::Invalid);
    check(table.begin_descriptor(7, 1, 16, 1, descriptor) == R::Invalid);
    std::vector<uint8_t> one(16);
    put(one, 0, 1, 4); put(one, 8, base >> 12);
    for (uint32_t id = 1; id <= ArmGpadl::maxDescriptors; ++id)
        check(table.begin_descriptor(id == 1 ? 8 : 7, id, 16, 1, one) == R::Ok);
    check(table.begin_descriptor(7, 100, 16, 1, one) == R::Limit);
    table.clear_channel(7);
    check(table.descriptor_count() == 1 && table.mapped_byte_count(8, 1) == 1);
    check(table.begin_descriptor(7, 2, 48, 2, {}) == R::Ok);
    check(table.append_descriptor(2, {}) == R::Invalid);
    check(table.append_descriptor(2, std::span(descriptor).first(1)) == R::Invalid);
    table.clear_channel(7); check(table.descriptor_count() == 1);
    table.clear_channel(8); check(table.descriptor_count() == 0);
    // Real header/body-sized fragments, including multiple in-flight mappings.
    std::vector<uint8_t> large(8 + 128 * 8);
    put(large, 0, 128 * 4096, 4);
    for (unsigned page = 0; page < 128; ++page) put(large, 8 + page * 8, (base >> 12) + (page * 3) % 8);
    check(table.begin_descriptor(7, 101, uint16_t(large.size()), 1, std::span(large).first(216)) == R::Ok);
    check(table.begin_descriptor(8, 102, 16, 1, std::span(one).first(8)) == R::Ok);
    check(table.append_descriptor(102, std::span(one).subspan(8)) == R::Ok && table.mapped_byte_count(8, 102) == 1);
    for (size_t at = 216; at < large.size();) {
        auto count = std::min<size_t>(224, large.size() - at);
        check(table.append_descriptor(101, std::span(large).subspan(at, count)) == R::Ok); at += count;
    }
    check(table.mapped_byte_count(7, 101) == 128 * 4096 && table.descriptor_owner(101) == 7);
    std::vector<uint8_t> crossed(25), oracle(25);
    for (size_t i = 0; i < 25; ++i) {
        auto logical = 4 * 4096 - 7 + i;
        oracle[i] = ram[((logical / 4096) * 3 % 8) * 4096 + logical % 4096];
    }
    check(table.read_mapped_bytes(7, 101, 4 * 4096 - 7, crossed) && crossed == oracle);
    table.clear_channel(7); table.clear_channel(8); check(table.descriptor_count() == 0);
    // Malformed wire descriptors must never expose a partial mapping or permit
    // a transfer outside the fully validated logical range (also run under ASan).
    std::mt19937 random(0x475041);
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        auto bytes = descriptor;
        bytes[random() % bytes.size()] ^= uint8_t(1u << (random() % 8));
        auto result = table.begin_descriptor(7, 100, 48, 2, bytes);
        if (result == R::Ok) {
            auto size = table.mapped_byte_count(7, 100); check(size && *size <= 6 * 4096);
            uint8_t last = 0;
            check(table.read_mapped_bytes(7, 100, *size - 1, {&last, 1}));
            check(!table.read_mapped_bytes(7, 100, *size, {&last, 1}));
            check(table.teardown_descriptor(7, 100) == R::Ok);
        } else check(result == R::Invalid && table.descriptor_count() == 0);
    }
    std::cout << checks << " ARM GPADL checks passed\n";
}
