#include "arm_pl031.hpp"
#include "arm_os_lock.hpp"
#include "arm_psci.hpp"
#include "arm_nor_flash.hpp"
#include "arm_pl011.hpp"
#include "arm_mmio.hpp"
#include "arm_fw_cfg.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
using namespace pc;
static unsigned checks = 0;
static void check(bool condition) {
    ++checks;
    if (!condition) throw std::runtime_error("ARM device check " + std::to_string(checks));
}
template <class F> static void rejects(F f) {
    bool rejected = false;
    try { f(); } catch (const std::runtime_error &) { rejected = true; }
    check(rejected);
}
int main() {
    unsigned rtcSamples = 0;
    ArmPl031 idleRtc([&] { ++rtcSamples; return int64_t(123); });
    check(!idleRtc.irq() && !idleRtc.irq() && rtcSamples == 1);
    int64_t now = 1792800000;
    ArmPl031 rtc([&] { return now; });
    check(rtc.read(0) == now);
    check(rtc.read(0xfe0) == 0x31 && rtc.read(0xffc) == 0xb1);
    rtc.write(4, uint32_t(now + 2));
    now += 3; // Polling can skip the matching second.
    check(rtc.read(0x14) == 1 && !rtc.irq());
    rtc.write(0x10, 1);
    check(rtc.irq());
    rtc.write(0x1c, 1);
    check(!rtc.irq());
    rtc.write(8, 100);
    check(rtc.read(0) == 100 && rtc.read(8) == 100);
    now += 7;
    check(rtc.read(0) == 107);
    rtc.write(0xc, 0);
    check(rtc.read(0xc) == 1);
    rtc.write(4, 110);
    now -= 20;
    check(!rtc.irq()); // Backwards wall-clock adjustment does not trigger alarm.
    now += 23;
    check(rtc.irq());
    rtc.write(0x1c, 1);
    rtc.write(8, 0xfffffffe);
    rtc.write(4, 1);
    now += 4;
    check(rtc.read(0) == 2 && rtc.irq());
    // SPEC: ARM DDI 0224C, sections 3.3.5-3.3.8: Reserved mask bits have no effect.
    rtc.write(0x10, 2);
    check(rtc.read(0x10) == 0 && rtc.read(0x14) == 1 && rtc.read(0x18) == 0);
    rtc.write(0x1c, 2);
    check(rtc.read(0x14) == 1);
    rtc.write(0x10, 3);
    check(rtc.read(0x10) == 1 && rtc.read(0x18) == 1);
    rtc.write(0x1c, 1);
    check(rtc.read(0x14) == 0 && rtc.read(0x18) == 0);
    // AD-HOC: Preserve unsupported-access fallback and the always-enabled RTCCR.
    rtc.write(0x20, 0xffffffff);
    rtc.write(0x00, 0);
    check(rtc.read(0x20) == 0 && rtc.read(0) == 2);
    check(armPsciCall(0x84000000, 0, 0).value == 2);
    check(armPsciCall(0xc4000003, 0, 0).value == -4);
    check(armPsciCall(0xc4000003, 1, 0).value == -2);
    check(armPsciCall(0x84000003, 1ull << 32, 0).value == -4);
    check(armPsciCall(0xc4000003, 1ull << 32, 0).value == -2);
    check(armPsciCall(0xc4000004, 0, 0).value == 0);
    check(armPsciCall(0xc4000004, 0, 1).value == -2);
    check(armPsciCall(0xc4000004, 1, 0).value == -2);
    check(armPsciCall(0x84000006, 0, 0).value == 2);
    check(armPsciCall(0xc4000001, 0, 0).value == 0);
    check(armPsciCall(0xc4000001, 0x10000, 0).value == -2);
    check(armPsciCall(0x84000002, 0, 0).value == -3);
    check(armPsciCall(0x84000008, 0, 0).action == ArmPowerAction::Off);
    check(armPsciCall(0x84000009, 0, 0).action == ArmPowerAction::Reset);
    check(armPsciCall(0x8400000a, 0, 0).value == -1);
    check(armPsciCall(0x80000000, 0, 0).value == -1);
    std::vector<uint8_t> memory(4096);
    ArmFwCfg cfg(memory, 0x40000000);
    check(cfg.read(0, 4) == 0x554d4551);
    cfg.write(8, 0x100, 2);
    check(cfg.read(0, 4) == 3);
    ArmFwCfg multicpu(memory, 0x40000000, 4);
    multicpu.write(8, 0x500, 2);
    check(multicpu.read(0, 2) == 4);
    multicpu.write(8, 0x0f00, 2);
    check(multicpu.read(0, 2) == 4);
    auto key = cfg.addFile("etc/ramfb", std::vector<uint8_t>(28), true);
    check(key == 0x20);
    rejects([&] { cfg.addFile("etc/ramfb", {}); });
    cfg.write(8, 0x1900, 2);
    check(cfg.read(0, 4) == 0x01000000);
    check(cfg.read(0, 4) == 0x1c000000);
    check(cfg.read(0, 2) == 0x2000);
    auto putBe = [&](size_t offset, uint64_t value, size_t count) {
        for (size_t i = count; i; --i) { memory[offset + i - 1] = value; value >>= 8; }
    };
    auto transfer = [&](uint32_t control, uint32_t length, uint64_t target) {
        putBe(0, control, 4); putBe(4, length, 4); putBe(8, target, 8);
        cfg.write(16, 0, 4); cfg.write(20, 0x40, 4);
        return memory[3];
    };
    memory[128] = 0x42;
    check(transfer((key << 16) | 8 | 16, 1, 0x40000080) == 0);
    check(cfg.file(key)[0] == 0x42);
    check(transfer((key << 16) | 8 | 2, 32, 0x40000100) == 0);
    check(memory[256] == 0x42 && memory[284] == 0 && memory[287] == 0);
    check(transfer((key << 16) | 8 | 16, 29, 0x40000080) == 1);
    check(cfg.file(key)[0] == 0x42);
    check(transfer(8 | 16, 1, 0x40000080) == 1); // Read-only signature.
    check(transfer(8 | 2, 32, 0x40000fff) == 1);
    check(transfer((key << 16) | 8 | 4, 27, 0) == 0);
    check(transfer(16, 2, 0x40000080) == 1); // No partial write beyond item.
    cfg.write(8, 0xffff, 2); check(cfg.read(0, 8) == 0);
    check(cfg.read(16, 8) == 0x47464320554d4551ull);
    rejects([&] { cfg.write(16, 0xffffffffffffffffull, 8); }); // Out-of-RAM descriptor is a platform error.
    std::vector<uint8_t> blank(0x80000, 0xff);
    ArmNorFlash flash(blank);
    flash.write(0, 0x00900090);
    check(flash.bytes()[0] == 0x89 && flash.bytes()[4] == 0x18 && flash.bytes()[8] == 0);
    flash.write(0, 0x00ff00ff);
    check(flash.bytes()[0] == 0xff && flash.bytes()[8] == 0xff);
    flash.write(0, 0x00700070);
    check(flash.bytes()[0] == 0x80 && flash.bytes()[2] == 0x80);
    check(flash.storage()[0] == 0xff);
    flash.write(0, 0x00ff00ff);
    flash.write(0, 0x00400040);
    flash.write(0, 0x12345678);
    check(flash.storage()[0] == 0x78 && flash.storage()[3] == 0x12);
    check(flash.bytes()[0] == 0x80);
    flash.write(0, 0x00ff00ff);
    check(flash.bytes()[0] == 0x78 && flash.bytes()[3] == 0x12);
    flash.write(0, 0x00400040); flash.write(0, 0xffffffff); flash.write(0, 0x00ff00ff);
    check(flash.storage()[0] == 0x78); // Programming cannot set zero bits.
    flash.write(0x40000, 0x00e800e8);
    flash.write(0x40000, 0x00010001); // Two x32 words.
    flash.write(0x40020, 0xaabbccdd); flash.write(0x40024, 0x11223344);
    check(flash.storage()[0x40020] == 0xff); // Buffer is not committed early.
    flash.write(0x40000, 0x00d000d0); flash.write(0x40000, 0x00ff00ff);
    check(flash.storage()[0x40020] == 0xdd && flash.storage()[0x40024] == 0x44);
    flash.write(0, 0x00200020); flash.write(0, 0x00d000d0); flash.write(0, 0x00ff00ff);
    check(flash.storage()[0] == 0xff && flash.storage()[0x3ffff] == 0xff);
    check(flash.storage()[0x40020] == 0xdd); // Erase stays within its block.
    flash.write(0, 0x00600060); flash.write(0, 0x00d000d0); flash.write(0, 0x00ff00ff);
    check(flash.storage()[0x40020] == 0xdd);
    auto dirty = flash.takeDirty(); check(dirty.first == 0 && dirty.second >= 0x40028);
    check(flash.takeDirty().first == SIZE_MAX);
    rejects([&] { flash.write(1, 0xff); });
    rejects([&] { flash.write(UINT64_MAX - 3, 0xff); });
    rejects([&] { flash.write(0x80000, 0xff); });
    rejects([&] { flash.write(0, 0x1234); });
    rejects([] { std::array<uint8_t, 3> bytes{}; ArmNorFlash invalid(bytes); });

    check(armMmioLoad(0x1234, 1, false, true) == 0x34);
    check(armMmioLoad(UINT64_MAX, 2, false, false) == 0xffff);
    check(armMmioLoad(0x80, 1, true, true) == 0xffffffffffffff80ull);
    check(armMmioLoad(0x8001, 2, true, false) == 0xffff8001);
    check(armMmioLoad(0x80000000, 4, true, true) == 0xffffffff80000000ull);
    check(armMmioLoad(UINT64_MAX, 8, false, true) == UINT64_MAX);
    ArmOsLock osLock;
    uint64_t lockValue = 0;
    check(osLock.access(0x62280503, lockValue) && lockValue == 10);
    lockValue = 0;
    check(osLock.access(0x62280400, lockValue));
    check(osLock.access(0x622807e3, lockValue) && lockValue == 8); // Rt=XZR.
    lockValue = UINT64_MAX;
    check(osLock.access(0x62280420, lockValue));
    check(osLock.access(0x62280403, lockValue) && lockValue == 10);
    check(!osLock.access(0x62280401, lockValue)); // OSLAR is write-only.
    check(!osLock.access(0x62280402, lockValue)); // OSLSR is read-only.
    check(!osLock.access(0x62280409, lockValue)); // Other CRm.
    check(!osLock.access(0x62080403, lockValue)); // Other op0.
    check(!osLock.access(0x60280403, lockValue)); // Invalid IL.
    check(!osLock.access(0x66280403, lockValue)); // Other EC.
    ArmOsLock freshLock;
    check(freshLock.access(0x62280403, lockValue) && lockValue == 10);
    lockValue = 0;
    check(freshLock.access(0x622807e6, lockValue)); // Linux clears OSDLR via XZR.
    check(freshLock.access(0x62280407, lockValue) && lockValue == 0);
    lockValue = 1;
    check(freshLock.access(0x62280406, lockValue));
    lockValue = 0;
    check(freshLock.access(0x62280406, lockValue));
    check(freshLock.access(0x62280407, lockValue) && lockValue == 1);
    lockValue = 0;
    check(freshLock.access(0x62280400, lockValue));
    check(freshLock.access(0x62280403, lockValue) && lockValue == 10);
    ArmPl011 uart;
    check(uart.read(0x18) == 0x90);
    uart.write(0, 'X'); check(uart.output().empty()); // UART disabled at reset.
    uart.write(0x30, 0x301); uart.write(0, 'A'); uart.write(0, 0x142);
    check(uart.output() == "AB");
    uart.input("xy"); check(uart.read(0x18) == 0x80);
    check(uart.read(0) == 'x' && uart.read(0) == 'y');
    check(uart.read(0x18) == 0x90 && uart.read(0) == 0);
    uart.write(0x24, 0x1234); uart.write(0x28, 0xff); uart.write(0x2c, 0x70);
    check(uart.read(0x24) == 0x1234 && uart.read(0x28) == 63 && uart.read(0x2c) == 0x70);
    check(uart.read(0xfe0) == 0x11 && uart.read(0xffc) == 0xb1);
    check(uart.read(0x40) == 0 && !uart.irq());
    uart.write(0x38, 0x50);
    uart.input("z");
    check(uart.irq() && uart.read(0x40) == 0x50);
    uart.write(0x44, 0x7ff);
    check(uart.irq()); // Clearing cannot suppress a nonempty RX level.
    check(uart.read(0) == 'z' && !uart.irq());
    uart.write(0x38, 0x20);
    check(uart.irq() && uart.read(0x40) == 0x20);
    uart.write(0x30, 0);
    check(!uart.irq());
    uart.write(0x30, 0x301);
    uart.write(0x38, 0);
    check(!uart.irq());
    uart.write(0x34, 0x12);
    check(uart.read(0x34) == 0x12);
    rejects([&] { uart.input(std::string(4097, 'x')); });
    rejects([&] { uart.read(0x123); });
    rejects([&] { uart.write(0x123, 0); });
    std::cout << checks << " ARM device checks passed\n";
}
