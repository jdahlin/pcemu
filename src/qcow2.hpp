#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>

namespace pc {
// Read-only standard L1/L2 mapping. Refcounts are never modified.
class Qcow2 {
    std::ifstream file;
    uint64_t fileSize, virtualSize = 0, cluster = 0, l1At = 0, l1Size = 0;
    unsigned version = 0;
    static constexpr uint64_t offsetMask = 0x00fffffffffffe00ull;
    static constexpr uint64_t copied = 1ull << 63;
    [[noreturn]] static void fail(const char *why) {
        throw std::runtime_error(std::string("QCOW2: ") + why);
    }
    void bounds(uint64_t at, uint64_t length) const {
        if (at > fileSize || length > fileSize - at) fail("Truncated image or invalid offset");
    }
    void load(uint64_t at, std::span<uint8_t> out) {
        bounds(at, out.size());
        file.clear();
        file.seekg(at);
        file.read(reinterpret_cast<char *>(out.data()), out.size());
        if (!file) fail("Cannot read image");
    }
    uint64_t integer(uint64_t at, unsigned width) {
        std::array<uint8_t, 8> b{};
        load(at, {b.data(), width});
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i) value = (value << 8) | b[i];
        return value;
    }
    uint64_t alignedOffset(uint64_t entry, uint64_t flags) const {
        if (entry & ~(offsetMask | flags)) fail("Unsupported cluster descriptor");
        auto offset = entry & offsetMask;
        if (offset % cluster) fail("Misaligned cluster");
        if (offset) bounds(offset, cluster);
        return offset;
    }
  public:
    Qcow2(const std::string &path, uint64_t bytes) : file(path, std::ios::binary), fileSize(bytes) {
        if (integer(0, 4) != 0x514649fb) fail("Invalid magic");
        version = integer(4, 4);
        if (version != 2 && version != 3) fail("Unsupported version");
        bounds(0, version == 2 ? 72 : 104);
        if (integer(8, 8) || integer(16, 4)) fail("Backing files are not supported");
        auto bits = integer(20, 4);
        if (bits < 9 || bits > 21) fail("Unsupported cluster size");
        cluster = 1ull << bits;
        virtualSize = integer(24, 8);
        if (!virtualSize || virtualSize % 512 || virtualSize > (1ull << 56)) fail("Invalid virtual size");
        if (integer(32, 4)) fail("Encryption is not supported");
        if (version == 3) {
            if (integer(72, 8)) fail("Unsupported incompatible features or dirty image");
            auto length = integer(100, 4);
            if (length < 104 || length > cluster || length % 8) fail("Invalid header length");
            bounds(0, length);
            if (integer(96, 4) > 6) fail("Invalid refcount order");
        }
        l1Size = integer(36, 4);
        l1At = integer(40, 8);
        auto coverage = cluster * (cluster / 8);
        if (!l1At || l1At % cluster || l1Size > (32ull << 20) / 8 ||
            l1Size < (virtualSize + coverage - 1) / coverage) fail("Invalid L1 table");
        bounds(l1At, l1Size * 8);
    }
    uint64_t size() const { return virtualSize; }
    void read(uint64_t at, std::span<uint8_t> out) {
        if (at > virtualSize || out.size() > virtualSize - at) fail("Read beyond virtual disk");
        while (!out.empty()) {
            auto index = at / cluster;
            auto l2 = alignedOffset(integer(l1At + index / (cluster / 8) * 8, 8), copied);
            uint64_t entry = l2 ? integer(l2 + index % (cluster / 8) * 8, 8) : 0;
            if (entry & (1ull << 62)) fail("Compressed clusters are not supported");
            auto offset = alignedOffset(entry, copied | (version == 3 ? 1 : 0));
            if (!offset && (entry & copied)) fail("Invalid allocated cluster");
            size_t within = at % cluster, count = std::min<uint64_t>(out.size(), cluster - within);
            if (offset && !(entry & 1)) load(offset + within, out.first(count));
            else std::fill_n(out.begin(), count, 0);
            at += count;
            out = out.subspan(count);
        }
    }
};
} // namespace pc
