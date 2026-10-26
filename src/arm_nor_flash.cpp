// The ARM firmware variable store uses an Intel-command-set NOR flash bank.
// NOR retains bytes across boots. Programming only clears bits (one to zero);
// an erase resets an entire block to all ones. Software issues command sequences
// to switch between array data, device identifiers and operation status, then
// programs one bus word or fills a write buffer before confirming the operation.
//
// Two x16 chips are interleaved on a 32-bit bus. Their 128-KiB erase blocks become
// 256-KiB bank blocks; manufacturer, device and status codes appear in both lanes.
// This model exposes a firmware-sized slice rather than a full 128-Mbit chip pair.
// The board traps writes and remaps dirty visible bytes. Persistent storage bytes
// are separate from temporary status/identifier overlays, so saving variables
// never writes those overlays into the flash file.
// IMPLEMENTATION NOTE: Operations finish immediately; status always reports ready.
// COMPATIBILITY: Decode command/count from the low x16 lane and apply it to both.
// FIXME: Implement actual block protection, CFI query, suspend/resume and error
// status before claiming the full Intel command set. The model's 128-byte buffer
// allowance is compatibility policy, not a claim about the chip's 32-byte buffer.
// Reference: 3 Volt Intel StrataFlash Memory, 290667-011, April 2002, command set.
// https://web.mit.edu/6.111/www/s2004/NEWKIT/datasheets/28F128J3A.pdf#page=17

