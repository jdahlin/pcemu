#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <zstd.h>
#include <zlib.h>

namespace pc {
// Read-only standard L1/L2 mapping. Refcounts are never modified.
class Qcow2 {
    std::ifstream file;
    uint64_t fileSize, virtualSize = 0, cluster = 0, l1At = 0, l1Size = 0;
    unsigned version = 0, clusterBits = 0, compression = 0;
    uint64_t cachedEntry = 0;
    std::vector<uint8_t> cachedCluster, l1, l2;
    uint64_t cachedL2 = 0;
    static uint64_t bigEntry(const uint8_t *p) {
        uint64_t result=0;
        for (unsigned i=0;i<8;++i) result=(result<<8)|p[i];
        return result;
    }
    void compressed(uint64_t entry) {
        if (cachedEntry == entry && !cachedCluster.empty()) return;
        if (entry & copied) fail("Invalid compressed cluster flags");
        const unsigned shift = 62 - (clusterBits - 8);
        const uint64_t at = entry & ((1ull << shift) - 1);
        const uint64_t sectors = ((entry >> shift) & ((1ull << (clusterBits-8))-1)) + 1;
        const uint64_t bytes = sectors * 512 - (at & 511);
        bounds(at, bytes);
        std::vector<uint8_t> packed(bytes), output(cluster);
        load(at, packed);
        if (compression == 1) {
            ZSTD_DCtx *context = ZSTD_createDCtx();
            if (!context) fail("Cannot allocate zstd context");
            ZSTD_inBuffer input{packed.data(), packed.size(), 0};
            ZSTD_outBuffer out{output.data(), output.size(), 0};
            size_t result = ZSTD_decompressStream(context, &out, &input);
            ZSTD_freeDCtx(context);
            if (ZSTD_isError(result) || result || out.pos != cluster)
                fail("Invalid zstd compressed cluster");
        } else {
            z_stream stream{};
            stream.next_in = packed.data(); stream.avail_in = packed.size();
            stream.next_out = output.data(); stream.avail_out = output.size();
            if (inflateInit2(&stream, -12) != Z_OK) fail("Cannot initialize deflate");
            int result = inflate(&stream, Z_FINISH);
            auto size = stream.total_out;
            inflateEnd(&stream);
            if (result != Z_STREAM_END || size != cluster) fail("Invalid deflate compressed cluster");
        }
        cachedCluster = std::move(output); cachedEntry = entry;
    }
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
        clusterBits = bits;
        cluster = 1ull << bits;
        virtualSize = integer(24, 8);
        if (!virtualSize || virtualSize % 512 || virtualSize > (1ull << 56)) fail("Invalid virtual size");
        if (integer(32, 4)) fail("Encryption is not supported");
        if (version == 3) {
            auto features = integer(72, 8);
            if (features & ~8ull) fail("Unsupported incompatible features or dirty image");
            auto length = integer(100, 4);
            if (length < 104 || length > cluster || length % 8) fail("Invalid header length");
            bounds(0, length);
            if (features & 8) {
                if (length < 112 || integer(104,1) != 1) fail("Unsupported compression type");
                compression = 1;
            } else if (length >= 112 && integer(104,1)) fail("Compression feature flag missing");
            if (integer(96, 4) > 6) fail("Invalid refcount order");
        }
        l1Size = integer(36, 4);
        l1At = integer(40, 8);
        auto coverage = cluster * (cluster / 8);
        if (!l1At || l1At % cluster || l1Size > (32ull << 20) / 8 ||
            l1Size < (virtualSize + coverage - 1) / coverage) fail("Invalid L1 table");
        bounds(l1At, l1Size * 8);
        l1.resize(l1Size*8); load(l1At,l1);
    }
    uint64_t size() const { return virtualSize; }
    void read(uint64_t at, std::span<uint8_t> out) {
        if (at > virtualSize || out.size() > virtualSize - at) fail("Read beyond virtual disk");
        while (!out.empty()) {
            auto index = at / cluster;
            auto table = alignedOffset(bigEntry(l1.data()+index/(cluster/8)*8),copied);
            if (table && table!=cachedL2) {
                l2.resize(cluster); load(table,l2); cachedL2=table;
            }
            uint64_t entry = table ? bigEntry(l2.data()+index%(cluster/8)*8) : 0;
            size_t within = at % cluster, count = std::min<uint64_t>(out.size(), cluster - within);
            if (entry & (1ull << 62)) {
                compressed(entry);
                std::copy_n(cachedCluster.data()+within, count, out.data());
                at += count; out = out.subspan(count); continue;
            }
            auto offset = alignedOffset(entry, copied | (version == 3 ? 1 : 0));
            if (!offset && (entry & copied)) fail("Invalid allocated cluster");
            if (offset && !(entry & 1)) {
                if (cachedEntry!=entry || cachedCluster.empty()) {
                    cachedCluster.resize(cluster); load(offset,cachedCluster); cachedEntry=entry;
                }
                std::copy_n(cachedCluster.data()+within,count,out.data());
            }
            else std::fill_n(out.begin(), count, 0);
            at += count;
            out = out.subspan(count);
        }
    }
};
} // namespace pc
