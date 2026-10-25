#pragma once
#include <array>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <vector>
namespace pc {
// Two fixed USB2 HID devices. Single interrupter, 32-byte contexts, INTx.
// All guest DMA and ring walks are bounded; serviced on the VM owning thread.
class UsbXhci {
  public:
    UsbXhci(std::span<uint8_t> ram, uint64_t base, std::function<void(bool)> irq);
    uint64_t configRead(uint64_t offset, unsigned width) const;
    void configWrite(uint64_t offset, uint64_t value, unsigned width);
    bool contains(uint64_t address, unsigned width) const;
    uint64_t read(uint64_t address, unsigned width);
    void write(uint64_t address, uint64_t value, unsigned width);
    void key(uint16_t usage, bool released);
    void mouse(int dx, int dy, int wheel, uint8_t buttons);
    void releaseAll();
    uint64_t commands = 0, transfers = 0, reports = 0;
    std::string inputState() const;
    const std::string &error() const {
        return failure;
    }

  private:
    struct Ring {
        uint64_t pointer = 0;
        bool cycle = true;
    };
    struct Slot {
        bool enabled = false;
        unsigned port = 0;
        std::array<Ring, 32> endpoints{};
        std::array<bool, 32> active{};
        std::vector<uint8_t> control;
        std::array<uint32_t, 32> actual{};
        std::array<bool, 32> shortPacket{};
        bool controlIn = false;
        unsigned configuration = 0, protocol = 1, idle = 0;
    };
    std::span<uint8_t> ram;
    uint64_t ramBase;
    std::function<void(bool)> interrupt;
    uint32_t bar = 0, pciCommand = 0, usbcmd = 0, status = 1, iman = 0;
    uint32_t erstSize = 0, config = 0, imod = 0;
    uint64_t crcr = 0, dcbaa = 0, erst = 0, erdp = 0;
    uint64_t eventBase = 0, eventIndex = 0, eventCount = 0, outstanding = 0;
    bool eventCycle = true, probe = false;
    Ring commandRing;
    std::array<Slot, 9> slots{};
    std::array<uint32_t, 2> ports{};
    std::array<std::deque<std::vector<uint8_t>>, 2> pending;
    std::array<bool, 256> keys{};
    uint8_t buttons = 0;
    std::string failure;
    uint64_t keyInputs = 0, pointerInputs = 0;
    uint64_t get(uint64_t address, unsigned width) const;
    void put(uint64_t address, uint64_t value, unsigned width);
    void range(uint64_t address, uint64_t length) const;
    void reset();
    void irq();
    void event(uint64_t parameter, uint32_t status, uint32_t control);
    bool next(Ring &ring, std::array<uint32_t, 4> &trb, uint64_t &address);
    void doorbell(unsigned slot, unsigned endpoint);
    void command();
    void transfer(unsigned slot, unsigned endpoint);
    void setup(Slot &slot, uint64_t packet);
    void queue(unsigned port, std::vector<uint8_t> report);
    void context(unsigned slot, unsigned endpoint, unsigned state);
    uint32_t reg(unsigned offset) const;
    void regWrite(unsigned offset, uint32_t value);
};
} // namespace pc