#include "arm_nor_flash.hpp"
#include <algorithm>
#include <stdexcept>
namespace pc {
namespace {
// 3.2 Device Commands, Table 4; command bytes are repeated on both x16 lanes.
// https://web.mit.edu/6.111/www/s2004/NEWKIT/datasheets/28F128J3A.pdf#page=18
enum class Command : unsigned {
    // Restore ordinary flash array reads.
    ReadArray = 0xff,
    // Expose manufacturer, device and lock-status identifier words.
    ReadIdentifierCodes = 0x90,
    // Expose the operation status register.
    ReadStatusRegister = 0x70,
    // Clear sticky operation error flags (none are currently generated).
    ClearStatusRegister = 0x50,
    // Arm a following byte/word programming cycle.
    WordProgram = 0x40,
    // Alternate encoding of the byte/word program setup command.
    AlternateWordProgram = 0x10,
    // Arm a block erase, requiring a following confirm.
    BlockErase = 0x20,
    // Arm set-lock/clear-lock command sequencing.
    BlockLockBitSetup = 0x60,
    // Enter write-buffer availability/count/data sequencing.
    WriteToBuffer = 0xe8,
    // Confirm erase, buffer programming or clearing lock bits.
    Confirm = 0xd0,
    // Confirm setting a block lock bit (acknowledged only by this model).
    SetBlockLockBit = 0x01,
};
// Read/status/identifier fields: sections 4.2 and 4.3.
// https://web.mit.edu/6.111/www/s2004/NEWKIT/datasheets/28F128J3A.pdf#page=20
// Low x16 lane supplies command/count bits in the existing model.
constexpr uint32_t CommandLaneMask = 0xffff;
// Status SR7=1 means the write state machine has completed its operation.
constexpr uint8_t WriteStateMachineReady = 0x80;
// Erased NOR cells read as ones; programming can only clear them.
constexpr uint8_t ErasedByte = 0xff;
// Read Identifier Codes returns Intel's assigned manufacturer code.
constexpr uint8_t IntelManufacturerCode = 0x89;
// Device identifier for the 28F128J3A command-set reference.
constexpr uint8_t DeviceCode128Mbit = 0x18;
// Three 32-bit bank words expose manufacturer, device and lock status.
constexpr size_t IdentifierOverlayBytes = 12;
// COMPATIBILITY: Accept up to 32 four-byte bus words per buffer operation.
constexpr size_t MaximumBufferedBusWords = 32;
// Programming extracts each eight-bit lane from the 32-bit bus value.
constexpr unsigned ByteBits = 8;
} // namespace

// Initialize persistent bytes and the independently mapped read view.
// Intel StrataFlash, 2.5 Memory Map and 4.1 Read Array.
// https://web.mit.edu/6.111/www/s2004/NEWKIT/datasheets/28F128J3A.pdf#page=14
ArmNorFlash::ArmNorFlash(std::span<const uint8_t> initial)
    : data(initial.begin(), initial.end()), visible(data) {
    if (data.empty() || data.size() % EraseBlockBytes)
        throw std::runtime_error("NOR size must be a multiple of 256 KiB");
}
// Record the union of changed visible ranges for host remapping.
// Intel StrataFlash, 4.1 Read Array (host mapping policy).
// https://web.mit.edu/6.111/www/s2004/NEWKIT/datasheets/28F128J3A.pdf#page=19
void ArmNorFlash::mark_dirty_range(size_t offset, size_t length) {
    dirtyStart = std::min(dirtyStart, offset);
    dirtyEnd = std::max(dirtyEnd, offset + length);
}
// Remove the temporary status/ID overlay and restore underlying array bytes.
// Intel StrataFlash, 4.1 Read Array.
// https://web.mit.edu/6.111/www/s2004/NEWKIT/datasheets/28F128J3A.pdf#page=19
void ArmNorFlash::read_array() {
    if (statusAddress != SIZE_MAX) {
        std::copy_n(data.begin() + statusAddress, statusLength, visible.begin() + statusAddress);
        mark_dirty_range(statusAddress, statusLength);
        statusAddress = SIZE_MAX;
    }
}
// Expose ready status on both interleaved x16 lanes at the addressed word.
// Intel StrataFlash, 4.3 Read Status Register.
// https://web.mit.edu/6.111/www/s2004/NEWKIT/datasheets/28F128J3A.pdf#page=20
void ArmNorFlash::read_status_register(size_t offset) {
    read_array();
    statusAddress = offset;
    statusLength = BusWordBytes;
    visible[offset] = visible[offset + ChipLaneBytes] = WriteStateMachineReady;
    visible[offset + 1] = visible[offset + BusWordBytes - 1] = 0;
    mark_dirty_range(offset, BusWordBytes);
}
// Program a bus word by clearing only bits that are one in the stored array.
// Intel StrataFlash, 5.1 Byte/Word Program.
// https://web.mit.edu/6.111/www/s2004/NEWKIT/datasheets/28F128J3A.pdf#page=22
void ArmNorFlash::program_word(size_t offset, uint32_t value) {
    for (unsigned i = 0; i < BusWordBytes; ++i)
        visible[offset + i] = data[offset + i] &= uint8_t(value >> (i * ByteBits));
    mark_dirty_range(offset, BusWordBytes);
}
// Advance the active program/erase/buffer command sequence for one bus write.
// Intel StrataFlash, 3.2 Device Commands and 5.2 Write to Buffer.
// https://web.mit.edu/6.111/www/s2004/NEWKIT/datasheets/28F128J3A.pdf#page=18
void ArmNorFlash::write_command_or_data(uint64_t offset64, uint32_t value) {
    if (offset64 % BusWordBytes || offset64 > data.size() - BusWordBytes)
        throw std::runtime_error("NOR access outside array or unaligned");
    size_t offset = size_t(offset64);
    const auto command = static_cast<Command>(value & CommandLaneMask);
    switch (pending) {
    case Pending::Word:
        read_array();
        program_word(offset, value);
        pending = Pending::None;
        read_status_register(blockAddress);
        return;
    case Pending::Erase:
        if (command != Command::Confirm)
            throw std::runtime_error("Invalid NOR erase confirmation");
        read_array();
        std::fill_n(data.begin() + blockAddress, EraseBlockBytes, ErasedByte);
        std::fill_n(visible.begin() + blockAddress, EraseBlockBytes, ErasedByte);
        mark_dirty_range(blockAddress, EraseBlockBytes);
        pending = Pending::None;
        read_status_register(blockAddress);
        return;
    // 7.1/7.2 Block Lock-Bits: acknowledge sequencing without enforcing locks.
    // https://web.mit.edu/6.111/www/s2004/NEWKIT/datasheets/28F128J3A.pdf#page=26
    case Pending::Lock:
        if (command != Command::Confirm && command != Command::SetBlockLockBit)
            throw std::runtime_error("Invalid NOR lock confirmation");
        pending = Pending::None;
        read_status_register(blockAddress);
        return;
    // 5.2 Write to Buffer: count is the number of following bus words minus one.
    // https://web.mit.edu/6.111/www/s2004/NEWKIT/datasheets/28F128J3A.pdf#page=22
    case Pending::Count:
        remaining = (value & CommandLaneMask) + 1;
        if (remaining > MaximumBufferedBusWords)
            throw std::runtime_error("NOR write buffer overflow");
        buffer.clear();
        pending = Pending::Buffer;
        return;
    case Pending::Buffer:
        if (offset / EraseBlockBytes != blockAddress / EraseBlockBytes)
            throw std::runtime_error("NOR buffer crosses erase block");
        buffer.push_back({offset, value});
        if (--remaining == 0)
            pending = Pending::Confirm;
        return;
    case Pending::Confirm:
        if (command != Command::Confirm)
            throw std::runtime_error("Invalid NOR buffer confirmation");
        read_array();
        for (auto [address, word] : buffer)
            program_word(address, word);
        pending = Pending::None;
        read_status_register(blockAddress);
        return;
    case Pending::None:
        break;
    }
    switch (command) {
    case Command::ReadArray:
        read_array();
        return;
    case Command::ReadIdentifierCodes:
        read_array();
        if (offset + IdentifierOverlayBytes > data.size())
            throw std::runtime_error("NOR ID beyond bank");
        statusAddress = offset;
        statusLength = IdentifierOverlayBytes;
        for (unsigned i = 0; i < IdentifierOverlayBytes; ++i)
            visible[offset + i] = 0;
        visible[offset] = visible[offset + ChipLaneBytes] = IntelManufacturerCode;
        visible[offset + BusWordBytes] = visible[offset + BusWordBytes + ChipLaneBytes] =
            DeviceCode128Mbit;
        mark_dirty_range(offset, IdentifierOverlayBytes);
        return;
    case Command::ReadStatusRegister:
        read_status_register(offset);
        return;
    case Command::ClearStatusRegister:
        read_status_register(offset);
        return;
    case Command::WordProgram:
    case Command::AlternateWordProgram:
        read_array();
        blockAddress = offset;
        pending = Pending::Word;
        return;
    // 6.1 Block Erase: discard the low block-address bits before confirmation.
    // https://web.mit.edu/6.111/www/s2004/NEWKIT/datasheets/28F128J3A.pdf#page=24
    case Command::BlockErase:
        read_array();
        blockAddress = offset & ~size_t(EraseBlockBytes - 1);
        pending = Pending::Erase;
        return;
    case Command::BlockLockBitSetup:
        read_array();
        blockAddress = offset;
        pending = Pending::Lock;
        return;
    case Command::WriteToBuffer:
        blockAddress = offset;
        read_status_register(offset);
        pending = Pending::Count;
        return;
    default:
        throw std::runtime_error("Unsupported NOR command: " + std::to_string(unsigned(command)));
    }
}
// Return and clear the host remapping interval accumulated since the last call.
// Intel StrataFlash, 4.1 Read Array (host mapping policy).
// https://web.mit.edu/6.111/www/s2004/NEWKIT/datasheets/28F128J3A.pdf#page=19
std::pair<size_t, size_t> ArmNorFlash::take_dirty_range() {
    auto result = std::pair{dirtyStart, dirtyEnd};
    dirtyStart = SIZE_MAX;
    dirtyEnd = 0;
    return result;
}
} // namespace pc
