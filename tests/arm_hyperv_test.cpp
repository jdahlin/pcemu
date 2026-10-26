#include "arm_hyperv.hpp"
#include "arm_vmbus.hpp"
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
static uint64_t get(const std::vector<uint8_t> &b, size_t at, size_t size = 8) {
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i) value |= uint64_t(b.at(at + i)) << (8 * i);
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
    // Slow SetVpRegisters, read-only protection and ignored association padding.
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
    check(hv.call(1, r, 0) && r[0] == one);
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
    check(zeroBaseSynic.write_vp_register(0xa0012, 0, 1) == 0 && zeroBaseRam[0] == 0);
    {
        std::vector<uint8_t> memory(8192);
        ArmSynic diagnostic(memory, base, 2);
        check(diagnostic.report().find("\"slotType\"") == std::string::npos);
        check(diagnostic.write_vp_register(0xa0010, 0, 1) == 0);
        check(diagnostic.write_vp_register(0xa0013, 0, base + 1) == 0);
        check(diagnostic.write_vp_register(0xa0002, 0, 0x10008) == 0);
        std::array<uint8_t, 20> versionReply{};
        versionReply[0] = 15; versionReply[8] = 1; versionReply[12] = 7;
        const auto before = memory;
        check(diagnostic.post_message(0, 2, 1, 0, versionReply) == 0);
        const auto posted = memory;
        const auto snapshot = diagnostic.report();
        check(snapshot.find("\"config\":65544,\"queued\":0,\"slotType\":1,\"payloadBytes\":20") != std::string::npos);
        check(snapshot.find("\"vmbusVersionReply\":{\"supported\":1,\"connectionState\":0,\"connectionId\":7,\"features\":0}") != std::string::npos);
        check(memory == posted && memory != before); // Reporting does not acknowledge a slot.
        check(diagnostic.post_message(0, 2, 1, 0, versionReply) == 0);
        check(diagnostic.report().find("\"config\":65544,\"queued\":1") != std::string::npos);
        memory[512] = 0; // Guest consumes the visible message; stale payload is private.
        check(diagnostic.report().find("\"vmbusVersionReply\"") == std::string::npos);
        check(diagnostic.service_message_queues() == 0);
        memory[516] = 15; // Truncated payload cannot expose response fields.
        check(diagnostic.report().find("\"vmbusVersionReply\"") == std::string::npos);
        memory[516] = 241;
        check(diagnostic.report().find("\"vmbusVersionReply\"") == std::string::npos);
        memory[516] = 20; memory[512] = 2; // Arbitrary payloads are never decoded.
        check(diagnostic.report().find("\"vmbusVersionReply\"") == std::string::npos);
        check(diagnostic.write_vp_register(0xa0013, 0, 0) == 0);
        check(diagnostic.report().find("\"slotType\"") == std::string::npos);
    }
    std::vector<uint8_t> synicRam(5 * 4096, 0xaa);
    ArmSynic synic(synicRam, base, 2);
    uint64_t value = 99;
    bool newly = true;
    for (unsigned vp = 0; vp < 2; ++vp) {
        check(synic.read_vp_register(0xa0010, vp, value) == 0 && value == 0);
        check(synic.read_vp_register(0xa0011, vp, value) == 0 && value == 1);
        for (unsigned sint = 0; sint < 16; ++sint)
            check(synic.read_vp_register(0xa0000 + sint, vp, value) == 0 && value == 0x10000);
    }
    check(synic.signal_event(1, 2, 0, newly) == 0x18 && !newly);
    check(synic.write_vp_register(0xa0011, 0, 1) == 5); // Read-only version.
    check(synic.write_vp_register(0xa0015, 0, 1) == 5); // No fake response-buffer support.
    check(synic.write_vp_register(0xa0010, 0, 2) == 5);
    check(synic.write_vp_register(0xa0012, 1, base + 1) == 0);
    check(synic.write_vp_register(0xa0013, 1, base + 4096 + 1) == 0);
    check(std::all_of(synicRam.begin(), synicRam.begin() + 8192, [](auto b) { return b == 0; }));
    check(synic.write_vp_register(0xa0010, 1, 1) == 0);
    for (uint64_t bad : {uint64_t(32), uint64_t(255), 16ull | (1ull << 17), 16ull | (1ull << 20)})
        check(synic.write_vp_register(0xa0002, 1, bad) == 5);
    check(synic.write_vp_register(0xa0002, 1, 16) == 0);
    check(synic.signal_event(1, 2, 0, newly) == 0x71 && !newly && synicRam[512] == 0);
    unsigned notifications = 0;
    synic.set_interrupt_sender([&](uint64_t targets, unsigned vector) {
        check(targets == 2 && vector == 16);
        check(synicRam[512] != 0 || synicRam[4096 + 512] != 0);
        ++notifications; return true;
    });
    check(synic.signal_event(1, 2, 0, newly) == 0 && newly);
    check(synic.signal_event(1, 2, 0, newly) == 0 && !newly && notifications == 1);
    check(synic.signal_event(1, 2, 2047, newly) == 0 && newly && synicRam[767] == 128);
    synicRam[512] &= ~1; // Guest clears a flag after servicing it.
    check(synic.signal_event(1, 2, 0, newly) == 0 && newly && notifications == 3);
    check(synic.write_vp_register(0xa0012, 1, base + 1) == 0 && synicRam[512] == 1);
    auto before = synicRam;
    for (auto bad : {base - 4096 + 1, base + 5 * 4096 + 1, base + 3})
        check(synic.write_vp_register(0xa0012, 1, bad) == 5 && synicRam == before);
    check(synic.signal_event(2, 2, 0, newly) == 14 && !newly);
    check(synic.signal_event(1, 16, 0, newly) == 5);
    check(synic.signal_event(1, 2, 2048, newly) == 5);
    check(synic.write_vp_register(0xa0002, 1, 16 | 0x10000) == 0);
    check(synic.signal_event(1, 2, 1, newly) == 0x18 && synicRam[512] == 1);
    check(synic.write_vp_register(0xa0002, 1, 16 | 0x50000) == 0); // Polling overrides mask.
    check(synic.signal_event(1, 2, 1, newly) == 0 && newly && notifications == 3);
    check(synic.write_vp_register(0xa0002, 1, 16) == 0);
    const std::array<uint8_t, 3> payload{11, 22, 33};
    check(synic.post_message(1, 2, 0x80000010, 0x12345678, payload) == 0);
    const size_t slot = 4096 + 512;
    check(get(synicRam, slot) == 0x0000000380000010ull);
    check(get(synicRam, slot + 8) == 0x12345678 && synicRam[slot + 16] == 11);
    check(notifications == 4);
    for (unsigned i = 0; i < 16; ++i)
        check(synic.post_message(1, 2, 1 + i, i, payload) == 0);
    check(synicRam[slot + 5] == 1 && notifications == 4);
    check(synic.post_message(1, 2, 99, 99, payload) == 0x13);
    check(synic.write_vp_register(0xa0014, 1, 0) == 0 && notifications == 4); // Occupied slot retained.
    for (unsigned i = 0; i < 16; ++i) {
        put(synicRam, slot, 0, 4); // Guest releases slot before EOM.
        check(synic.write_vp_register(0xa0014, 1, 0) == 0);
        check(synicRam[slot] == i + 1 && get(synicRam, slot + 8) == i);
        check(synicRam[slot + 5] == (i != 15));
    }
    check(notifications == 20);
    check(synic.read_vp_register(0xa0014, 1, value) == 0 && value == 0);
    check(synic.write_vp_register(0xa0014, 1, 1) == 5);
    put(synicRam, slot, 0, 4);
    synic.set_interrupt_sender([](uint64_t, unsigned) { return false; });
    auto empty = synicRam;
    check(synic.post_message(1, 2, 77, 0, payload) == 0x71 && synicRam == empty);
    check(synic.write_vp_register(0xa0002, 1, 16 | 0x10000) == 0);
    check(synic.post_message(1, 2, 78, 0, payload) == 0 && synicRam[slot] == 78);
    check(synic.post_message(1, 2, 79, 0, payload) == 0);
    check(synic.write_vp_register(0xa0013, 1, base + 4096) == 0);
    put(synicRam, slot, 0, 4);
    check(synic.write_vp_register(0xa0014, 1, 0) == 0 && synicRam[slot] == 0); // Disabled page defers queue.
    check(synic.post_message(1, 2, 80, 0, payload) == 0x18);
    check(synic.write_vp_register(0xa0013, 1, base + 4096 + 1) == 0);
    check(synic.write_vp_register(0xa0014, 1, 0) == 0 && synicRam[slot] == 79);
    check(synic.post_message(1, 2, 0, 0, payload) == 5);
    check(synic.post_message(1, 2, 1, 0, std::span<const uint8_t>(synicRam).first(241)) == 5);
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
    // Classify repeated guest writes without dropping their rearm semantics.
    ArmHyperVTimers observed(2);
    check(observed.write(0xb0000, 0, 0x1108, 0) == 0);
    check(observed.write(0xb0001, 0, 100, 0) == 0);
    observed.service(1);
    check(observed.nextDelay(1) == 99);
    check(observed.write(0xb0001, 0, 100, 1) == 0);
    check(observed.write(0xb0000, 0, 0x1109, 2) == 0);
    check(observed.write(0xb0001, 0, 0, 3) == 0);
    check(observed.write(0xb0000, 0, 0x110b, 4) == 0);
    check(observed.write(0xb0001, 0, 5, 4) == 0);
    check(observed.write(0xb0001, 0, 5, 6) == 0);
    observed.service(10);
    check(observed.nextDelay(10) == 1); // Same periodic count still restarts the period.
    observed.service(11); // No delivery backend: retry the same expiration.
    const auto &activity = observed.activity();
    check(activity.configWrites == 3 && activity.countWrites == 5);
    check(activity.repeatedConfigWrites == 1 && activity.repeatedCountWrites == 2);
    check(activity.zeroCountWrites == 1 && activity.autoEnableWrites == 1);
    check(activity.periodicRearms == 2 && activity.unchangedArmedDeadline == 2);
    check(activity.configFlags[8] == 1 && activity.configFlags[9] == 1 && activity.configFlags[11] == 1);
    check(observed.serviceCalls() == 3 && observed.serviceTimerVisits() == 8 && observed.serviceFastReturns() == 2);
    check(observed.deadlineQueries() == 2 && observed.deadlineTimerVisits() == 16);
    check(observed.deliveryRetries() == 1);
    // The derived minima must follow lazy changes, cancellation and earlier
    // deadlines even when a previously cached minimum belonged to another VP.
    ArmHyperVTimers minima(2);
    check(minima.write(0xb0001, 0, 1000, 0) == 0);
    check(minima.write(0xb0000, 0, 0x1105, 0) == 0); // Lazy.
    check(minima.write(0xb0003, 1, 2000, 0) == 0);
    check(minima.write(0xb0002, 1, 0x1101, 0) == 0);
    check(minima.nextDeadline() == 1000 && minima.nextDeadline(false) == 2000);
    check(minima.write(0xb0000, 0, 0x1101, 0) == 0);
    check(minima.nextDeadline(false) == 1000);
    check(minima.write(0xb0001, 0, 0, 0) == 0);
    check(minima.nextDeadline() == 2000);
    check(minima.write(0xb0003, 1, 500, 0) == 0);
    check(minima.nextDeadline(false) == 500);
    const auto visitsBeforeIdle = minima.deadlineTimerVisits();
    for (unsigned now = 1; now < 500; ++now) {
        minima.service(now);
        check(minima.nextDelay(now, false) == 500 - now);
    }
    check(minima.deadlineTimerVisits() == visitsBeforeIdle && minima.serviceTimerVisits() == 0);
    minima.service(500); // Busy/missing backend keeps the due timer for retry.
    check(minima.nextDelay(501) == 0 && minima.deliveryRetries() == 1);
    minima.setDelivery([](unsigned, unsigned, uint64_t, uint64_t expiration, uint64_t now) {
        check(expiration == 500 && now == 501); return true;
    });
    minima.service(501);
    check(minima.delivered() == 1 && !minima.nextDeadline());
    // One consistent clock sample serves all deadline sources in a query.
    std::vector<uint8_t> snapshotRam(16384);
    unsigned clockReads = 0;
    ArmHyperV clockSnapshot(true, snapshotRam, base, 2, [&] { ++clockReads; return uint64_t(1000); });
    clockSnapshot.setPrivateInterruptSender([](uint64_t, unsigned) { return true; });
    clockSnapshot.advertiseSynic(true);
    clockSnapshot.enableVmbus();
    clockSnapshot.enableHeartbeat();
    clockSnapshot.enableTimeSync([] { return uint64_t(116444736000000000ull); });
    clockSnapshot.enableShutdown();
    clockSnapshot.enableKvp();
    clockReads = 0;
    clockSnapshot.serviceDelay100ns();
    check(clockReads == 1);
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
    check(timerHv.report().find("\"timerActivity\":{\"configWrites\":") != std::string::npos);
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
    check(synic.post_message(1, 2, 80, 0, payload) == 0 && synic.needs_service());
    check(synic.service_message_queues() == 0 && synicRam[slot] == 79); // Still occupied.
    put(synicRam, slot, 0, 4);
    check(synic.service_message_queues() == 0 && synicRam[slot] == 80 && !synic.needs_service());
    check(synic.post_message(1, 2, 81, 0, payload) == 0);
    check(synic.write_vp_register(0xa0013, 1, base + 4096) == 0 && !synic.needs_service());
    check(synic.write_vp_register(0xa0013, 1, base + 4096 + 1) == 0 && synic.needs_service());
    put(synicRam, slot, 0, 4);
    check(synic.service_message_queues() == 0 && synicRam[slot] == 81 && !synic.needs_service());
    check(synic.post_message(1, 2, 82, 0, payload) == 0);
    check(synic.write_vp_register(0xa0002, 1, 16) == 0);
    put(synicRam, slot, 0, 4);
    auto beforeRescanFailure = synicRam;
    check(synic.service_message_queues() == 0x71 && synic.needs_service() && synicRam == beforeRescanFailure);
    synic.set_interrupt_sender([](uint64_t, unsigned) { return true; });
    check(synic.service_message_queues() == 0 && synicRam[slot] == 82 && !synic.needs_service());
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
    check(receiver.write_vp_register(0xa0010, 0, 1) == 0 && receiver.write_vp_register(0xa0012, 0, base + 4096 + 1) == 0);
    check(receiver.write_vp_register(0xa0002, 0, 16) == 0);
    unsigned connectionIrqs = 0;
    connected.setPrivateInterruptSender([&](uint64_t mask, unsigned vector) {
        check(mask == 1 && vector == 16); ++connectionIrqs; return true;
    });
    check(connected.bindEventConnection(42, 10, [&](unsigned vp, uint16_t relative) {
        check(vp == 1); bool newly = false;
        return receiver.signal_event(0, 2, 100 + relative, newly);
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
        check(connected.report().find("\"connectionId\":7,\"reserved\":0,\"messageType\":1,\"payloadBytes\":240,\"vmbusPrefixHex\":\"\"") != std::string::npos);
        check(connectionCall(true, smccc, true) == 3);
        check(connected.report().find("\"packetObserved\":false") != std::string::npos);
        for (auto extra : {one, 1ull << 17, 1ull << 31, 1ull << 48})
            check(connectionCall(true, smccc, false, extra) == 3);
        check(connectionCall(true, smccc, false, 0, base + 1) == 4);
        check(connectionCall(true, smccc, false, 0, base + 4096 - 8) == 3);
        check(connectionCall(true, smccc, false, 0, base + 8192) == 3);
    }
    put(connectionRam, 8, 0, 4); check(connectionCall(true, true, false) == 5);
    put(connectionRam, 8, 0x80000001, 4); check(connectionCall(true, true, false) == 5);
    put(connectionRam, 8, 1, 4); put(connectionRam, 12, 241, 4); check(connectionCall(true, true, false) == 5);
    put(connectionRam, 12, 240, 4); put(connectionRam, 4, 1, 4); check(connectionCall(true, true, false) == 0);
    put(connectionRam, 4, 0, 4); put(connectionRam, 0, 42, 4); check(connectionCall(true, true, false) == 0x11);
    put(connectionRam, 0, 7); check(connectionCall(false, true, true) == 0x11);
    check(connected.removeConnection(7) && !connected.removeConnection(7));
    check(connected.bindMessageConnection(7, [&](unsigned, uint32_t, std::span<const uint8_t>) {
        check(connected.removeConnection(7)); return uint16_t(0x13);
    }));
    put(connectionRam, 8, 1, 4); put(connectionRam, 12, 0, 4);
    check(connectionCall(true, true, false) == 0x13); // Preserve endpoint backpressure.
    check(connectionCall(true, true, false) == 0x12); // Callback removed itself safely.
    check(connected.report().find("\"postedMessages\":3") != std::string::npos);
    check(connected.report().find("\"signaledEvents\":4") != std::string::npos);
    for (uint32_t id = 100; id < 163; ++id)
        check(connected.bindMessageConnection(id, [](unsigned, uint32_t, std::span<const uint8_t>) { return uint16_t(0); }));
    check(!connected.bindMessageConnection(163, [](unsigned, uint32_t, std::span<const uint8_t>) { return uint16_t(0); }));
    r = {}; r[0] = 0x46000001; r[1] = one | 0x10050;
    r[2] = UINT64_MAX; r[3] = 0xfffffffe; r[4] = 0x200;
    check(connected.call(0, r, 0) && r[0] == one && r[6] == (features | (1ull << 36) | (1ull << 37)));
    // ARM SynIC sources may target SGIs as well as PPIs. The controller
    // must pass the INTID unchanged, including zero, without an APIC offset.
    std::vector<uint8_t> sgiRam(4096);
    ArmSynic sgiSynic(sgiRam, base, 1);
    check(sgiSynic.write_vp_register(0xa0010, 0, 1) == 0 && sgiSynic.write_vp_register(0xa0012, 0, base + 1) == 0);
    unsigned sgiNotifications = 0, expectedIntid = 0;
    sgiSynic.set_interrupt_sender([&](uint64_t mask, unsigned intid) {
        check(mask == 1 && intid == expectedIntid); ++sgiNotifications; return true;
    });
    for (unsigned intid = 0; intid < 32; ++intid) {
        expectedIntid = intid;
        check(sgiSynic.write_vp_register(0xa0001, 0, intid) == 0);
        check(sgiSynic.read_vp_register(0xa0001, 0, value) == 0 && value == intid);
        check(sgiSynic.signal_event(0, 1, intid, newly) == 0 && newly);
        check(sgiRam[256 + intid / 8] & (1u << (intid % 8)));
    }
    check(sgiNotifications == 32);
    check(sgiSynic.write_vp_register(0xa0001, 0, 32) == 5);
    check(sgiSynic.read_vp_register(0xa0001, 0, value) == 0 && value == 31);
    ArmHyperV profile(true, sgiRam, base, 1), disabledProfile(false, sgiRam, base, 1);
    bool rejectedProfile = false;
    try { profile.advertiseSynic(true); } catch (const std::runtime_error &) { rejectedProfile = true; }
    check(rejectedProfile);
    disabledProfile.setPrivateInterruptSender([](uint64_t, unsigned) { return true; });
    rejectedProfile = false;
    try { disabledProfile.advertiseSynic(true); } catch (const std::runtime_error &) { rejectedProfile = true; }
    check(rejectedProfile);
    profile.setPrivateInterruptSender([](uint64_t, unsigned) { return true; });
    profile.advertiseSynic(true);
    r = {}; r[0] = 0x46000001; r[1] = one | 0x10050;
    r[2] = UINT64_MAX; r[3] = 0xfffffffe; r[4] = 0x200;
    check(profile.call(0, r, 0) && r[0] == one && r[6] == (features | 4));
    check(profile.report().find("\"mode\":\"synic\"") != std::string::npos);
    rejectedProfile = false;
    try { profile.setPrivateInterruptSender({}); } catch (const std::runtime_error &) { rejectedProfile = true; }
    check(rejectedProfile);
    profile.advertiseSynic(false);
    check(profile.report().find("\"mode\":\"basic\"") != std::string::npos);
    std::vector<uint8_t> busRam(16384);
    ArmVmbus bus(busRam, base, 2);
    std::vector<uint8_t> contact(40), request(8), response;
    put(contact, 0, 14, 4); put(contact, 8, 0x60000, 4); put(contact, 12, 1, 4); contact[16] = 2;
    unsigned busReplies = 0;
    bool busReady = false;
    bus.setSender([&](unsigned vp, unsigned sint, std::span<const uint8_t> bytes) {
        check(vp == 1 && sint == 2);
        if (!busReady) return uint16_t(0x18);
        response.assign(bytes.begin(), bytes.end()); ++busReplies; return uint16_t(0);
    });
    put(request, 0, 3, 4); check(bus.receive(1, request) == 0x15);
    check(bus.receive(1, contact) == 0 && bus.needsService());
    bus.service(); check(busReplies == 0 && bus.needsService());
    busReady = true; bus.service();
    check(busReplies == 1 && !bus.needsService() && response.size() == 20);
    check(get(response, 0) == 15 && response[8] == 1 && response[9] == 0);
    check(get(response, 12) == 1); // Connection ID 1, features zero.
    check(bus.receive(1, contact) == 0x15); // No implicit reconnection while live.
    check(bus.receive(1, request) == 0); bus.service();
    check(response.size() == 8 && get(response, 0) == 4 && busReplies == 2);
    put(request, 0, 16, 4); check(bus.receive(1, request) == 0); bus.service();
    check(get(response, 0) == 17 && busReplies == 3);
    for (uint32_t version : {0x50000u, 0x50001u, 0x50002u, 0x50003u, 0x60000u}) {
        put(contact, 8, version, 4);
        check(bus.receive(1, contact) == 0); bus.service();
        check(response[8] == 1 && response.size() == (version == 0x60000 ? 20 : 16));
        check(bus.receive(1, request) == 0); bus.service();
        check(get(response, 0) == 17);
    }
    put(contact, 8, 0x60001, 4); check(bus.receive(1, contact) == 0); bus.service();
    check(response.size() == 16 && response[8] == 0 &&
          std::all_of(response.begin() + 12, response.end(), [](uint8_t byte) { return byte == 0; }));
    put(contact, 8, 0x60000, 4);
    auto invalidContact = [&](size_t offset, uint64_t value, unsigned bytes, uint16_t status) {
        auto copy = contact; put(copy, offset, value, bytes);
        check(bus.receive(1, copy, true) == status && !bus.needsService());
    };
    invalidContact(4, 1, 4, 5); invalidContact(12, 2, 4, 14);
    invalidContact(16, 16, 1, 5); invalidContact(17, 1, 1, 6);
    invalidContact(18, 1, 2, 5); invalidContact(24, base + 1, 8, 5);
    invalidContact(24, base + 16384, 8, 5); invalidContact(24, UINT64_MAX - 4095, 8, 5);
    check(bus.receive(2, contact) == 5);
    check(bus.receive(1, std::span<const uint8_t>(contact).first(39)) == 5);
    put(contact, 20, 8, 4); check(bus.receive(1, contact) == 5); // Missing client GUID.
    contact.resize(56); check(bus.receive(1, contact) == 0); bus.service();
    check(response.size() == 20 && get(response, 12) == 1); // No client-ID feature negotiated.
    check(bus.receive(1, request) == 0); bus.service();
    put(contact, 20, 0, 4); put(contact, 24, base, 8); put(contact, 32, base, 8);
    check(bus.receive(1, contact) == 5); // Monitor directions may not alias.
    put(contact, 32, base + 4096, 8); check(bus.receive(1, contact) == 0); bus.service();
    check(response[8] == 1);
    check(bus.receive(1, request) == 0); bus.service();
    // Keep replies across backend absence and enforce a finite host queue.
    bus.setSender({}); put(contact, 8, 0x70000, 4);
    for (unsigned i = 0; i < 16; ++i) check(bus.receive(1, contact) == 0);
    check(bus.receive(1, contact) == 0x13 && bus.needsService());
    bus.service(); check(bus.needsService());
    bus.setSender([](unsigned, unsigned, std::span<const uint8_t>) { return uint16_t(0); });
    bus.service(); check(!bus.needsService());
    check(bus.report().find("\"offeredChannels\":0") != std::string::npos);
    for (bool smccc : {false, true}) {
        std::vector<uint8_t> integratedRam(16384);
        ArmHyperV integrated(true, integratedRam, base, 2);
        bool rejected = false;
        try { integrated.enableVmbus(); } catch (const std::runtime_error &) { rejected = true; }
        check(rejected);
        unsigned notifications = 0;
        integrated.setPrivateInterruptSender([&](uint64_t mask, unsigned intid) {
            check(mask == 2 && intid == 16); ++notifications; return true;
        });
        integrated.advertiseSynic(true);
        integrated.enableVmbus(); integrated.enableVmbus(); // Idempotent setup.
        r = {}; r[0] = 0x46000001; r[1] = one | 0x10050;
        r[2] = UINT64_MAX; r[3] = 0xfffffffe; r[4] = 0x200;
        check(integrated.call(0, r, 0) && r[0] == one &&
              r[6] == (features | 0x1c | (1ull << 36) | (1ull << 37)));
        check((r[6] & 0x7e) == 0x7e); // WinHv driver initialization prerequisite.
        check(!integrated.removeConnection(1) && !integrated.removeConnection(4));
        check(!integrated.bindMessageConnection(4, [](unsigned, uint32_t, std::span<const uint8_t>) { return uint16_t(0); }));
        rejected = false;
        try { integrated.advertiseSynic(false); } catch (const std::runtime_error &) { rejected = true; }
        check(rejected);
        auto &controller = integrated.synicController();
        check(controller.write_vp_register(0xa0010, 1, 1) == 0);
        check(controller.write_vp_register(0xa0013, 1, base + 4096 + 1) == 0);
        check(controller.write_vp_register(0xa0002, 1, 16) == 0);
        r = {}; r[0] = 0x46000001; r[1] = one | 0x10051;
        r[2] = UINT64_MAX; r[3] = 0xfffffffe; r[4] = 0x90002; r[6] = 0x1000a000066bbull;
        check(integrated.call(0, r, 0) && r[0] == one);
        auto post = [&](uint32_t connection, uint32_t kind, unsigned bytes) {
            std::fill(integratedRam.begin(), integratedRam.begin() + 256, 0);
            put(integratedRam, 0, connection, 4); put(integratedRam, 4, 0xfeedbeef, 4);
            put(integratedRam, 8, 1, 4);
            put(integratedRam, 12, bytes, 4); put(integratedRam, 16, kind, 4);
            if (kind == 14) {
                put(integratedRam, 24, 0x60000, 4); put(integratedRam, 28, 1, 4);
                integratedRam[32] = 2;
            }
            r = {}; r[0] = smccc ? 0x46000001 : 0x5c;
            r[smccc ? 1 : 0] = 0x5c; r[smccc ? 2 : 1] = base;
            if (smccc) r[0] = 0x46000001;
            check(integrated.call(smccc ? 0 : 1, r, 0) && r[0] == 0);
            check(integrated.serviceDelay100ns().value_or(10001) <= 10000);
            integrated.service();
        };
        post(4, 14, 40);
        check(integrated.report().find("\"vmbusPrefixHex\":\"0e00000000000000000006000100000002") != std::string::npos);
        const size_t slot = 4096 + 2 * 256;
        check(notifications == 1 && get(integratedRam, slot, 4) == 1);
        check(integratedRam[slot + 4] == 20 && get(integratedRam, slot + 16, 4) == 15);
        check(integratedRam[slot + 24] == 1 && get(integratedRam, slot + 28, 4) == 1);
        post(1, 3, 8); // Occupied version reply must survive the request-offers reply.
        check(get(integratedRam, slot + 16, 4) == 15 && notifications == 1);
        put(integratedRam, slot, 0, 4); integrated.service();
        check(get(integratedRam, slot + 16, 4) == 4 && notifications == 2);
        put(integratedRam, slot, 0, 4);
        post(1, 16, 8);
        check(get(integratedRam, slot + 16, 4) == 17 && notifications == 3);
        check(integrated.report().find("\"mode\":\"vmbus\"") != std::string::npos);
        check(integrated.report().find("\"offeredChannels\":0") != std::string::npos);
    }
    ArmHyperV collision(true, busRam, base, 1);
    collision.setPrivateInterruptSender([](uint64_t, unsigned) { return true; });
    collision.advertiseSynic(true);
    check(collision.bindMessageConnection(1, [](unsigned, uint32_t, std::span<const uint8_t>) { return uint16_t(0); }));
    bool collisionRejected = false;
    try { collision.enableVmbus(); } catch (const std::runtime_error &) { collisionRejected = true; }
    check(collisionRejected && collision.removeConnection(1));
    check(collision.bindMessageConnection(4, [](unsigned, uint32_t, std::span<const uint8_t>) { return uint16_t(0); }));
    ArmHyperV pageDiagnostic(true, busRam, base, 2);
    auto diagnosticWrite = [&](uint32_t name, uint64_t low, uint64_t high, unsigned vp) {
        r = {}; r[0] = 0x46000001; r[1] = one | 0x10051;
        r[2] = UINT64_MAX; r[3] = 0xfffffffe; r[4] = name; r[6] = low; r[7] = high;
        check(pageDiagnostic.call(0, r, vp)); return r[0];
    };
    check(diagnosticWrite(0x90002, 0x5043454d55, 0, 0) == one);
    check(diagnosticWrite(0xa0013, base + busRam.size() + 1, 0, 1) == 5);
    check(pageDiagnostic.report().find("\"lastWriteLow\":" + std::to_string(base + busRam.size() + 1)) != std::string::npos);
    check(pageDiagnostic.report().find("\"lastWriteVp\":1,\"lastWriteStatus\":5") != std::string::npos);
    check(diagnosticWrite(0xa0013, base + 1, 0, 0) == one);
    check(pageDiagnostic.report().find("\"lastWriteLow\":" + std::to_string(base + 1)) != std::string::npos);
    check(pageDiagnostic.report().find("\"lastWriteVp\":0,\"lastWriteStatus\":0") != std::string::npos);
    check(diagnosticWrite(0xa0013, 0x7ffff001, 0, 0) == 5); // Outside this tiny test RAM.
    r = {}; r[0] = 0x46000001; r[1] = one | 0x10051;
    r[2] = UINT64_MAX; r[3] = 0xfffffffe; r[4] = 0xa0013 | (0xbeefull << 32);
    r[5] = 0x11223344; r[6] = base + 1;
    check(pageDiagnostic.call(0, r, 0) && r[0] == one);
    check(pageDiagnostic.report().find("\"lastWriteNamePadding\":48879,\"lastWritePadding\":287454020,\"lastWriteFast\":true") != std::string::npos);
    for (bool smccc : {false, true}) for (bool fast : {false, true}) {
        std::fill(busRam.begin(), busRam.begin() + 256, 0);
        put(busRam, 0, UINT64_MAX); put(busRam, 8, 0xfffffffe, 4);
        put(busRam, 16, 0xa0013, 4); put(busRam, 20, 0x4567, 4);
        put(busRam, 24, 0x737475767778797aull); put(busRam, 32, base + 4096 + 1);
        r = {}; r[0] = smccc ? 0x46000001 : (one | 0x51 | (fast ? 0x10000 : 0));
        if (smccc) r[1] = one | 0x51 | (fast ? 0x10000 : 0);
        unsigned start = smccc ? 2 : 1;
        if (fast) for (unsigned i = 0; i < 6; ++i) r[start + i] = get(busRam, i * 8);
        else r[start] = base;
        check(pageDiagnostic.call(smccc ? 0 : 1, r, 0) && r[0] == one);
        check(pageDiagnostic.synicController().read_vp_register(0xa0013, 0, value) == 0 && value == base + 4096 + 1);
        // Padding tolerance does not permit nonzero bits beyond a 64-bit register.
        if (fast) r[start + 5] = 1; else put(busRam, 40, 1);
        r[0] = smccc ? 0x46000001 : (one | 0x51 | (fast ? 0x10000 : 0));
        check(pageDiagnostic.call(smccc ? 0 : 1, r, 0) && r[0] == 5);
    }
    ArmVmbus ordinaryBus(busRam, base, 2);
    unsigned ordinaryReplies = 0;
    ordinaryBus.setSender([&](unsigned vp, unsigned sint, std::span<const uint8_t> bytes) {
        check(vp == 0 && sint == 2 && bytes.size() >= 8); ++ordinaryReplies; return uint16_t(0);
    });
    std::vector<uint8_t> ordinaryContact(40), ordinaryUnload(8);
    put(ordinaryContact, 0, 14, 4); put(ordinaryContact, 8, 0x60000, 4);
    put(ordinaryUnload, 0, 16, 4);
    check(ordinaryBus.receive(1, ordinaryContact) == 0); ordinaryBus.service();
    check(ordinaryReplies == 1 && ordinaryBus.report().find("\"targetSint\":2") != std::string::npos);
    check(ordinaryBus.receive(1, ordinaryUnload) == 0); ordinaryBus.service();
    // Lower target-info fields are unused on the ordinary connection.
    put(ordinaryContact, 16, 0xffffffff, 4);
    check(ordinaryBus.receive(1, ordinaryContact) == 0); ordinaryBus.service();
    check(ordinaryReplies == 3);
    ArmVmbus scopedBus(busRam, base, 2);
    scopedBus.setSender([](unsigned vp, unsigned sint, std::span<const uint8_t>) {
        check(vp == 1 && sint == 5); return uint16_t(0);
    });
    put(ordinaryContact, 12, 1, 4); put(ordinaryContact, 16, 5, 4);
    check(scopedBus.receive(1, ordinaryContact, true) == 0); scopedBus.service();
    check(scopedBus.report().find("\"targetSint\":5") != std::string::npos);
    {
        std::vector<uint8_t> ram(16384,0xa5);
        uint64_t now=0,counter=24000000;
        ArmHyperV ref(true,ram,base,2,[&]{return now;});
        auto access=[&](uint32_t name,bool write=false,uint64_t value=0,uint64_t high=0,unsigned vp=0) {
            ArmHyperV::Registers x{};x[0]=0x46000001;x[1]=one|(write?0x10051:0x10050);
            x[2]=~0ull;x[3]=0xfffffffe;x[4]=name;x[6]=value;x[7]=high;
            check(ref.call(0,x,vp));return x;
        };
        check(!(access(0x200)[6]&(1ull<<9)));
        check(access(0x90017)[0]==5);
        check(!ref.enableReferenceClock([]{return ArmHyperV::CounterSample{0,10000000};}));
        check(ref.enableReferenceClock([&]{return ArmHyperV::CounterSample{counter,24000000};}));
        check(!ref.enableReferenceClock([&]{return ArmHyperV::CounterSample{counter,24000000};}));
        check(access(0x200)[6]&(1ull<<9));
        check(access(0x90017)[0]==6);
        check(access(0x90002,true,42)[0]==one);
        check(access(0x90017)[6]==0);
        auto prior=ram;
        check(access(0x90017,true,base+4099)[0]==5 && ram==prior);
        check(access(0x90017,true,base+4097,1)[0]==5 && ram==prior);
        check(access(0x90017,true,base+16385)[0]==5 && ram==prior);
        check(access(0x90017,true,base+4097)[0]==one);
        check(access(0x90017,false,0,0,1)[6]==base+4097);
        auto sequence=get(ram,4096,4);check(sequence!=0);
        auto readPage=[&]{return uint64_t((__uint128_t(counter)*get(ram,4104))>>64)+get(ram,4112);};
        check(readPage()==0);
        ref.rebaseReferenceClock(true);check(get(ram,4096,4)==0);
        counter+=120000000;now+=10000000;
        ref.rebaseReferenceClock(false);
        check(get(ram,4096,4)!=sequence && get(ram,4096,4)!=0);
        check(readPage()==now);
        check(access(0x90017,true,0)[0]==one && get(ram,4096,4)==0);
        check(access(0x90017)[6]==0);
    }
    std::cout << checks << " ARM Hyper-V checks passed\n";
}
