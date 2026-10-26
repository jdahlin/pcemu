#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>
namespace pc {
// Two interleaved x16 Intel command-set NOR devices on a 32-bit bus.
// Firmware model uses 256-KiB bank blocks and allows 128-byte write buffers.
// Intel StrataFlash datasheet 290667-011, April 2002 (command-set reference).
// https://web.mit.edu/6.111/www/s2004/NEWKIT/datasheets/28F128J3A.pdf
// See implementation overview for geometry, status and protection limitations.
class ArmNorFlash {
  public:
    // Initialize the firmware-visible slice from persisted flash bytes.
    explicit ArmNorFlash(std::span<const uint8_t> initial);
    // Process a command or data word according to the current write sequence.
    void write_command_or_data(uint64_t offset, uint32_t value);
    // Return currently visible array/status/ID bytes for host memory mapping.
    // IMPLEMENTATION NOTE: Mapping view of 4.1 Read Array / 4.3 Read Status.
    // https://web.mit.edu/6.111/www/s2004/NEWKIT/datasheets/28F128J3A.pdf#page=19
    std::span<const uint8_t> bytes() const {
        return visible;
    }
    // Return persistent array bytes; temporary identifier/status overlays are excluded.
    // IMPLEMENTATION NOTE: Host persistence around 4.1 Read Array.
    // https://web.mit.edu/6.111/www/s2004/NEWKIT/datasheets/28F128J3A.pdf#page=19
    std::span<const uint8_t> storage() const {
        return data;
    }
    std::pair<size_t, size_t> take_dirty_range();

  private:
    friend struct ArmSnapshotAccess; // Host checkpoint fields; never serialize host bindings.
    // Two interleaved x16 devices present one four-byte bank word.
    static constexpr size_t BusWordBytes = 4;
    // One chip occupies two adjacent byte lanes within the bank word.
    static constexpr size_t ChipLaneBytes = 2;
    // Two 128-KiB chip blocks are erased together as one bank block.
    static constexpr size_t EraseBlockBytes = 256 * 1024;
    enum class Pending { None, Word, Erase, Lock, Count, Buffer, Confirm };
    Pending pending = Pending::None;
    std::vector<uint8_t> data, visible;
    std::vector<std::pair<size_t, uint32_t>> buffer;
    size_t statusLength = BusWordBytes;
    size_t statusAddress = SIZE_MAX, blockAddress = 0, remaining = 0;
    size_t dirtyStart = SIZE_MAX, dirtyEnd = 0;
    void mark_dirty_range(size_t offset, size_t length);
    void read_array();
    void read_status_register(size_t offset);
    void program_word(size_t offset, uint32_t value);
};
} // namespace pc
