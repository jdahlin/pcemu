// The firmware-configuration interface (fw_cfg) publishes named byte arrays to
// guest firmware. Firmware writes a selector key, then streams the selected item
// through the Data Register. A directory maps file names to keys and lengths.
// This is how the ARM machine supplies ACPI tables and the RAM framebuffer
// descriptor; machine composition owns their contents and physical placement.
//
// A direct memory access (DMA) command points to a guest-RAM descriptor containing
// control flags, length and a guest address. Transfers may select an item, read,
// write or skip bytes. Registers/descriptors are big-endian; feature and CPU-count
// item contents are little-endian; streamed bytes preserve their original order.
// IMPLEMENTATION NOTE: Transfers complete synchronously and only target mapped RAM.
// Bad transfer payloads set the descriptor error bit; an inaccessible descriptor
// itself raises a platform error. Successful DMA writes cannot resize an item.
// Reserved register reads return zero and traditional data writes are ignored.
// Reference: QEMU Firmware Configuration (fw_cfg) Device documentation, including
// the MMIO layout and DMA write extension documented for QEMU v2.9 and later.
// https://www.qemu.org/docs/master/specs/fw_cfg.html#guest-side-hardware-interface
// https://www.qemu.org/docs/master/specs/fw_cfg.html#guest-side-dma-interface

#include "arm_fw_cfg.hpp"
#include <algorithm>
#include <stdexcept>

