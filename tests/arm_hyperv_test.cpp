#include "arm_hyperv.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <source_location>
#include <vector>
using namespace pc;
static unsigned checks = 0;
static void check(bool value, std::source_location location = std::source_location::current()) {
    if (!value) throw std::runtime_error("Hyper-V check " + std::to_string(checks + 1) + " at line " + std::to_string(location.line()));
    ++checks;
}
static void put(std::vector<uint8_t> &b, size_t at, uint64_t value, size_t size = 8) {
    for (size_t i = 0; i < size; ++i) b.at(at + i) = uint8_t(value >> (8 * i));
}
static uint64_t get(const std::vector<uint8_t> &b, size_t at) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value |= uint64_t(b.at(at + i)) << (8 * i);
    return value;
}
int main() {
    constexpr uint64_t base = 0x140000000ull, one = 1ull << 32;
    constexpr uint64_t features = (1ull << 1) | (1ull << 5) | (1ull << 6) | (1ull << 49) | (1ull << 51) | (1ull << 52);
    std::vector<uint8_t> memory(8192);
    ArmHyperV off(false, memory, base, 2), hv(true, memory, base, 2);
    ArmHyperV::Registers r{};
    r[0] = 0x84000009;
    check(!hv.call(0, r, 0) && r[0] == 0x84000009); // PSCI reset stays with PSCI
    r[0] = 0x8600ff01;
    check(!off.call(0, r, 0) && r[0] == 0x8600ff01);
    check(hv.call(0, r, 0));
    check(r[0] == 0x4d32ba58 && r[1] == 0xcd244764 && r[2] == 0x8eef6c75 && r[3] == 0x16597024);
    r = {}; r[0] = 0x80000000;
    check(hv.call(0, r, 0) && r[0] == 0x10002);
    r[0] = 0x80000001; r[1] = 0x46000001;
    check(hv.call(0, r, 0) && r[0] == 0);
    r[0] = 0x80000001; r[1] = 0x80008000;
    check(hv.call(0, r, 0) && r[0] == uint64_t(-1)); // No invented mitigation support
    auto fastRead = [&](uint32_t name, bool smccc = true, unsigned vp = 0) {
        r.fill(0xcccccccccccccccc);
        r[0] = smccc ? 0x46000001 : one | 0x10050;
        auto start = smccc ? 2u : 1u;
        if (smccc) r[1] = one | 0x10050;
        r[start] = uint64_t(-1); r[start + 1] = 0xfffffffe; r[start + 2] = name;
        check(hv.call(smccc ? 0 : 1, r, vp));
    };
    fastRead(0x200);
    check(r[0] == one && r[6] == features && r[7] == 256);
    check(r[2] == uint64_t(-1) && r[4] == 0x200 && r[8] == 0xcccccccccccccccc);
    fastRead(0x200, false);
    check(r[0] == one && r[15] == features && r[16] == 256);
    check(r[5] == 0xcccccccccccccccc); // Legacy output is NOT after the input
    fastRead(0x90006);
    check(r[0] == one && r[6] == 0x31237648 && r[7] == 0); // Discovery before OS identity
    fastRead(0x90003);
    check(r[0] == 6); // Identity required for non-early registers
    auto fastWrite = [&](uint64_t id, unsigned vp = 0, uint64_t high = 0) {
        r = {}; r[0] = 0x46000001; r[1] = one | 0x10051;
        r[2] = uint64_t(-1); r[3] = 0xfffffffe; r[4] = 0x90002;
        r[6] = id; r[7] = high;
        check(hv.call(0, r, vp));
    };
    fastWrite(0x1000a000066bbull);
    check(r[0] == one);
    fastRead(0x90002, false, 1);
    check(r[0] == one && r[15] == 0x1000a000066bbull);
    fastRead(0x90003, true, 1);
    check(r[0] == one && r[6] == 1);
    fastWrite(42, 1); check(r[0] == 6);
    fastWrite(42, 0, 1); check(r[0] == 5);
    fastRead(0x90004); check(r[0] == one && r[6] > 0 && r[7] == 0);
    fastRead(0x201); check(r[0] == one && r[6] == 0xffffffff00000000ull && r[7] == 0);
    fastRead(0x202); check(r[0] == one && r[6] == 0x200000002ull);
    // The partition info page is allocated by the guest, not a fabricated GPA.
    auto page = [&](uint64_t value, uint64_t high = 0) {
        r = {}; r[0] = 0x46000001; r[1] = one | 0x10051;
        r[2] = uint64_t(-1); r[3] = 0xfffffffe; r[4] = 0x90015;
        r[6] = value; r[7] = high;
        check(hv.call(0, r, 0));
    };
    fastRead(0x90015); check(r[0] == one && r[6] == 0);
    std::fill(memory.begin() + 4096, memory.end(), 0xaa);
    page(base + 4096 + 1); check(r[0] == one);
    check(std::all_of(memory.begin() + 4096, memory.end(), [](uint8_t b) { return b == 0; }));
    fastRead(0x90015); check(r[6] == base + 4096 + 1);
    memory[4096] = 0x42;
    for (uint64_t bad : {base + 4096 + 3, base + 8192 + 1, base - 4096 + 1}) {
        page(bad); check(r[0] == 5 && memory[4096] == 0x42);
    }
    page(base + 4096 + 1, 1); check(r[0] == 5 && memory[4096] == 0x42);
    fastRead(0x90015); check(r[6] == base + 4096 + 1);
    page(0); check(r[0] == one && memory[4096] == 0x42);
    fastRead(0x90015); check(r[6] == 0);
    auto writeVp = [&](uint32_t name, uint64_t value, unsigned vp = 0, uint64_t high = 0) {
        r = {}; r[0] = 0x46000001; r[1] = one | 0x10051;
        r[2] = uint64_t(-1); r[3] = 0xfffffffe; r[4] = name;
        r[6] = value; r[7] = high;
        check(hv.call(0, r, vp));
    };
    writeVp(0x90016, 1); check(r[0] == one);
    fastRead(0x90016); check(r[6] == 1);
    fastRead(0x90016, false, 1); check(r[15] == 0); // Per-VP, not partition-wide
    writeVp(0x90016, 2); check(r[0] == 5);
    writeVp(0x90016, 1, 0, 1); check(r[0] == 5);
    fastRead(0x90016); check(r[6] == 1);
    writeVp(0x40400, 0xfffff80012345000ull); check(r[0] == one);
    fastRead(0x40400); check(r[6] == 0xfffff80012345000ull);
    fastRead(0x40400, false, 1); check(r[15] == 0);
    writeVp(0x40400, 123); check(r[0] == 5);
    writeVp(0x40400, 0, 0, 1); check(r[0] == 5);
    fastRead(0x40400); check(r[6] == 0xfffff80012345000ull);
    fastRead(0x40401); check(r[6] == 0);
    writeVp(0x40401, 1); check(r[0] == 5);
    // Memory ABI above 4 GiB, partial completion and resume.
    put(memory, 0, uint64_t(-1)); put(memory, 8, 0xfffffffe);
    put(memory, 16, 0x90003, 4); put(memory, 20, 0xffffffff, 4);
    auto memoryRead = [&](uint64_t control, uint64_t input = base, uint64_t output = base + 4096) {
        r = {}; r[0] = 0x46000001; r[1] = control; r[2] = input; r[3] = output;
        check(hv.call(0, r, 1));
    };
    std::fill(memory.begin() + 4096, memory.end(), 0xaa);
    memoryRead((2ull << 32) | 0x50);
    check(r[0] == (one | 5) && get(memory, 4096) == 1 && get(memory, 4112) == 0xaaaaaaaaaaaaaaaaull);
    put(memory, 20, 0x90002, 4);
    memoryRead((2ull << 32) | (1ull << 48) | 0x50);
    check(r[0] == 2 * one && get(memory, 4096) == 1 && get(memory, 4112) == 0x1000a000066bbull);
    // Cross-page, unaligned, overlapping and unmapped parameter buffers fail safely.
    for (auto input : {base + 1, base + 4096 - 8, base - 8, base + 8192}) {
        memoryRead(one | 0x50, input); check(r[0] == 4);
    }
    memoryRead(one | 0x50, base, base + 8); check(r[0] == 4);
    memoryRead(one | 0x50, base, base + 4097); check(r[0] == 4);
    for (uint64_t bad : {uint64_t(0x50), one | 0x20050, one | 0x80000050,
                         one | 0x8000050, one | (1ull << 44) | 0x50,
                         one | (1ull << 60) | 0x50, one | (1ull << 48) | 0x50}) {
        memoryRead(bad); check(r[0] == 3);
    }
    put(memory, 0, 123); memoryRead(one | 0x50); check(r[0] == 6);
    put(memory, 0, uint64_t(-1)); put(memory, 8, 0); memoryRead(one | 0x50); check(r[0] == 14);
    put(memory, 8, 0xfffffffe); put(memory, 12, 1, 4); memoryRead(one | 0x50); check(r[0] == 5);
    put(memory, 12, 0, 4);
    // Slow SetVpRegisters, read-only protection and reserved association bytes.
    put(memory, 16, 0x90002, 4); put(memory, 20, 0, 4); put(memory, 24, 0);
    put(memory, 32, 321); put(memory, 40, 0);
    r = {}; r[0] = one | 0x51; r[1] = base;
    check(hv.call(1, r, 0) && r[0] == one);
    fastRead(0x90002); check(r[6] == 321);
    put(memory, 16, 0x90003, 4);
    r = {}; r[0] = one | 0x51; r[1] = base;
    check(hv.call(1, r, 0) && r[0] == 5);
    put(memory, 16, 0x90002, 4); put(memory, 24, 1);
    r = {}; r[0] = one | 0x51; r[1] = base;
    check(hv.call(1, r, 0) && r[0] == 5);
    r = {}; r[0] = 0x46000001; r[1] = 0xdead;
    check(hv.call(0, r, 0) && r[0] == 2);
    r = {}; r[0] = 0x46000001; r[1] = one | 0x10050;
    check(!off.call(0, r, 0) && r[0] == 0x46000001);
    check(!hv.call(2, r, 0));
    // Extended query has no input and exactly eight bytes of output.
    for (bool smccc : {false, true}) {
        for (bool fast : {false, true}) {
            r.fill(0xcccccccccccccccc);
            r[0] = smccc ? 0x46000001 : (fast ? 0x18001 : 0x8001);
            if (smccc) r[1] = fast ? 0x18001 : 0x8001;
            r[smccc ? 2 : 1] = uint64_t(-1); // Unused input must not be dereferenced.
            r[smccc ? 3 : 2] = base + 8192 - 8;
            std::fill(memory.begin() + 8176, memory.end(), 0xaa);
            check(hv.call(smccc ? 0 : 1, r, 1) && r[0] == 0);
            unsigned out = smccc ? 2 : 15;
            if (fast) check(r[out] == 0 && r[out + 1] == 0 && get(memory, 8184) == 0xaaaaaaaaaaaaaaaaull);
            else check(get(memory, 8184) == 0 && get(memory, 8176) == 0xaaaaaaaaaaaaaaaaull);
        }
    }
    for (auto address : {base - 8, base + 1, base + 8192, uint64_t(-8)}) {
        r = {}; r[0] = 0x8001; r[2] = address;
        check(hv.call(1, r, 0) && r[0] == 4);
    }
    for (auto extra : {1ull << 17, 1ull << 31, 1ull << 32, 1ull << 48, 1ull << 63}) {
        r = {}; r[0] = 0x18001 | extra; r[15] = 123;
        check(hv.call(1, r, 0) && r[0] == 3 && r[15] == 123);
    }
    r = {}; r[0] = 0x18001;
    check(hv.call(1, r, 2) && r[0] == 3);
    for (unsigned code : {0x8002, 0x8003, 0x8004, 0x8005, 0x8006}) {
        r = {}; r[0] = code;
        check(hv.call(1, r, 0) && r[0] == 2); // No extended services falsely advertised.
    }
    fastWrite(0); fastRead(0x90003); check(r[0] == 6);
    fastRead(0x90016); check(r[0] == 6);
    r = {}; r[0] = 0x18001; r[15] = 123;
    check(hv.call(1, r, 0) && r[0] == 6 && r[15] == 123);
    check(hv.report().find("\"code\":32769,\"count\":15,\"failures\":11") != std::string::npos);
    writeVp(0x40400, 0); check(r[0] == one); // Early register may be written without ID
    fastRead(0x40400); check(r[0] == one && r[6] == 0);
    ArmHyperV fresh(true, memory, base, 2);
    r = {}; r[0] = one | 0x10050; r[1] = uint64_t(-1); r[2] = 0xfffffffe; r[3] = 0x40400;
    check(fresh.call(1, r, 0) && r[0] == one && r[15] == 0); // VM recreation resets state
    check(hv.report().find("\"guestOsId\":0") != std::string::npos);
    check(off.report().find("\"disabledCalls\":1") != std::string::npos);
    uint64_t ticks = 1000;
    ArmHyperV timed(true, memory, base, 2, [&] { return ticks; });
    auto timeRead = [&](unsigned vp, bool smccc) {
        r = {}; r[0] = smccc ? 0x46000001 : one | 0x10050;
        if (smccc) r[1] = one | 0x10050;
        unsigned in = smccc ? 2 : 1;
        r[in] = uint64_t(-1); r[in + 1] = 0xfffffffe; r[in + 2] = 0x90004;
        check(timed.call(smccc ? 0 : 1, r, vp) && r[0] == one);
        check(r[smccc ? 7 : 16] == 0);
        return r[smccc ? 6 : 15];
    };
    check(timed.report().find("\"referenceTime100ns\":0") != std::string::npos);
    ticks += 10000000; // One second is exactly ten million 100 ns units.
    check(timeRead(0, true) == 10000000); // Early read, no identity.
    check(timeRead(1, false) == 10000001); // Shared ordering, equal-resolution sample.
    ticks += 10000000;
    check(timeRead(1, true) == 20000000); // No per-VP epoch or clock-rate drift.
    ticks -= 500;
    check(timeRead(0, false) == 20000001); // Defensive monotonicity for a bad source.
    r = {}; r[0] = one | 0x10051; r[1] = uint64_t(-1); r[2] = 0xfffffffe;
    r[3] = 0x90004; r[5] = 123;
    check(timed.call(1, r, 0) && r[0] == 5); // Read-only, no identity needed to reject.
    ticks += 10000000;
    check(timeRead(0, true) == 29999500); // Failed write did not alter the epoch.
    ArmHyperV newClock(true, memory, base, 2, [&] { return ticks; });
    check(newClock.report().find("\"referenceTime100ns\":0") != std::string::npos);
    ArmHyperV crash(true, memory, base, 2);
    auto crashWrite = [&](uint32_t name, uint64_t value, unsigned vp = 0, uint64_t high = 0) {
        r = {}; r[0] = one | 0x10051; r[1] = uint64_t(-1); r[2] = 0xfffffffe;
        r[3] = name; r[5] = value; r[6] = high;
        check(crash.call(1, r, vp)); return r[0];
    };
    auto crashRead = [&](uint32_t name, unsigned vp = 0) {
        r = {}; r[0] = 0x46000001; r[1] = one | 0x10050;
        r[2] = uint64_t(-1); r[3] = 0xfffffffe; r[4] = name;
        check(crash.call(0, r, vp) && r[0] == one && r[7] == 0);
        return r[6];
    };
    constexpr uint64_t notify = 1ull << 63, message = 1ull << 62;
    check(crashRead(0x215) == (notify | message));
    for (unsigned i = 0; i < 5; ++i) {
        check(crashRead(0x210 + i) == 0);
        check(crashWrite(0x210 + i, 11 + i, 1) == one);
        check(crashRead(0x210 + i, 1) == 11 + i);
        check(crashRead(0x210 + i, 0) == 0); // Per-VP banks.
    }
    check(crash.takeCrashReport().empty());
    check(crashWrite(0x215, notify, 1) == one);
    auto event = crash.takeCrashReport();
    check(event.find("\"parameters\":[11,12,13,14,15]") != std::string::npos);
    check(event.find("\"vp\":1") != std::string::npos);
    check(crash.takeCrashReport().empty());
    check(crashWrite(0x210, 999, 1) == one);
    check(crash.report().find("\"parameters\":[11,12,13,14,15]") != std::string::npos);
    check(crashWrite(0x215, 0) == one && crash.takeCrashReport().empty());
    check(crashWrite(0x215, message) == 5);
    check(crashWrite(0x215, notify | 1) == 5);
    check(crashWrite(0x215, notify, 0, 1) == 5);
    check(crashWrite(0x210, 42, 0, 1) == 5);
    check(crashRead(0x210) == 0);
    check(crashWrite(0x213, base + 4094) == one); // Cross-page is valid for a crash message.
    check(crashWrite(0x214, 4) == one);
    memory[4094] = 0; memory[4095] = 0x1b; memory[4096] = 0xff; memory[4097] = 'A';
    check(crashWrite(0x215, notify | message | (3ull << 58) | (1ull << 61)) == one);
    memory[4094] = 99;
    event = crash.takeCrashReport();
    check(event.find("\"messageHex\":\"001bff41\"") != std::string::npos);
    check(event.find("\"notifications\":2") != std::string::npos);
    check(crashRead(0x215) == (notify | message)); // Reads return capabilities, not the last command.
    for (auto bad : {uint64_t(4097), uint64_t(-1)}) {
        check(crashWrite(0x214, bad) == one);
        check(crashWrite(0x215, notify | message) == 5);
        check(crash.takeCrashReport().empty());
    }
    check(crashWrite(0x214, 4) == one);
    for (auto bad : {base - 1, base + memory.size() - 3, uint64_t(-2)}) {
        check(crashWrite(0x213, bad) == one);
        check(crashWrite(0x215, notify | message) == 5);
    }
    check(crashWrite(0x213, base) == one);
    check(crashWrite(0x214, 4096) == one);
    check(crashWrite(0x215, notify | message) == one);
    check(crash.takeCrashReport().size() < 8500); // Bounded even at maximum payload.
    ArmHyperV crashReset(true, memory, base, 2);
    check(crashReset.takeCrashReport().empty());
    check(crashReset.report().find("\"notifications\":0") != std::string::npos);
    uint64_t synced = 0;
    unsigned syncCalls = 0;
    ArmHyperV syncing(true, memory, base, 2);
    r = {}; r[0] = one | 0x10051; r[1] = uint64_t(-1); r[2] = 0xfffffffe;
    r[3] = 0x90002; r[5] = 123;
    check(syncing.call(1, r, 0) && r[0] == one);
    auto sync = [&](uint64_t flags, uint64_t mask, bool smccc, uint64_t control = 0x10019, unsigned vp = 0) {
        r.fill(0x5555555555555555);
        r[0] = smccc ? 0x46000001 : control;
        if (smccc) r[1] = control;
        unsigned in = smccc ? 2 : 1;
        r[in] = flags; r[in + 1] = mask;
        check(syncing.call(smccc ? 0 : 1, r, vp));
        check(r[in] == flags && r[in + 1] == mask && r[17] == 0x5555555555555555);
        return r[0];
    };
    check(sync(1, 0, false) == 2); // No backend: never fake successful synchronization.
    syncing.setContextSynchronizer([&](uint64_t targets) { synced = targets; ++syncCalls; return true; });
    check(sync(1, 0, false) == 0 && synced == 3 && syncCalls == 1);
    check(sync(0, 2, true, 0x10019, 1) == 0 && synced == 2 && syncCalls == 2);
    check(sync(0, 0, true) == 0 && synced == 0 && syncCalls == 3);
    for (auto bad : {uint64_t(0x30019), one | 0x10019, (1ull << 48) | 0x10019})
        check(sync(1, 0, false, bad) == 3 && syncCalls == 3);
    check(sync(2, 0, true) == 5 && syncCalls == 3);
    check(sync(1, 1, false) == 5 && syncCalls == 3);
    check(sync(0, 4, true) == 14 && syncCalls == 3);
    check(sync(1, 0, true, 0x10019, 2) == 3 && syncCalls == 3);
    check(sync(1, 0, false, 0x19) == 4 && syncCalls == 3); // Slow unaligned GPA.
    syncing.setContextSynchronizer([](uint64_t) { return false; });
    check(sync(1, 0, true) == 0x71);
    check(syncing.report().find("\"contextSyncRequests\":3") != std::string::npos);
    check(syncing.report().find("\"contextSyncTargetMask\":0") != std::string::npos);
    ArmHyperV earlySync(true, memory, base, 1);
    r = {}; r[0] = 0x10019; r[1] = 1;
    check(earlySync.call(1, r, 0) && r[0] == 6);
    ArmHyperV ex(true, memory, base, 2);
    r = {}; r[0] = one | 0x10051; r[1] = uint64_t(-1); r[2] = 0xfffffffe;
    r[3] = 0x90002; r[5] = 456;
    check(ex.call(1, r, 0) && r[0] == one);
    unsigned exCalls = 0;
    uint64_t exTargets = 0;
    ex.setContextSynchronizer([&](uint64_t targets) { ++exCalls; exTargets = targets; return true; });
    auto invokeEx = [&](bool smccc, bool fast, unsigned banks = 1, uint64_t address = base,
                        uint64_t extraControl = 0, unsigned vp = 0, uint16_t code = 0x1a) {
        r.fill(0xcccccccccccccccc);
        auto control = uint64_t(code) | (fast ? 0x10000 : 0) | (uint64_t(banks) << 17) | extraControl;
        r[0] = smccc ? 0x46000001 : control;
        if (smccc) r[1] = control;
        unsigned in = smccc ? 2 : 1;
        if (fast) {
            for (unsigned i = 0; i < std::min(16u, 3 + banks); ++i) r[in + i] = get(memory, i * 8);
        } else { r[in] = address; r[in + 1] = uint64_t(-1); } // No output: ignored GPA.
        auto previous = r;
        check(ex.call(smccc ? 0 : 1, r, vp));
        check(std::equal(r.begin() + 1, r.end(), previous.begin() + 1));
        return r[0];
    };
    auto setVpSet = [&](uint64_t flags, uint64_t format, uint64_t validBanks, uint64_t contents) {
        std::fill(memory.begin(), memory.end(), 0);
        put(memory, 0, flags); put(memory, 8, format); put(memory, 16, validBanks); put(memory, 24, contents);
    };
    for (bool smccc : {false, true}) for (bool fast : {false, true}) {
        setVpSet(0, 0, 1, 2);
        check(invokeEx(smccc, fast) == 0 && exTargets == 2);
        setVpSet(0, 1, 0, 0);
        check(invokeEx(smccc, fast, 0) == 0 && exTargets == 3);
        setVpSet(0, 0, 0, 0);
        check(invokeEx(smccc, fast, 0) == 0 && exTargets == 0);
        // Fixed SyncContext in memory has the same two input words as fast.
        setVpSet(1, 0, 0, 0);
        check(invokeEx(smccc, false, 0, base, 0, 0, 0x19) == 0 && exTargets == 3);
    }
    unsigned beforeInvalid = exCalls;
    setVpSet(1, 0, 1, 2); check(invokeEx(true, true) == 5);
    setVpSet(0, 2, 1, 2); check(invokeEx(true, true) == 5);
    setVpSet(0, 1, 1, 0); check(invokeEx(false, false) == 5);
    setVpSet(0, 0, 1, 4); check(invokeEx(false, true) == 14);
    setVpSet(0, 0, 1ull << 63, 1); check(invokeEx(true, false) == 14);
    setVpSet(0, 0, 3, 0); check(invokeEx(false, true, 1) == 3); // Truncated bank array.
    setVpSet(0, 0, 1, 0); check(invokeEx(false, false, 2) == 3); // Excess header bytes.
    for (auto bit : {1ull << 27, 1ull << 31, 1ull << 32, 1ull << 48, 1ull << 63})
        check(invokeEx(true, true, 1, base, bit) == 3);
    for (auto address : {base - 8, base + 1, base + 4096 - 24, base + 8192})
        check(invokeEx(false, false, 1, address) == 4);
    check(invokeEx(true, true, 65) == 3);
    check(invokeEx(false, true, 14) == 3); // Declared input exceeds the fast register window.
    check(invokeEx(true, false, 1, base, 0, 2) == 3);
    check(exCalls == beforeInvalid); // No side effects after any malformed input.
    setVpSet(0, 0, 1ull << 63, 0); // Explicitly empty banks outside the VP range are legal.
    check(invokeEx(true, true) == 0 && exTargets == 0);
    setVpSet(0, 0, uint64_t(-1), 1);
    check(invokeEx(false, false, 64) == 0 && exTargets == 1);
    setVpSet(0, 0, 1, 2);
    put(memory, 4096 - 32, 0); put(memory, 4096 - 24, 0);
    put(memory, 4096 - 16, 1); put(memory, 4096 - 8, 2);
    check(invokeEx(true, false, 1, base + 4096 - 32) == 0 && exTargets == 2);
    ArmHyperV flushing(true, memory, base, 2);
    auto flushCall = [&](bool extended, bool smccc, bool fast, uint64_t flags, uint64_t mask,
                         unsigned banks = 1, uint64_t address = base, uint64_t extra = 0,
                         uint64_t validBanks = 1, uint64_t format = 0) {
        std::fill(memory.begin(), memory.end(), 0);
        put(memory, 0, 0x12345000); put(memory, 8, flags);
        put(memory, 16, extended ? format : mask);
        put(memory, 24, validBanks); put(memory, 32, mask);
        auto control = (extended ? 0x13ull : 2ull) | (fast ? 0x10000 : 0) |
            (extended ? uint64_t(banks) << 17 : 0) | extra;
        r.fill(0x7777777777777777);
        r[0] = smccc ? 0x46000001 : control;
        if (smccc) r[1] = control;
        unsigned in = smccc ? 2 : 1;
        if (fast) for (unsigned i = 0; i < (extended ? 5u : 3u); ++i) r[in + i] = get(memory, i * 8);
        else { r[in] = address; r[in + 1] = uint64_t(-1); }
        auto before = r;
        check(flushing.call(smccc ? 0 : 1, r, 0));
        check(std::equal(r.begin() + 1, r.end(), before.begin() + 1));
        return r[0];
    };
    check(flushCall(false, false, true, 0, 1) == 6); // Identity before any service.
    r = {}; r[0] = one | 0x10051; r[1] = uint64_t(-1); r[2] = 0xfffffffe;
    r[3] = 0x90002; r[5] = 789;
    check(flushing.call(1, r, 0) && r[0] == one);
    check(flushCall(false, true, true, 1, uint64_t(-1)) == 2); // No native backend, no success.
    unsigned flushCalls = 0;
    uint64_t flushed = 0;
    flushing.setTranslationFlusher([&](uint64_t targets) { ++flushCalls; flushed = targets; return true; });
    for (bool extended : {false, true}) for (bool smccc : {false, true}) for (bool fast : {false, true}) {
        for (unsigned flags = 0; flags < 8; ++flags) {
            check(flushCall(extended, smccc, fast, flags, flags & 1 ? uint64_t(-1) : 2) == 0);
            check(flushed == (flags & 1 ? 3u : 2u)); // ALL_PROCESSORS must ignore even an invalid mask.
        }
        check(flushCall(extended, smccc, fast, 0, 0) == 0 && flushed == 0);
        auto before = flushCalls;
        check(flushCall(extended, smccc, fast, 8, 1) == 5);
        check(flushCall(extended, smccc, fast, 0, 4) == 14);
        for (auto bit : {1ull << 27, 1ull << 31, 1ull << 32, 1ull << 48})
            check(flushCall(extended, smccc, fast, 0, 1, 1, base, bit) == 3);
        check(flushCalls == before);
    }
    check(flushCall(true, true, true, 1, 1, 1, base, 0, 1ull << 63) == 0 && flushed == 3);
    check(flushCall(true, false, false, 0, 1, 1, base, 0, 1ull << 63) == 14);
    check(flushCall(true, false, true, 0, 0, 0, base, 0, 0, 1) == 0 && flushed == 3);
    check(flushCall(true, true, false, 1, 1, 1, base, 0, 1, 2) == 5);
    auto beforeBadFlush = flushCalls;
    for (auto address : {base - 8, base + 1, base + 4096 - 16, base + 8192})
        check(flushCall(false, false, false, 0, 1, 0, address) == 4);
    check(flushCall(true, true, true, 0, 1, 2) == 3); // Bank mask/length mismatch.
    check(flushCall(true, true, true, 0, 1, 13) == 3); // 136-byte input exceeds fast registers.
    check(flushCalls == beforeBadFlush);
    flushing.setTranslationFlusher([](uint64_t) { return false; });
    check(flushCall(false, true, true, 7, 1) == 0x71);
    check(flushing.report().find("\"tlbFlushRequests\":74") != std::string::npos);
    check(flushing.report().find("\"contextSyncRequests\":0") != std::string::npos);
    auto drainTlb = [&](bool smccc, bool fast, unsigned vp, uint64_t extra = 0) {
        r.fill(0x9999999999999999);
        auto control = uint64_t(0xd6) | (fast ? 0x10000 : 0) | extra;
        r[0] = smccc ? 0x46000001 : control;
        if (smccc) r[1] = control;
        unsigned in = smccc ? 2 : 1;
        r[in] = uint64_t(-1); r[in + 1] = uint64_t(-1); // Both GPAs are ignored.
        auto before = r;
        check(flushing.call(smccc ? 0 : 1, r, vp));
        check(std::equal(r.begin() + 1, r.end(), before.begin() + 1));
        return r[0];
    };
    check(drainTlb(false, false, 0) == 0x71); // Existing deliberately failing callback.
    flushing.setTranslationFlusher([&](uint64_t targets) { ++flushCalls; flushed = targets; return true; });
    for (bool smccc : {false, true}) for (bool fast : {false, true}) for (unsigned vp : {0, 1})
        check(drainTlb(smccc, fast, vp) == 0 && flushed == (1ull << vp));
    auto beforeDrainFailure = flushCalls;
    for (auto bit : {1ull << 17, 1ull << 27, 1ull << 31, 1ull << 32, 1ull << 48})
        check(drainTlb(true, false, 0, bit) == 3);
    check(drainTlb(false, false, 2) == 3 && flushCalls == beforeDrainFailure);
    check(flushing.report().find("\"tlbFlushRequests\":82") != std::string::npos);
    ArmHyperV earlyDrain(true, memory, base, 1);
    r = {}; r[0] = 0xd6;
    check(earlyDrain.call(1, r, 0) && r[0] == 6);
    ArmHyperV interrupts(true, memory, base, 2);
    auto sendIpi = [&](bool extended, bool smccc, bool fast, uint64_t vector, uint64_t targets,
                       unsigned banks = 1, uint64_t address = base, uint64_t extra = 0, uint64_t format = 0) {
        std::fill(memory.begin(), memory.end(), 0);
        put(memory, 0, vector); put(memory, 8, extended ? format : targets);
        put(memory, 16, format == 1 ? 0 : 1); put(memory, 24, targets);
        auto control = (extended ? 0x15ull : 0xbull) | (fast ? 0x10000 : 0) |
                       (extended ? uint64_t(banks) << 17 : 0) | extra;
        r.fill(0x5555555555555555); r[0] = smccc ? 0x46000001 : control;
        if (smccc) r[1] = control;
        unsigned in = smccc ? 2 : 1;
        if (fast) for (unsigned i = 0; i < (extended ? 4u : 2u); ++i) r[in + i] = get(memory, i * 8);
        else { r[in] = address; r[in + 1] = uint64_t(-1); }
        auto before = r;
        check(interrupts.call(smccc ? 0 : 1, r, 0));
        check(std::equal(r.begin() + 1, r.end(), before.begin() + 1));
        return r[0];
    };
    check(sendIpi(false, false, true, 0, 2) == 6);
    r = {}; r[0] = one | 0x10051; r[1] = uint64_t(-1); r[2] = 0xfffffffe;
    r[3] = 0x90002; r[5] = 456;
    check(interrupts.call(1, r, 0) && r[0] == one);
    check(sendIpi(false, true, true, 0, 2) == 2);
    unsigned sends = 0, sentVector = 99;
    uint64_t sentMask = 0;
    interrupts.setSoftwareInterruptSender([&](uint64_t mask, unsigned vector) {
        ++sends; sentMask = mask; sentVector = vector; return true;
    });
    for (bool extended : {false, true}) for (bool smccc : {false, true}) for (bool fast : {false, true}) {
        check(sendIpi(extended, smccc, fast, 0, 2) == 0 && sentMask == 2 && sentVector == 0);
        check(sendIpi(extended, smccc, fast, 15 | (0x10ull << 32), 1) == 0 && sentMask == 1 && sentVector == 15);
        check(sendIpi(extended, smccc, fast, 7, 0) == 0 && sentMask == 0);
        check(sendIpi(extended, smccc, fast, 15 | (0xfull << 32), 2) == 0 && sentMask == 2 && sentVector == 15);
        auto before = sends;
        for (auto vector : {uint64_t(16), uint64_t(255), 0x11ull << 32, 1ull << 40})
            check(sendIpi(extended, smccc, fast, vector, 1) == 5);
        check(sendIpi(extended, smccc, fast, 0, 4) == 14);
        check(sendIpi(extended, smccc, fast, 0, 1, 1, base, 1ull << 31) == 3);
        check(sends == before);
    }
    check(sendIpi(true, true, false, 0, 0, 0, base, 0, 1) == 0 && sentMask == 3);
    check(sendIpi(true, false, true, 0, 1, 2) == 3);
    check(sendIpi(true, true, true, 0, 1, 14) == 3);
    check(sendIpi(false, false, false, 0, 1, 0, base + 4096 - 8) == 4);
    interrupts.setSoftwareInterruptSender([](uint64_t, unsigned) { return false; });
    check(sendIpi(false, false, true, 0, 1) == 0x71);
    check(interrupts.report().find("\"ipiRequests\":33") != std::string::npos);
    check(interrupts.report().find("\"ipiTargetRequests\":[9,17]") != std::string::npos);
    r = {}; r[0] = one | 0x10050; r[1] = uint64_t(-1); r[2] = 0xfffffffe; r[3] = 0x201;
    check(interrupts.call(1, r, 0) && r[15] == 0xffffffff00000004ull); // Real backend enables IPI recommendation.
    // SynIC state, message ownership and event coalescing. Guest RAM is only
    // touched while all VPs are stopped, including the injection callback.
    std::vector<uint8_t> zeroBaseRam(4096, 0xaa);
    ArmSynic zeroBaseSynic(zeroBaseRam, 0, 1);
    check(zeroBaseSynic.write(0xa0012, 0, 1) == 0 && zeroBaseRam[0] == 0);
    std::vector<uint8_t> synicRam(5 * 4096, 0xaa);
    ArmSynic synic(synicRam, base, 2);
    uint64_t value = 99;
    bool newly = true;
    for (unsigned vp = 0; vp < 2; ++vp) {
        check(synic.read(0xa0010, vp, value) == 0 && value == 0);
        check(synic.read(0xa0011, vp, value) == 0 && value == 1);
        for (unsigned sint = 0; sint < 16; ++sint)
            check(synic.read(0xa0000 + sint, vp, value) == 0 && value == 0x10000);
    }
    check(synic.signalEvent(1, 2, 0, newly) == 0x18 && !newly);
    check(synic.write(0xa0011, 0, 1) == 5); // Read-only version.
    check(synic.write(0xa0015, 0, 1) == 5); // No fake response-buffer support.
    check(synic.write(0xa0010, 0, 2) == 5);
    check(synic.write(0xa0012, 1, base + 1) == 0);
    check(synic.write(0xa0013, 1, base + 4096 + 1) == 0);
    check(std::all_of(synicRam.begin(), synicRam.begin() + 8192, [](auto b) { return b == 0; }));
    check(synic.write(0xa0010, 1, 1) == 0);
    for (uint64_t bad : {uint64_t(15), uint64_t(32), 16ull | (1ull << 17), 16ull | (1ull << 20)})
        check(synic.write(0xa0002, 1, bad) == 5);
    check(synic.write(0xa0002, 1, 16) == 0);
    check(synic.signalEvent(1, 2, 0, newly) == 0x71 && !newly && synicRam[512] == 0);
    unsigned notifications = 0;
    synic.setInterruptSender([&](uint64_t targets, unsigned vector) {
        check(targets == 2 && vector == 16);
        check(synicRam[512] != 0 || synicRam[4096 + 512] != 0);
        ++notifications; return true;
    });
    check(synic.signalEvent(1, 2, 0, newly) == 0 && newly);
    check(synic.signalEvent(1, 2, 0, newly) == 0 && !newly && notifications == 1);
    check(synic.signalEvent(1, 2, 2047, newly) == 0 && newly && synicRam[767] == 128);
    synicRam[512] &= ~1; // Guest clears a flag after servicing it.
    check(synic.signalEvent(1, 2, 0, newly) == 0 && newly && notifications == 3);
    check(synic.write(0xa0012, 1, base + 1) == 0 && synicRam[512] == 1);
    auto before = synicRam;
    for (auto bad : {base - 4096 + 1, base + 5 * 4096 + 1, base + 3})
        check(synic.write(0xa0012, 1, bad) == 5 && synicRam == before);
    check(synic.signalEvent(2, 2, 0, newly) == 14 && !newly);
    check(synic.signalEvent(1, 16, 0, newly) == 5);
    check(synic.signalEvent(1, 2, 2048, newly) == 5);
    check(synic.write(0xa0002, 1, 16 | 0x10000) == 0);
    check(synic.signalEvent(1, 2, 1, newly) == 0x18 && synicRam[512] == 1);
    check(synic.write(0xa0002, 1, 16 | 0x50000) == 0); // Polling overrides mask.
    check(synic.signalEvent(1, 2, 1, newly) == 0 && newly && notifications == 3);
    check(synic.write(0xa0002, 1, 16) == 0);
    const std::array<uint8_t, 3> payload{11, 22, 33};
    check(synic.postMessage(1, 2, 0x80000010, 0x12345678, payload) == 0);
    const size_t slot = 4096 + 512;
    check(get(synicRam, slot) == 0x0000000380000010ull);
    check(get(synicRam, slot + 8) == 0x12345678 && synicRam[slot + 16] == 11);
    check(notifications == 4);
    for (unsigned i = 0; i < 16; ++i)
        check(synic.postMessage(1, 2, 1 + i, i, payload) == 0);
    check(synicRam[slot + 5] == 1 && notifications == 4);
    check(synic.postMessage(1, 2, 99, 99, payload) == 0x13);
    check(synic.write(0xa0014, 1, 0) == 0 && notifications == 4); // Occupied slot retained.
    for (unsigned i = 0; i < 16; ++i) {
        put(synicRam, slot, 0, 4); // Guest releases slot before EOM.
        check(synic.write(0xa0014, 1, 0) == 0);
        check(synicRam[slot] == i + 1 && get(synicRam, slot + 8) == i);
        check(synicRam[slot + 5] == (i != 15));
    }
    check(notifications == 20);
    check(synic.read(0xa0014, 1, value) == 0 && value == 0);
    check(synic.write(0xa0014, 1, 1) == 5);
    put(synicRam, slot, 0, 4);
    synic.setInterruptSender([](uint64_t, unsigned) { return false; });
    auto empty = synicRam;
    check(synic.postMessage(1, 2, 77, 0, payload) == 0x71 && synicRam == empty);
    check(synic.write(0xa0002, 1, 16 | 0x10000) == 0);
    check(synic.postMessage(1, 2, 78, 0, payload) == 0 && synicRam[slot] == 78);
    check(synic.postMessage(1, 2, 79, 0, payload) == 0);
    check(synic.write(0xa0013, 1, base + 4096) == 0);
    put(synicRam, slot, 0, 4);
    check(synic.write(0xa0014, 1, 0) == 0 && synicRam[slot] == 0); // Disabled page defers queue.
    check(synic.postMessage(1, 2, 80, 0, payload) == 0x18);
    check(synic.write(0xa0013, 1, base + 4096 + 1) == 0);
    check(synic.write(0xa0014, 1, 0) == 0 && synicRam[slot] == 79);
    check(synic.postMessage(1, 2, 0, 0, payload) == 5);
    check(synic.postMessage(1, 2, 1, 0, std::span<const uint8_t>(synicRam).first(241)) == 5);
    fastWrite(0x1000a000066bbull); check(r[0] == one);
    // ARM Get/SetVpRegisters delegates to this controller without advertising
    // the incomplete feature. Both HVC register layouts remain valid.
    for (bool smccc : {false, true}) {
        r = {}; r[0] = smccc ? 0x46000001 : one | 0x10051;
        unsigned start = smccc ? 2 : 1;
        if (smccc) r[1] = one | 0x10051;
        r[start] = uint64_t(-1); r[start + 1] = 0xfffffffe;
        r[start + 2] = 0xa0010; r[start + 4] = 1;
        check(hv.call(smccc ? 0 : 1, r, 1) && r[0] == one);
        fastRead(0xa0010, smccc, 1);
        check(r[0] == one && r[smccc ? 6 : 15] == 1);
    }
    ArmHyperVTimers timerModel(2);
    check(!timerModel.nextDeadline());
    for (unsigned vp = 0; vp < 2; ++vp) for (uint32_t reg = 0xb0000; reg <= 0xb0007; ++reg)
        check(timerModel.read(reg, vp, value) == 0 && value == 0);
    unsigned timerSignals = 0;
    uint64_t expectedExpiration = 100, expectedDelivery = 100;
    timerModel.setDelivery([&](unsigned vp, unsigned timer, uint64_t config, uint64_t expiration, uint64_t now) {
        check(vp == 1 && timer == 3 && (config & 0x1000));
        check(expiration == expectedExpiration && now == expectedDelivery);
        ++timerSignals; return true;
    });
    check(timerModel.write(0xb0006, 1, 0x1108, 10) == 0); // Direct PPI 16, AutoEnable.
    check(timerModel.write(0xb0007, 1, 100, 10) == 0);
    timerModel.service(99); check(timerSignals == 0);
    check(timerModel.nextDeadline() == 100);
    timerModel.service(100); check(timerSignals == 1 && !timerModel.nextDeadline());
    check(timerModel.read(0xb0006, 1, value) == 0 && !(value & 1));
    timerModel.service(200); check(timerSignals == 1);
    check(timerModel.write(0xb0006, 1, 0x110a, 200) == 0); // Periodic + AutoEnable.
    check(timerModel.write(0xb0007, 1, 50, 200) == 0);
    expectedExpiration = 250; expectedDelivery = 399;
    timerModel.service(399); check(timerSignals == 2 && timerModel.nextDeadline() == 400);
    expectedExpiration = expectedDelivery = 400;
    timerModel.service(400); check(timerSignals == 3 && timerModel.nextDeadline() == 450);
    check(timerModel.write(0xb0007, 1, 0, 401) == 0 && !timerModel.nextDeadline());
    check(timerModel.read(0xb0006, 1, value) == 0 && !(value & 1));
    for (uint64_t invalid : {1ull << 9, 1ull << 13, 1ull << 20, 1ull << 63})
        check(timerModel.write(0xb0006, 1, invalid, 0) == 5);
    check(timerModel.write(0xb0006, 2, 0, 0) == 14);
    check(timerModel.write(0xb0008, 1, 0, 0) == 5);
    check(timerModel.write(0xb0006, 1, 1, 0) == 0); // SINT 0 disables message timers.
    check(timerModel.read(0xb0006, 1, value) == 0 && !(value & 1));
    check(timerModel.write(0xb0006, 1, 0x1108, 0) == 0);
    check(timerModel.write(0xb0007, 1, 50, 0) == 0);
    timerModel.setDelivery([](unsigned, unsigned, uint64_t, uint64_t, uint64_t) { return false; });
    timerModel.service(60);
    check(timerModel.delivered() == 3 && timerModel.nextDeadline() == 50);
    check(timerModel.read(0xb0006, 1, value) == 0 && !(value & 1)); // Expired even if busy.
    expectedExpiration = 50; expectedDelivery = 70;
    timerModel.setDelivery([&](unsigned, unsigned, uint64_t, uint64_t expiration, uint64_t now) {
        check(expiration == expectedExpiration && now == expectedDelivery); return true;
    });
    timerModel.service(70); check(timerModel.delivered() == 4 && !timerModel.nextDeadline());
    // Integrated timer registers share the partition clock and SynIC pages.
    uint64_t timerClock = 1000;
    std::vector<uint8_t> timerRam(8192);
    ArmHyperV timerHv(true, timerRam, base, 2, [&] { return timerClock; });
    auto timerWrite = [&](uint32_t name, uint64_t low, uint64_t high = 0) {
        r = {}; r[0] = 0x46000001; r[1] = one | 0x10051;
        r[2] = uint64_t(-1); r[3] = 0xfffffffe; r[4] = name; r[6] = low; r[7] = high;
        check(timerHv.call(0, r, 0)); return r[0];
    };
    check(timerWrite(0xb0000, 0) == 6);
    check(timerWrite(0x90002, 0x1000a000066bbull) == one);
    check(timerWrite(0xb0000, 0, 1) == 5);
    check(timerWrite(0xa0010, 1) == one);
    check(timerWrite(0xa0013, base + 1) == one);
    check(timerWrite(0xa0002, 31) == one);
    unsigned timerIrqs = 0;
    timerHv.setPrivateInterruptSender([&](uint64_t mask, unsigned vector) {
        check(mask == 1 && vector == 31); ++timerIrqs; return true;
    });
    check(timerWrite(0xb0000, 0x20008) == one);
    check(timerWrite(0xb0001, 100) == one);
    check(timerHv.serviceDelay100ns() == 100);
    timerClock = 1099; timerHv.service(); check(timerIrqs == 0);
    timerClock = 1100; timerHv.service(); check(timerIrqs == 1 && !timerHv.timersActive());
    check(get(timerRam, 512) == 0x0000001880000010ull);
    check(get(timerRam, 512 + 16) == 0 && get(timerRam, 512 + 24) == 100 && get(timerRam, 512 + 32) == 100);
    check(timerWrite(0xb0003, 120) == one);
    check(timerWrite(0xb0002, 0x20001) == one);
    timerClock = 1120; timerHv.service();
    check(timerIrqs == 1 && timerHv.timersActive() && timerRam[517] == 1);
    put(timerRam, 512, 0, 4); // Free slot for timer 1's dedicated pending message.
    timerClock = 1125; timerHv.service();
    check(timerIrqs == 2 && !timerHv.timersActive());
    check(get(timerRam, 512 + 16) == 1 && get(timerRam, 512 + 24) == 120 && get(timerRam, 512 + 32) == 125);
    check(timerHv.report().find("\"syntheticTimerDeliveries\":2") != std::string::npos);
    ArmHyperVTimers allTimers(2);
    unsigned firedMask = 0;
    allTimers.setDelivery([&](unsigned vp, unsigned timer, uint64_t, uint64_t expiration, uint64_t now) {
        check(expiration == 500 && now == 500);
        firedMask |= 1u << (vp * 4 + timer); return true;
    });
    for (unsigned vp = 0; vp < 2; ++vp) for (unsigned timer = 0; timer < 4; ++timer) {
        check(allTimers.write(0xb0001 + timer * 2, vp, 500, 0) == 0);
        check(allTimers.write(0xb0000 + timer * 2, vp, 0x1101, 0) == 0);
    }
    allTimers.service(499); check(firedMask == 0);
    allTimers.service(500); check(firedMask == 255 && allTimers.delivered() == 8 && !allTimers.nextDeadline());
    allTimers.setDelivery([](unsigned, unsigned, uint64_t, uint64_t expiration, uint64_t now) {
        check(expiration == 1100 && now == 1250); return true;
    });
    check(allTimers.write(0xb0000, 0, 0x1106, 1000) == 0);
    check(allTimers.write(0xb0001, 0, 100, 1000) == 0);
    check(allTimers.write(0xb0000, 0, 0x1107, 1000) == 0);
    check(allTimers.nextDeadline() == 1100 && !allTimers.nextDeadline(false));
    allTimers.service(1250); check(allTimers.nextDeadline() == 1300);
    check(allTimers.write(0xb0000, 0, 0, 1250) == 0 && !allTimers.nextDeadline());
    check(allTimers.write(0xb0001, 0, UINT64_MAX, 10) == 0);
    check(allTimers.write(0xb0000, 0, 0x1103, 10) == 0);
    auto beforeOverflow = allTimers.delivered();
    allTimers.service(UINT64_MAX);
    check(allTimers.delivered() == beforeOverflow && allTimers.nextDeadline() == 9);
    allTimers.setDelivery([](unsigned, unsigned, uint64_t, uint64_t expiration, uint64_t now) {
        check(expiration == 9 && now == 9); return true;
    });
    allTimers.service(8); check(allTimers.delivered() == beforeOverflow);
    allTimers.service(9); check(allTimers.delivered() == beforeOverflow + 1);
    ArmHyperVTimers wrapped(1);
    unsigned wrappedSignals = 0;
    bool acceptWrapped = false;
    wrapped.setDelivery([&](unsigned, unsigned, uint64_t, uint64_t expiration, uint64_t now) {
        check(expiration == 10 && (now == 10 || now == 59));
        if (acceptWrapped) ++wrappedSignals;
        return acceptWrapped;
    });
    check(wrapped.write(0xb0001, 0, 20, UINT64_MAX - 9) == 0);
    check(wrapped.write(0xb0000, 0, 0x1103, UINT64_MAX - 9) == 0);
    check(wrapped.nextDeadline() == 10 && wrapped.nextDelay(UINT64_MAX - 9) == 20);
    wrapped.service(UINT64_MAX); check(wrappedSignals == 0 && wrapped.nextDelay(UINT64_MAX) == 11);
    wrapped.service(5); check(wrappedSignals == 0 && wrapped.nextDelay(5) == 5);
    wrapped.service(10); check(wrappedSignals == 0 && wrapped.nextDelay(10) == 0);
    acceptWrapped = true;
    wrapped.service(59); check(wrappedSignals == 1 && wrapped.nextDeadline() == 70 && wrapped.nextDelay(59) == 11);
    // Draining after a guest EOI/slot release must not require a new post or
    // an EOM hypercall. Disabled mappings stop forcing host polling.
    check(synic.postMessage(1, 2, 80, 0, payload) == 0 && synic.needsService());
    check(synic.service() == 0 && synicRam[slot] == 79); // Still occupied.
    put(synicRam, slot, 0, 4);
    check(synic.service() == 0 && synicRam[slot] == 80 && !synic.needsService());
    check(synic.postMessage(1, 2, 81, 0, payload) == 0);
    check(synic.write(0xa0013, 1, base + 4096) == 0 && !synic.needsService());
    check(synic.write(0xa0013, 1, base + 4096 + 1) == 0 && synic.needsService());
    put(synicRam, slot, 0, 4);
    check(synic.service() == 0 && synicRam[slot] == 81 && !synic.needsService());
    check(synic.postMessage(1, 2, 82, 0, payload) == 0);
    check(synic.write(0xa0002, 1, 16) == 0);
    put(synicRam, slot, 0, 4);
    auto beforeRescanFailure = synicRam;
    check(synic.service() == 0x71 && synic.needsService() && synicRam == beforeRescanFailure);
    synic.setInterruptSender([](uint64_t, unsigned) { return true; });
    check(synic.service() == 0 && synicRam[slot] == 82 && !synic.needsService());
    ArmHyperVTimers crossing(1);
    bool acceptCrossing = false;
    crossing.setDelivery([&](unsigned, unsigned, uint64_t, uint64_t expiration, uint64_t now) {
        check(expiration == UINT64_MAX - 10 && (now == UINT64_MAX - 10 || now == 5));
        return acceptCrossing;
    });
    check(crossing.write(0xb0001, 0, 10, UINT64_MAX - 20) == 0);
    check(crossing.write(0xb0000, 0, 0x1103, UINT64_MAX - 20) == 0);
    crossing.service(UINT64_MAX - 10);
    check(crossing.delivered() == 0);
    acceptCrossing = true;
    crossing.service(5);
    check(crossing.delivered() == 1 && crossing.nextDeadline() == 9 && crossing.nextDelay(5) == 4);
    ArmHyperVTimers orderedWrap(2);
    unsigned orderedMask = 0;
    orderedWrap.setDelivery([&](unsigned vp, unsigned, uint64_t, uint64_t expiration, uint64_t) {
        check(expiration == (vp ? UINT64_MAX - 4 : 10)); orderedMask |= 1u << vp; return true;
    });
    check(orderedWrap.write(0xb0001, 0, 20, UINT64_MAX - 9) == 0);
    check(orderedWrap.write(0xb0000, 0, 0x1103, UINT64_MAX - 9) == 0);
    check(orderedWrap.write(0xb0001, 1, UINT64_MAX - 4, UINT64_MAX - 9) == 0);
    check(orderedWrap.write(0xb0000, 1, 0x1101, UINT64_MAX - 9) == 0);
    check(orderedWrap.nextDeadline() == UINT64_MAX - 4 && orderedWrap.nextDelay(UINT64_MAX - 9) == 5);
    orderedWrap.service(UINT64_MAX - 4); check(orderedMask == 2 && orderedWrap.nextDeadline() == 10);
    orderedWrap.service(10); check(orderedMask == 3 && orderedWrap.nextDeadline() == 30);
    std::vector<uint8_t> connectionRam(8192);
    ArmHyperV connected(true, connectionRam, base, 2);
    auto connectedWrite = [&](uint32_t reg, uint64_t value) {
        r = {}; r[0] = 0x46000001; r[1] = one | 0x10051;
        r[2] = uint64_t(-1); r[3] = 0xfffffffe; r[4] = reg; r[6] = value;
        check(connected.call(0, r, 0) && r[0] == one);
    };
    auto connectionCall = [&](bool message, bool smccc, bool fast, uint64_t controlExtra = 0, uint64_t input = base) {
        r.fill(0xabcdef);
        r[0] = smccc ? 0x46000001 : ((message ? 0x5c : 0x5d) | (fast ? 0x10000 : 0) | controlExtra);
        if (smccc) r[1] = (message ? 0x5c : 0x5d) | (fast ? 0x10000 : 0) | controlExtra;
        unsigned start = smccc ? 2 : 1;
        r[start] = fast ? get(connectionRam, 0) : input;
        r[start + 1] = UINT64_MAX; // No output buffer; fast pair padding ignored.
        check(connected.call(smccc ? 0 : 1, r, 1)); return r[0];
    };
    check(connectionCall(false, true, true) == 6);
    connectedWrite(0x90002, 0x1000a000066bbull);
    check(connectionCall(true, false, false) == 6); // No host endpoints or privilege yet.
    auto &receiver = connected.synicController();
    check(receiver.write(0xa0010, 0, 1) == 0 && receiver.write(0xa0012, 0, base + 4096 + 1) == 0);
    check(receiver.write(0xa0002, 0, 16) == 0);
    unsigned connectionIrqs = 0;
    connected.setPrivateInterruptSender([&](uint64_t mask, unsigned vector) {
        check(mask == 1 && vector == 16); ++connectionIrqs; return true;
    });
    check(connected.bindEventConnection(42, 10, [&](unsigned vp, uint16_t relative) {
        check(vp == 1); bool newly = false;
        return receiver.signalEvent(0, 2, 100 + relative, newly);
    }));
    check(!connected.bindEventConnection(42, 10, [](unsigned, uint16_t) { return 0; }));
    check(!connected.bindEventConnection(43, 0, [](unsigned, uint16_t) { return 0; }));
    check(!connected.bindMessageConnection(0x1000000, [](unsigned, uint32_t, std::span<const uint8_t>) { return 0; }));
    for (bool smccc : {false, true}) for (bool fast : {false, true}) {
        put(connectionRam, 0, 42 | (3ull << 32));
        check(connectionCall(false, smccc, fast) == 0);
        check(connectionRam[4096 + 512 + 103 / 8] & (1 << (103 % 8)));
    }
    check(connectionIrqs == 1); // Duplicate accepted event requests coalesce.
    put(connectionRam, 0, 42 | (10ull << 32)); check(connectionCall(false, true, true) == 5);
    put(connectionRam, 0, 42 | (1ull << 48)); check(connectionCall(false, true, true) == 5);
    put(connectionRam, 0, 99); check(connectionCall(false, true, true) == 0x12);
    put(connectionRam, 0, 0x8000002a); check(connectionCall(false, true, true) == 0x12);
    check(connected.bindMessageConnection(7, [&](unsigned vp, uint32_t type, std::span<const uint8_t> payload) {
        check(vp == 1 && type == 1 && payload.size() == 240 && payload[0] == 11 && payload[239] == 22);
        return uint16_t(0);
    }));
    for (bool smccc : {false, true}) {
        std::fill(connectionRam.begin(), connectionRam.begin() + 256, 0);
        put(connectionRam, 0, 7, 4); put(connectionRam, 8, 1, 4); put(connectionRam, 12, 240, 4);
        connectionRam[16] = 11; connectionRam[255] = 22;
        check(connectionCall(true, smccc, false) == 0);
        check(connectionCall(true, smccc, true) == 3);
        for (auto extra : {one, 1ull << 17, 1ull << 31, 1ull << 48})
            check(connectionCall(true, smccc, false, extra) == 3);
        check(connectionCall(true, smccc, false, 0, base + 1) == 4);
        check(connectionCall(true, smccc, false, 0, base + 4096 - 8) == 3);
        check(connectionCall(true, smccc, false, 0, base + 8192) == 3);
    }
    put(connectionRam, 8, 0, 4); check(connectionCall(true, true, false) == 5);
    put(connectionRam, 8, 0x80000001, 4); check(connectionCall(true, true, false) == 5);
    put(connectionRam, 8, 1, 4); put(connectionRam, 12, 241, 4); check(connectionCall(true, true, false) == 5);
    put(connectionRam, 12, 240, 4); put(connectionRam, 4, 1, 4); check(connectionCall(true, true, false) == 5);
    put(connectionRam, 4, 0, 4); put(connectionRam, 0, 42, 4); check(connectionCall(true, true, false) == 0x11);
    put(connectionRam, 0, 7); check(connectionCall(false, true, true) == 0x11);
    check(connected.removeConnection(7) && !connected.removeConnection(7));
    check(connected.bindMessageConnection(7, [&](unsigned, uint32_t, std::span<const uint8_t>) {
        check(connected.removeConnection(7)); return uint16_t(0x13);
    }));
    put(connectionRam, 8, 1, 4); put(connectionRam, 12, 0, 4);
    check(connectionCall(true, true, false) == 0x13); // Preserve endpoint backpressure.
    check(connectionCall(true, true, false) == 0x12); // Callback removed itself safely.
    check(connected.report().find("\"postedMessages\":2") != std::string::npos);
    check(connected.report().find("\"signaledEvents\":4") != std::string::npos);
    for (uint32_t id = 100; id < 163; ++id)
        check(connected.bindMessageConnection(id, [](unsigned, uint32_t, std::span<const uint8_t>) { return uint16_t(0); }));
    check(!connected.bindMessageConnection(163, [](unsigned, uint32_t, std::span<const uint8_t>) { return uint16_t(0); }));
    r = {}; r[0] = 0x46000001; r[1] = one | 0x10050;
    r[2] = UINT64_MAX; r[3] = 0xfffffffe; r[4] = 0x200;
    check(connected.call(0, r, 0) && r[0] == one && r[6] == (features | (1ull << 36) | (1ull << 37)));
    std::cout << checks << " ARM Hyper-V checks passed\n";
}
