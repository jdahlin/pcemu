#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <span>
#include <utility>
#include <vector>
namespace pc {
uint64_t armMmioLoad(uint64_t value, unsigned width, bool signExtend, bool wideRegister);
// Two interleaved x16 Intel command-set NOR devices on a 32-bit bus.
// The firmware uses 256 KiB erase blocks and 128-byte write buffers.
class ArmNorFlash {
  public:
    explicit ArmNorFlash(std::span<const uint8_t> initial);
    void write(uint64_t offset, uint32_t value);
    std::span<const uint8_t> bytes() const { return visible; }
    std::span<const uint8_t> storage() const { return data; }
    std::pair<size_t, size_t> takeDirty();
  private:
    enum class Pending { None, Word, Erase, Lock, Count, Buffer, Confirm };
    Pending pending = Pending::None;
    std::vector<uint8_t> data, visible;
    std::vector<std::pair<size_t, uint32_t>> buffer;
    size_t statusLength = 4;
    size_t statusAddress = SIZE_MAX, blockAddress = 0, remaining = 0;
    size_t dirtyStart = SIZE_MAX, dirtyEnd = 0;
    void dirty(size_t offset, size_t length);
    void array();
    void status(size_t offset);
    void program(size_t offset, uint32_t value);
};
// Polled PL011 subset for firmware console. No UART interrupt/DMA backend yet.
class ArmPl011 {
  public:
    uint32_t read(unsigned offset);
    void write(unsigned offset, uint32_t value);
    void input(const std::string &text);
    const std::string &output() const { return transcript; }
  private:
    std::deque<uint8_t> receive;
    std::string transcript;
    uint32_t control = 0x300, integerDivisor = 0, fractionalDivisor = 0, lineControl = 0;
};
} // namespace pc
