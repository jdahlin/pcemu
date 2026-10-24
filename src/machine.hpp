#pragma once
#include "ata.hpp"
#include "cpu.hpp"
#include "cmos_rtc.hpp"
#include "disk.hpp"
#include "io_port_space.hpp"
#include "i8042.hpp"
#include "ne2000.hpp"
#include "pci.hpp"
#include "pic.hpp"
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
    PIC8259 pic[2];
    PIT8254 pit;
    I8042Device ps2;
    PCI pci;
    NE2000 nic;
    bool nicLine = false;
    uint8_t biosMouseCount = 0;
    std::array<uint8_t, 3> biosMousePacket{};
    uint8_t biosKeyPrefix = 0, biosPauseBytes = 0;
    bool ataLine = false, cascadeLine = false;
    bool ataInService = false;
    bool timerPending = false, timerInService = false;
    std::unique_ptr<Disk> disk, floppy;
    std::deque<uint16_t> keys;
    uint8_t picBase[2]{8, 0x70}, picInit[2]{};
    bool keyboardInService = false;
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
    void writePicPort(uint16_t port, uint32_t value);
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
