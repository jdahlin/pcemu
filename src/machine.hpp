#pragma once
#include "ata.hpp"
#include "cpu.hpp"
#include "disk.hpp"
#include "mouse.hpp"
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
    CPU cpu;
    VGA vga;
    ATA ata;
    PIC8259 pic[2];
    PIT8254 pit;
    PS2Mouse mouse;
    PCI pci;
    NE2000 nic;
    bool nicLine = false;
    bool mouseLine = false, keyboardAux = false;
    uint8_t biosMouseCount = 0;
    std::array<uint8_t, 3> biosMousePacket{};
    uint8_t biosKeyPrefix = 0, biosPauseBytes = 0;
    bool keyLine = false, ataLine = false, cascadeLine = false;
    bool keyboardFull = false;
    uint8_t keyboardData = 0;
    uint64_t keyboardNextAt = 0;
    uint8_t keyboardCommandByte = 0x45, keyboardControllerPending = 0, keyboardParameter = 0,
            keyboardLEDs = 0, keyboardScanSet = 2, keyboardOutputPort = 3;
    bool keyboardEnabled = true, keyboardLastCommand = false;
    std::deque<uint8_t> keyboardReplies;
    bool ataInService = false;
    bool timerPending = false, timerInService = false;
    std::unique_ptr<Disk> disk, floppy;
    std::deque<uint16_t> keys;
    std::deque<uint8_t> scanCodes;
    uint8_t picBase[2]{8, 0x70}, picInit[2]{};
    bool keyboardInService = false;
    bool waitingKey = false, verbose = false;
    bool resetRequested = false, bootFromFloppy = false, vgaOnReset = false;
    uint64_t resets = 0;
    std::vector<uint8_t> vgaRom;
    uint64_t ticks = 0;
    unsigned videoMode = 3, columns = 80, rows = 25, cursor = 0, activePage = 0;
    uint8_t ports[65536]{}, cmosIndex = 0;
    uint32_t ioReads = 0;
    int64_t rtcOffset = 0;
    explicit Machine(const std::string &hd, const std::string &fd = "",
                     size_t ram = 16 * 1024 * 1024);
    void boot(bool fromFloppy = false);
    void initializeFirmware();
    void initializeVga();
    void restart();
    void bindDevices();
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
    void keyboardPoll();
    void keyboardCommand(uint8_t value);
    void keyboardWrite(uint8_t value);
    std::string text() const;
};
} // namespace pc
