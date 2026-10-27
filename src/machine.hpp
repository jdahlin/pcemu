#pragma once
#include "host_cursor.hpp"
#include "ata.hpp"
#include "absolute_mouse.hpp"
#include "cpu.hpp"
#include "cmos_rtc.hpp"
#include "disk.hpp"
#include "io_port_space.hpp"
#include "i8042.hpp"
#include "ne2000.hpp"
#include "pci.hpp"
#include "i8259_pair.hpp"
#include "system_port_b.hpp"
#include "port92.hpp"
#include "debug_console.hpp"
#include "pit.hpp"
#include "vga.hpp"
#include <deque>
#include <fstream>
#include <memory>
#include <unordered_map>
namespace pc {
struct Machine {
    // Runtime wiring only; bindDevices rebuilds callbacks after snapshot moves.
    IoPortSpace ioPorts;
    CPU cpu;
    VGA vga;
    ATA ata;
    I8259PairDevice pic;
    SystemPortBDevice systemPortB;
    Port92Device port92;
    DebugConsoleDevice debugConsole;
    PIT8254 pit;
    // Runtime clock wiring, not snapshot state. Ordinary machines retain the
    // deterministic instruction-derived PIT clock. Benchmark owners may inject
    // a monotonic 1,193,182-Hz clock; reset the PIT at that clock's origin before
    // boot, and do not restore snapshots from another clock domain.
    std::function<uint64_t()> pitClock;
    uint64_t pitTicks() const { return pitClock ? pitClock() : cpu.instructions / 4; }
    I8042Device ps2;
    AbsoluteMouse absoluteMouse;
    HostCursor hostCursor;
    void absolutePointer(int x, int y, int wheel, uint8_t buttons) {
        if (absoluteMouse.motion(x, y, wheel, buttons))
            ps2.accumulate_motion(1, 0, buttons); // Wake the guest driver even for a stationary click.
    }
    PCI pci;
    NE2000 nic;
    bool nicLine = false;
    uint8_t biosMouseCount = 0;
    std::array<uint8_t, 3> biosMousePacket{};
    uint8_t biosKeyPrefix = 0, biosPauseBytes = 0;
    bool ataLine = false;
    std::unique_ptr<Disk> disk, floppy;
    std::deque<uint16_t> keys;
    bool waitingKey = false, verbose = false;
    bool resetRequested = false, bootFromFloppy = false, vgaOnReset = false;
    uint64_t resets = 0;
    std::vector<uint8_t> vgaRom;
    uint64_t ticks = 0;
    unsigned videoMode = 3, columns = 80, rows = 25, cursor = 0, activePage = 0;
    uint8_t ports[65536]{};
    CmosRtc rtc;
    uint32_t ioReads = 0;
    explicit Machine(const std::string &hd, const std::string &fd = "",
                     size_t ram = 16 * 1024 * 1024);
    void boot(bool fromFloppy = false);
    void initializeFirmware();
    void initializeVga();
    void restart();
    void bindDevices();
    void bindIoPorts();
    I8042Device::IrqState ps2IrqState() const;
    void applyPs2Irqs(I8042Device::Irqs irqs);
    int64_t rtcTime() const;
    void saveSnapshot(const std::string &directory);
    void loadSnapshot(const std::string &directory);
    bool bios(uint8_t vector);
    bool service(uint8_t vector, bool guestStub = false);
    uint32_t in(uint16_t port, int width);
    void out(uint16_t port, uint32_t value, int width);
    void character(uint8_t c, uint8_t attr = 7, bool teletype = true);
    void scroll(unsigned top, unsigned left, unsigned bottom, unsigned right, int lines,
                uint8_t attr);
    void key(uint16_t key);
    void timer();
    void poll();
    void pollPs2();
    std::string text() const;
};
} // namespace pc
