#include "arm_hyperv.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace pc;
static unsigned checks = 0;
static void check(bool value) {
    if (!value) throw std::runtime_error("Hyper-V check " + std::to_string(checks + 1));
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
    std::cout << checks << " ARM Hyper-V checks passed\n";
}
