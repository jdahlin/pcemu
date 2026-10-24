#include "arm_fw_cfg.hpp"
#include <algorithm>
#include <stdexcept>

namespace pc {
namespace {
uint64_t be(std::span<const uint8_t> bytes) {
    uint64_t value = 0;
    for (auto byte : bytes) value = (value << 8) | byte;
    return value;
}
void put(std::span<uint8_t> bytes, uint64_t value) {
    for (size_t n = bytes.size(); n; --n) { bytes[n - 1] = value; value >>= 8; }
}
uint64_t swap(uint64_t value, unsigned width) {
    uint64_t result = 0;
    for (unsigned i = 0; i < width; ++i) { result = (result << 8) | (value & 255); value >>= 8; }
    return result;
}
}
ArmFwCfg::ArmFwCfg(std::span<uint8_t> ram, uint64_t ramBase) : ram(ram), base(ramBase) {
    directory.resize(4);
}
uint16_t ArmFwCfg::addFile(std::string name, std::vector<uint8_t> bytes, bool writable) {
    if (name.empty() || name.size() >= 56 || name.find('\0') != std::string::npos ||
        bytes.size() > UINT32_MAX || items.size() >= 0x3fe0)
        throw std::runtime_error("Invalid fw_cfg file");
    for (const auto &item : items)
        if (item.name == name) throw std::runtime_error("Duplicate fw_cfg file");
    uint16_t key = 0x20 + items.size();
    size_t start = directory.size();
    directory.resize(start + 64);
    auto entry = std::span(directory).subspan(start, 64);
    put(entry.first(4), bytes.size());
    put(entry.subspan(4, 2), key);
    std::copy(name.begin(), name.end(), entry.begin() + 8);
    items.push_back({std::move(name), std::move(bytes), writable});
    put(std::span(directory).first(4), items.size());
    return key;
}
std::span<const uint8_t> ArmFwCfg::file(uint16_t key) const {
    if (key < 0x20 || size_t(key - 0x20) >= items.size()) return {};
    return items[key - 0x20].bytes;
}
std::span<const uint8_t> ArmFwCfg::data() const {
    static constexpr uint8_t signature[] = {'Q', 'E', 'M', 'U'};
    static constexpr uint8_t features[] = {3, 0, 0, 0};
    if (selected == 0) return signature;
    if (selected == 1) return features;
    if (selected == 0x19) return directory;
    return file(selected);
}
std::span<uint8_t> ArmFwCfg::memory(uint64_t address, uint64_t length) {
    if (address < base || address - base > ram.size() || length > ram.size() - (address - base))
        throw std::runtime_error("fw_cfg DMA outside guest RAM");
    return ram.subspan(address - base, length);
}
uint64_t ArmFwCfg::read(unsigned offset, unsigned width) {
    if (width != 1 && width != 2 && width != 4 && width != 8)
        throw std::runtime_error("Invalid fw_cfg access width");
    if (offset == 0) {
        auto bytes = data();
        uint64_t result = 0;
        for (unsigned i = 0; i < width; ++i) {
            if (cursor < bytes.size()) result |= uint64_t(bytes[cursor]) << (i * 8);
            if (cursor != UINT64_MAX) ++cursor;
        }
        return result;
    }
    static constexpr uint8_t signature[] = {'Q', 'E', 'M', 'U', ' ', 'C', 'F', 'G'};
    if (offset >= 16 && offset <= 24 && width <= 24 - offset) {
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i) value |= uint64_t(signature[offset - 16 + i]) << (8 * i);
        return value;
    }
    return 0;
}
void ArmFwCfg::write(unsigned offset, uint64_t value, unsigned width) {
    if (offset == 8 && width == 2) { selected = swap(value, 2); cursor = 0; }
    else if (offset == 16 && width == 4) dmaAddress = swap(value, 4) << 32;
    else if (offset == 20 && width == 4) { dmaAddress |= swap(value, 4); dma(); }
    else if (offset == 16 && width == 8) { dmaAddress = swap(value, 8); dma(); }
    // Traditional data writes and writes to reserved registers are ignored.
}
void ArmFwCfg::dma() {
    uint64_t address = dmaAddress;
    dmaAddress = 0;
    auto descriptor = memory(address, 16);
    uint32_t control = be(descriptor.first(4));
    uint32_t length = be(descriptor.subspan(4, 4));
    uint64_t destination = be(descriptor.subspan(8, 8));
    bool error = false;
    try {
        if (control & 8) { selected = control >> 16; cursor = 0; }
        auto bytes = data();
        if (control & 2) {
            auto target = memory(destination, length);
            size_t available = cursor < bytes.size() ? std::min<uint64_t>(length, bytes.size() - cursor) : 0;
            std::fill(target.begin(), target.end(), 0);
            std::copy_n(bytes.begin() + std::min<uint64_t>(cursor, bytes.size()), available, target.begin());
        } else if (control & 16) {
            if (selected < 0x20 || size_t(selected - 0x20) >= items.size() ||
                !items[selected - 0x20].writable || cursor > bytes.size() || length > bytes.size() - cursor)
                throw std::runtime_error("Invalid fw_cfg DMA write");
            auto source = memory(destination, length);
            std::copy(source.begin(), source.end(), items[selected - 0x20].bytes.begin() + cursor);
        }
        if (control & (2 | 4 | 16)) cursor = length > UINT64_MAX - cursor ? UINT64_MAX : cursor + length;
    } catch (const std::runtime_error &) { error = true; }
    put(descriptor.first(4), error ? 1 : 0);
}
} // namespace pc
