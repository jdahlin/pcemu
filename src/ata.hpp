#pragma once
#include <array>
#include <cstdint>
namespace pc {
struct Disk;
// Primary-channel ATA PIO task file. Commands follow the ATA-4 PIO protocols;
// the data register transfers words, not consecutive I/O-port addresses.
class ATA {
  public:
    Disk *disk = nullptr;
    bool irq = false;
    uint8_t control = 0;
    explicit ATA(Disk *d = nullptr) : disk(d) {}
    uint32_t in(uint16_t port, int width);
    void out(uint16_t port, uint32_t value, int width);

  private:
    friend class SnapshotCodec;
    uint8_t error = 1, count = 1, sector = 1, cylLow = 0, cylHigh = 0, device = 0xa0, status = 0x50,
            features = 0;
    std::array<uint8_t, 512> buffer{};
    unsigned position = 0, remaining = 0, heads = 16, spt = 63;
    uint64_t lba = 0;
    bool writing = false, identify = false;
    void command(uint8_t command);
    void fail(uint8_t code = 4);
    bool load();
    void advance();
    void identifyData();
};
} // namespace pc