namespace pc {
namespace {
// Register Locations, File Directory, Ranges and Guest-side DMA Interface.
// https://www.qemu.org/docs/master/specs/fw_cfg.html#register-locations
// https://www.qemu.org/docs/master/specs/fw_cfg.html#file-directory-key-0x0019-fw-cfg-file-dir
// https://www.qemu.org/docs/master/specs/fw_cfg.html#guest-side-dma-interface
// String-preserving Data Register at the device base.
constexpr unsigned DataRegister = 0;
// Big-endian 16-bit item selector, resetting the stream cursor.
constexpr unsigned SelectorRegister = 8;
// Big-endian 64-bit address of a DMA descriptor in guest RAM.
constexpr unsigned DmaAddressRegister = 16;
// Writing the low address word triggers a transfer after the high word.
constexpr unsigned DmaAddressLowWord = 20;
// MMIO layout ends after the eight-byte DMA Address Register.
constexpr unsigned RegisterWindowBytes = 24;
// FW_CFG_SIGNATURE selects the four ASCII bytes QEMU for discovery.
constexpr uint16_t SignatureItem = 0;
// FW_CFG_ID selects the little-endian interface feature bitmap.
constexpr uint16_t FeatureBitmapItem = 1;
// Selector keys also follow QEMU's published qemu_fw_cfg.h definitions.
// https://github.com/qemu/qemu/blob/master/include/standard-headers/linux/qemu_fw_cfg.h
// FW_CFG_NB_CPUS publishes the current CPU count, a little-endian 16-bit value.
constexpr uint16_t NumberOfCpusItem = 5;
// FW_CFG_MAX_CPUS publishes the maximum configured CPU count.
constexpr uint16_t MaximumCpusItem = 0x0f;
// FW_CFG_FILE_DIR selects the file count and packed directory records.
constexpr uint16_t FileDirectoryItem = 0x19;
// FW_CFG_FILE_FIRST: first key assigned to directory-listed items.
constexpr uint16_t FirstFileItem = 0x20;
// Generic read keys end before selector bit 14, the historical write bit.
constexpr size_t GenericSelectorLimit = 0x4000;
// Directory names are NUL-terminated in a fixed 56-byte field.
constexpr size_t FileNameBytes = 56;
// Directory begins with one big-endian 32-bit file count.
constexpr size_t DirectoryHeaderBytes = 4;
// Each directory record contains size, selector, reserved word and name.
constexpr size_t DirectoryEntryBytes = 64;
// File record size field is a big-endian 32-bit byte count.
constexpr size_t FileSizeBytes = 4;
// The 16-bit selector follows the file size in each directory record.
constexpr size_t FileSelectorOffset = 4;
// Directory selectors use big-endian 16-bit values.
constexpr size_t FileSelectorBytes = 2;
// The file name follows size, selector and two reserved bytes.
constexpr size_t FileNameOffset = 8;
// DMA descriptor is control u32, length u32 and guest address u64.
constexpr size_t DmaDescriptorBytes = 16;
// Length follows the control word in the DMA descriptor.
constexpr size_t DmaLengthOffset = 4;
// The descriptor guest address begins after the control and length words.
constexpr size_t DmaDestinationOffset = 8;
// Control bits 31:16 hold the item key when SELECT is set.
constexpr unsigned DmaSelectorShift = 16;
// ERROR reports a failed transfer; zero control means successful completion.
constexpr uint32_t DmaError = 1u << 0;
// READ copies selected item bytes into guest RAM.
constexpr uint32_t DmaRead = 1u << 1;
// SKIP advances the stream cursor without transferring data.
constexpr uint32_t DmaSkip = 1u << 2;
// SELECT changes the selected item and resets its cursor.
constexpr uint32_t DmaSelect = 1u << 3;
// WRITE copies guest RAM into an explicitly writable item.
constexpr uint32_t DmaWrite = 1u << 4;
// Wire formats pack one eight-bit byte per stream element.
constexpr unsigned ByteBits = 8;
// IMPLEMENTATION NOTE: Platform topology currently supports one through eight CPUs.
constexpr unsigned MaximumMachineCpus = 8;
// Discovery item: identifies the fw_cfg wire protocol, not reuse of a QEMU device.
constexpr uint8_t FirmwareSignature[] = {'Q', 'E', 'M', 'U'};
// Reading the DMA register returns the string QEMU CFG to advertise its presence.
constexpr uint8_t DmaSignature[] = {'Q', 'E', 'M', 'U', ' ', 'C', 'F', 'G'};
// Feature bits 0/1 advertise the traditional stream and DMA interfaces respectively.
constexpr uint8_t InterfaceFeatures[] = {(1u << 0) | (1u << 1), 0, 0, 0};

// Decode the big-endian fields in a directory or DMA descriptor.
// QEMU fw_cfg, Guest-side DMA Interface.
// https://www.qemu.org/docs/master/specs/fw_cfg.html#guest-side-dma-interface
uint64_t read_big_endian_value(std::span<const uint8_t> bytes) {
    uint64_t value = 0;
    for (auto byte : bytes)
        value = (value << ByteBits) | byte;
    return value;
}
// Encode directory fields or DMA completion control in big-endian byte order.
// QEMU fw_cfg, Guest-side DMA Interface.
// https://www.qemu.org/docs/master/specs/fw_cfg.html#guest-side-dma-interface
void write_big_endian_value(std::span<uint8_t> bytes, uint64_t value) {
    for (size_t n = bytes.size(); n; --n) {
        bytes[n - 1] = value;
        value >>= ByteBits;
    }
}
// Convert the MMIO accessor value to the big-endian register byte order.
// QEMU fw_cfg, Selector (Control) Register.
// https://www.qemu.org/docs/master/specs/fw_cfg.html#selector-control-register
uint64_t reverse_byte_order(uint64_t value, unsigned width) {
    uint64_t result = 0;
    for (unsigned i = 0; i < width; ++i) {
        result = (result << ByteBits) | (value & 255);
        value >>= ByteBits;
    }
    return result;
}
} // namespace
// Bind guest RAM and publish a CPU count for the configured machine topology.
// QEMU fw_cfg, Firmware Configuration Items.
// https://www.qemu.org/docs/master/specs/fw_cfg.html#firmware-configuration-items
ArmFwCfg::ArmFwCfg(std::span<uint8_t> ram, uint64_t ramBase, unsigned cpuCount)
    : ram(ram), base(ramBase) {
    if (!cpuCount || cpuCount > MaximumMachineCpus)
        throw std::runtime_error("Invalid fw_cfg CPU count");
    cpus[0] = uint8_t(cpuCount);
    directory.resize(DirectoryHeaderBytes);
}
// Add a uniquely named item and append its size/key/name to the directory.
// QEMU fw_cfg, File Directory (FW_CFG_FILE_DIR).
// https://www.qemu.org/docs/master/specs/fw_cfg.html#file-directory-key-0x0019-fw-cfg-file-dir
uint16_t ArmFwCfg::add_file(std::string name, std::vector<uint8_t> bytes, bool writable) {
    if (name.empty() || name.size() >= FileNameBytes || name.find('\0') != std::string::npos ||
        bytes.size() > UINT32_MAX || items.size() >= GenericSelectorLimit - FirstFileItem)
        throw std::runtime_error("Invalid fw_cfg file");
    for (const auto &item : items)
        if (item.name == name)
            throw std::runtime_error("Duplicate fw_cfg file");
    uint16_t key = FirstFileItem + items.size();
    size_t start = directory.size();
    directory.resize(start + DirectoryEntryBytes);
    auto entry = std::span(directory).subspan(start, DirectoryEntryBytes);
    write_big_endian_value(entry.first(FileSizeBytes), bytes.size());
    write_big_endian_value(entry.subspan(FileSelectorOffset, FileSelectorBytes), key);
    std::copy(name.begin(), name.end(), entry.begin() + FileNameOffset);
    items.push_back({std::move(name), std::move(bytes), writable});
    write_big_endian_value(std::span(directory).first(DirectoryHeaderBytes), items.size());
    return key;
}
// Look up a file item by selector, returning no bytes for an unknown key.
// QEMU fw_cfg, File Directory (FW_CFG_FILE_DIR).
// https://www.qemu.org/docs/master/specs/fw_cfg.html#file-directory-key-0x0019-fw-cfg-file-dir
std::span<const uint8_t> ArmFwCfg::file_data(uint16_t key) const {
    if (key < FirstFileItem || size_t(key - FirstFileItem) >= items.size())
        return {};
    return items[key - FirstFileItem].bytes;
}
// Resolve discovery, CPU, directory or file bytes for the current selector.
// QEMU fw_cfg, Firmware Configuration Items.
// https://www.qemu.org/docs/master/specs/fw_cfg.html#firmware-configuration-items
std::span<const uint8_t> ArmFwCfg::selected_item_data() const {
    if (selected == SignatureItem)
        return FirmwareSignature;
    if (selected == FeatureBitmapItem)
        return InterfaceFeatures;
    if (selected == NumberOfCpusItem || selected == MaximumCpusItem)
        return cpus; // NB_CPUS / MAX_CPUS, little endian.
    if (selected == FileDirectoryItem)
        return directory;
    return file_data(selected);
}
// Validate and map a DMA guest-physical range into the supplied RAM span.
// QEMU fw_cfg, Guest-side DMA Interface.
// https://www.qemu.org/docs/master/specs/fw_cfg.html#guest-side-dma-interface
std::span<uint8_t> ArmFwCfg::guest_memory(uint64_t address, uint64_t length) {
    if (address < base || address - base > ram.size() || length > ram.size() - (address - base))
        throw std::runtime_error("fw_cfg DMA outside guest RAM");
    return ram.subspan(address - base, length);
}
// Stream data bytes or report the DMA discovery signature.
// QEMU fw_cfg, Data Register.
// https://www.qemu.org/docs/master/specs/fw_cfg.html#data-register
uint64_t ArmFwCfg::read_register(unsigned offset, unsigned width) {
    if (width != 1 && width != 2 && width != 4 && width != 8)
        throw std::runtime_error("Invalid fw_cfg access width");
    if (offset == DataRegister) {
        auto bytes = selected_item_data();
        uint64_t result = 0;
        for (unsigned i = 0; i < width; ++i) {
            if (cursor < bytes.size())
                result |= uint64_t(bytes[cursor]) << (i * ByteBits);
            if (cursor != UINT64_MAX)
                ++cursor;
        }
        return result;
    }
    if (offset >= DmaAddressRegister && offset <= RegisterWindowBytes &&
        width <= RegisterWindowBytes - offset) {
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value |= uint64_t(DmaSignature[offset - DmaAddressRegister + i]) << (ByteBits * i);
        return value;
    }
    return 0;
}
// Select an item or assemble a DMA address, triggering on its final write.
// QEMU fw_cfg, Guest-side DMA Interface.
// https://www.qemu.org/docs/master/specs/fw_cfg.html#guest-side-dma-interface
void ArmFwCfg::write_register(unsigned offset, uint64_t value, unsigned width) {
    if (offset == SelectorRegister && width == 2) {
        selected = reverse_byte_order(value, 2);
        cursor = 0;
    } else if (offset == DmaAddressRegister && width == 4)
        dmaAddress = reverse_byte_order(value, 4) << 32;
    else if (offset == DmaAddressLowWord && width == 4) {
        dmaAddress |= reverse_byte_order(value, 4);
        perform_dma_transfer();
    } else if (offset == DmaAddressRegister && width == 8) {
        dmaAddress = reverse_byte_order(value, 8);
        perform_dma_transfer();
    }
    // Traditional data writes and writes to reserved registers are ignored.
}
// Execute one selected-item transfer and publish its completion/error control.
// QEMU fw_cfg, Guest-side DMA Interface.
// https://www.qemu.org/docs/master/specs/fw_cfg.html#guest-side-dma-interface
void ArmFwCfg::perform_dma_transfer() {
    uint64_t address = dmaAddress;
    dmaAddress = 0;
    auto descriptor = guest_memory(address, DmaDescriptorBytes);
    uint32_t control = read_big_endian_value(descriptor.first(sizeof(uint32_t)));
    uint32_t length = read_big_endian_value(descriptor.subspan(DmaLengthOffset, sizeof(uint32_t)));
    uint64_t destination =
        read_big_endian_value(descriptor.subspan(DmaDestinationOffset, sizeof(uint64_t)));
    bool error = false;
    try {
        if (control & DmaSelect) {
            selected = control >> DmaSelectorShift;
            cursor = 0;
        }
        auto bytes = selected_item_data();
        if (control & DmaRead) {
            auto target = guest_memory(destination, length);
            size_t available =
                cursor < bytes.size() ? std::min<uint64_t>(length, bytes.size() - cursor) : 0;
            std::fill(target.begin(), target.end(), 0);
            std::copy_n(bytes.begin() + std::min<uint64_t>(cursor, bytes.size()), available,
                        target.begin());
        } else if (control & DmaWrite) {
            if (selected < FirstFileItem || size_t(selected - FirstFileItem) >= items.size() ||
                !items[selected - FirstFileItem].writable || cursor > bytes.size() ||
                length > bytes.size() - cursor)
                throw std::runtime_error("Invalid fw_cfg DMA write");
            auto source = guest_memory(destination, length);
            std::copy(source.begin(), source.end(),
                      items[selected - FirstFileItem].bytes.begin() + cursor);
        }
        if (control & (DmaRead | DmaSkip | DmaWrite))
            cursor = length > UINT64_MAX - cursor ? UINT64_MAX : cursor + length;
    } catch (const std::runtime_error &) {
        error = true;
    }
    write_big_endian_value(descriptor.first(sizeof(uint32_t)), error ? DmaError : 0);
}
} // namespace pc
