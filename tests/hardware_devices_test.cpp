#include "ata.hpp"
#include "disk.hpp"
#include "i8259_pair.hpp"
#include "pit.hpp"
#include "vga.hpp"
#include "pci.hpp"
#include "ne2000.hpp"
#include "network.hpp"
#include "system_port_b.hpp"
#include "port92.hpp"
#include "debug_console.hpp"
#include "arm_pl011.hpp"
#include "arm_pl031.hpp"
#include "arm_nor_flash.hpp"
#include "arm_fw_cfg.hpp"
#include "arm_ramfb.hpp"
#include "virtio_blk.hpp"
#include "virtio_net.hpp"
#include "virtio_rng.hpp"
#include "nvme.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace pc;
static unsigned checks;
static void check(bool condition) {
    ++checks;
    if (!condition) throw std::runtime_error("Hardware device check " + std::to_string(checks));
}
int main() {
    auto root = std::filesystem::temp_directory_path() / ("pcemu-devices-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    try {
        {
            std::ofstream raw(root / "disk.raw", std::ios::binary);
            raw.seekp(64 * 512 - 1); raw.put(0);
        }
        Disk disk((root / "disk.raw").string());
        ATA ata(&disk);
        ata.out(0x1f7, 0xec, 1);
        check(ata.interruptLevel() && (ata.in(0x3f6, 1) & 8));
        check(ata.interruptLevel());
        check(ata.in(0x1f7, 1) & 8); check(!ata.interruptLevel());
        check(ata.in(0x1f0, 2) == 0x40);
        for (unsigned i = 1; i < 256; ++i) ata.in(0x1f0, 2);
        ata.out(0x1f2, 1, 1); ata.out(0x1f3, 2, 1); ata.out(0x1f6, 0xe0, 1);
        ata.out(0x1f7, 0x30, 1);
        for (unsigned i = 0; i < 256; ++i) ata.out(0x1f0, 0x5a5a, 2);
        std::array<uint8_t, 512> sector{};
        check(disk.read(2, 1, sector.data()) && sector.front() == 0x5a && sector.back() == 0x5a);
        check(ata.interruptLevel()); ata.out(0x3f6, 2, 1); check(!ata.interruptLevel());
        ata.reset(&disk); check(!ata.interruptLevel());

        I8259PairDevice pic;
        pic[0].base = 0x20; pic[1].base = 0x28;
        pic[0].mask = 0xfb; pic[1].mask = 0xbf;
        pic.raise(14);
        check(!pic.nextVector(false) && (pic[0].irr & 4));
        check(pic.nextVector(true) == 0x2e && (pic[0].isr & 4) && (pic[1].isr & 0x40));
        check(pic.state().ataInService);
        pic.write(1, false, 0x20); pic.write(0, false, 0x20);
        check(!pic.state().ataInService && pic[0].isr == 0);
        pic[0].mask = 0xfe; pic.raise(0);
        check(!pic.nextVector(false) && pic.state().timerPending);
        check(pic.nextVector(true) == 0x20 && pic.state().timerInService);
        pic.reset(); check(pic[0].mask == 0xff && !pic.state().cascadeLine);
        PIT8254 pit; pit.reset(1234); check(pit.counters[0].loadedAt == 1234);
        SystemPortBDevice portB;
        check(portB.write(0xcf) && portB.read(false, false) == 0xcf);
        check(portB.read(true, true) == 0xff);
        portB.reset(); check(portB.read(false, true) == 0x20);
        portB.restore(1); check(portB.read(false, false) == 1);
        Port92Device fast;
        check(fast.read(false) == 0 && fast.read(true) == 2);
        check(fast.write(3).a20 && fast.write(3).reset && !fast.write(0).a20);
        std::string output;
        DebugConsoleDevice debug;
        debug.bind([&](uint8_t value) { output.push_back(char(value)); });
        debug.write('A'); debug.restore('B');
        check(output == "A" && debug.read() == 'B');
        debug.reset(); check(debug.read() == 0 && output == "A");
        VGA vga;
        check(vga.portRead(0x3da, 1) == 9 && vga.portRead(0x3da, 2) == 0);
        vga.dispiPortWrite(false, 1); vga.dispiPortWrite(true, 800);
        check(vga.dispiPortRead(true) == 800);
        PCI bus;
        bus.out(0xcf8, 0x80001000, 4); check(bus.in(0xcfc, 4) == 0x11111234);
        bus.out(0xcf8, 0x80001010, 4); bus.out(0xcfc, 0xffffffffu, 4);
        check(bus.display().barProbe && bus.in(0xcfc, 4) == 0xff000000u);
        bus.reset(); check(!bus.address && !bus.display().barProbe);
        NE2000 nic; check(!nic.irq()); nic.reset();
        Network gateway; // Host packet backend also links without an execution engine.
        ArmPl011 uart; uart.write(0x30, 0x301); uart.write(0, 'Z'); check(uart.output() == "Z");
        ArmPl031 rtc([] { return int64_t(1000); }); check(rtc.read(0) == 1000);
        std::vector<uint8_t> flashBytes(0x40000, 0xff), ram(65536);
        ArmNorFlash flash(flashBytes); check(flash.bytes().front() == 0xff);
        ArmFwCfg config(ram, 0x40000000); config.addFile("etc/ramfb", std::vector<uint8_t>(28), true);
        ArmRamFb fb(config, ram, 0x40000000); check(fb.mode() == "inactive");
        auto irq = [](bool) {};
        VirtioBlock block(disk, ram, 0x40000000, irq);
        VirtioNet network(ram, 0x40000000, irq);
        VirtioRng entropy(ram, 0x40000000, irq);
        Nvme nvme(disk, ram, 0x40000000, irq);
        check(block.read(0) == 0x74726976 && network.read(0) == 0x74726976 && entropy.read(0) == 0x74726976);
        check((nvme.configRead(0, 4) & 0xffff) == 0x1b36);
        std::filesystem::remove_all(root);
        std::cout << checks << " standalone hardware device checks passed (no CPU/machine backend)\n";
    } catch (...) { std::filesystem::remove_all(root); throw; }
}
