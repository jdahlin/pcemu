#pragma once
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <unistd.h>
#include <vector>

namespace pc {
// Original MS-VHDX v20240423 backend. Fixed/dynamic, parentless images.
// Payload writes are direct; BAT allocation updates use the native VHDX log.
class Vhdx {
    using Bytes = std::vector<uint8_t>;
    using Guid = std::array<uint8_t, 16>;
    struct Region {
        uint64_t at, size;
    };
    struct Handle {
        int fd = -1;
        ~Handle() {
            if (fd >= 0)
                close(fd);
        }
    } handle;
    bool writable = false, identified = false, failed = false;
    Bytes currentHeader;
    uint64_t headerAt = 0, logAt = 0, logSize = 0, batAt = 0;
    uint64_t fileSize, virtualSize = 0, blockSize = 0, ratio = 0;
    std::vector<uint64_t> payloads;
    std::vector<Region> occupied;
    static uint64_t le(std::span<const uint8_t> b, size_t at, size_t width) {
        if (at > b.size() || width > b.size() - at)
            fail("Truncated structure");
        uint64_t value = 0;
        for (size_t i = 0; i < width; ++i)
            value |= uint64_t(b[at + i]) << (8 * i);
        return value;
    }
    [[noreturn]] static void fail(const char *why) {
        throw std::runtime_error(std::string("VHDX: ") + why);
    }
    static Guid guid(std::span<const uint8_t> b, size_t at) {
        if (at > b.size() || b.size() - at < 16)
            fail("Truncated GUID");
        Guid result{};
        std::copy_n(b.begin() + at, 16, result.begin());
        return result;
    }
    static void put(std::span<uint8_t> b, size_t at, uint64_t value, unsigned width) {
        if (at > b.size() || width > b.size() - at)
            fail("Truncated output structure");
        for (unsigned n = 0; n < width; ++n)
            b[at + n] = value >> (8 * n);
    }
    static uint32_t crc(std::span<const uint8_t> b) {
        uint32_t value = ~0u;
        for (auto byte : b) {
            value ^= byte;
            for (unsigned n = 0; n < 8; ++n)
                value = (value >> 1) ^ ((value & 1) ? 0x82f63b78u : 0);
        }
        return ~value;
    }
    static bool checked(Bytes b, const char *signature) {
        if (b.size() < 8 || std::memcmp(b.data(), signature, 4))
            return false;
        uint32_t stored = le(b, 4, 4);
        std::fill_n(b.begin() + 4, 4, 0);
        return stored == crc(b);
    }
    static void checksum(Bytes &b) {
        put(b, 4, 0, 4);
        put(b, 4, crc(b), 4);
    }
    static Guid freshGuid() {
        std::random_device random;
        Guid result{};
        for (auto &byte : result)
            byte = random();
        result[7] = (result[7] & 15) | 64;
        result[8] = (result[8] & 63) | 128;
        return result;
    }
    void sync() {
        if (fsync(handle.fd))
            fail("Cannot flush image");
    }
    void store(uint64_t at, std::span<const uint8_t> data) {
        bounds(at, data.size());
        while (!data.empty()) {
            auto done = pwrite(handle.fd, data.data(), data.size(), at);
            if (done < 0 && errno == EINTR)
                continue;
            if (done <= 0)
                fail("Cannot write image");
            at += done;
            data = data.subspan(done);
        }
    }
    void header(Bytes next) {
        auto sequence = le(currentHeader, 8, 8);
        if (sequence == UINT64_MAX)
            fail("Header sequence exhausted");
        put(next, 8, sequence + 1, 8);
        checksum(next);
        auto destination = headerAt == 65536 ? 131072 : 65536;
        store(destination, next);
        sync();
        currentHeader = std::move(next);
        headerAt = destination;
    }
    void identity() {
        if (identified)
            return;
        auto next = currentHeader;
        auto file = freshGuid(), data = freshGuid();
        std::copy(file.begin(), file.end(), next.begin() + 16);
        std::copy(data.begin(), data.end(), next.begin() + 32);
        header(std::move(next));
        identified = true;
    }
    bool dirty() const {
        return std::any_of(currentHeader.begin() + 48, currentHeader.begin() + 64,
                           [](auto v) { return v; });
    }
    void clearLog() {
        auto next = currentHeader;
        std::fill_n(next.begin() + 48, 16, 0);
        header(std::move(next));
    }
    void replay() {
        if (!writable)
            fail("Dirty log requires writable recovery");
        if (le(currentHeader, 64, 2) != 0 || logSize < 8192 || logSize > 64 * (1 << 20))
            fail("Unsupported dirty log");
        // This backend emits one self-contained entry at offset zero per
        // transaction, with a fresh log GUID. Reject foreign multi-entry logs.
        auto log = bytes(logAt, logSize);
        auto id = guid(currentHeader, 48);
        size_t valid = 0;
        for (size_t at = 0; at + 4096 <= log.size(); at += 4096) {
            if (std::memcmp(log.data() + at, "loge", 4) || guid(log, at + 32) != id)
                continue;
            auto length = le(log, at + 8, 4);
            if (length < 4096 || length % 4096 || length > log.size() - at)
                continue;
            Bytes candidate(log.begin() + at, log.begin() + at + length);
            if (checked(candidate, "loge")) {
                if (at != 0)
                    fail("Unsupported log sequence");
                ++valid;
            }
        }
        if (valid != 1 || le(log, 8, 4) != 8192 || le(log, 12, 4) != 0 || le(log, 16, 8) != 1 ||
            le(log, 24, 4) != 1 || le(log, 48, 8) > fileSize || le(log, 56, 8) > fileSize ||
            std::memcmp(log.data() + 64, "desc", 4) || le(log, 88, 8) != 1 ||
            std::memcmp(log.data() + 4096, "data", 4) || le(log, 4100, 4) != 0 ||
            le(log, 8188, 4) != 1)
            fail("Unsupported or invalid active log");
        auto target = le(log, 80, 8);
        if (target < (1 << 20) || target % 4096 ||
            (target < logAt + logSize && target + 4096 > logAt))
            fail("Invalid log destination");
        bounds(target, 4096);
        Bytes sector(4096);
        std::copy_n(log.begin() + 72, 8, sector.begin());
        std::copy_n(log.begin() + 4104, 4084, sector.begin() + 8);
        std::copy_n(log.begin() + 68, 4, sector.begin() + 4092);
        auto next = currentHeader;
        auto file = freshGuid();
        std::copy(file.begin(), file.end(), next.begin() + 16);
        header(std::move(next));
        store(target, sector);
        sync();
        clearLog();
    }
    void journal(uint64_t target, const Bytes &sector) {
        if (logSize < 8192 || le(currentHeader, 64, 2) != 0)
            fail("No supported VHDX log");
        auto id = freshGuid();
        Bytes record(8192);
        std::memcpy(record.data(), "loge", 4);
        put(record, 8, 8192, 4);
        put(record, 16, 1, 8);
        put(record, 24, 1, 4);
        std::copy(id.begin(), id.end(), record.begin() + 32);
        put(record, 48, fileSize, 8);
        put(record, 56, fileSize, 8);
        std::memcpy(record.data() + 64, "desc", 4);
        std::copy_n(sector.begin() + 4092, 4, record.begin() + 68);
        std::copy_n(sector.begin(), 8, record.begin() + 72);
        put(record, 80, target, 8);
        put(record, 88, 1, 8);
        std::memcpy(record.data() + 4096, "data", 4);
        std::copy_n(sector.begin() + 8, 4084, record.begin() + 4104);
        put(record, 8188, 1, 4);
        checksum(record);
        // Prepare the durable entry before publishing its GUID. No BAT update
        // is visible until the header commits that entry as the active log.
        store(logAt, record);
        sync();
        auto next = currentHeader;
        std::copy(id.begin(), id.end(), next.begin() + 48);
        header(std::move(next));
        store(target, sector);
        sync();
        clearLog();
    }
    void bounds(uint64_t at, uint64_t size) const {
        if (at > fileSize || size > fileSize - at)
            fail("File extent outside image");
    }
    void readAt(uint64_t at, std::span<uint8_t> out) {
        bounds(at, out.size());
        while (!out.empty()) {
            auto done = pread(handle.fd, out.data(), out.size(), at);
            if (done < 0 && errno == EINTR)
                continue;
            if (done <= 0)
                fail("Truncated file read");
            at += done;
            out = out.subspan(done);
        }
    }
    Bytes bytes(uint64_t at, size_t size) {
        Bytes b(size);
        readAt(at, b);
        return b;
    }
    void reserve(uint64_t at, uint64_t size) {
        bounds(at, size);
        if (!size || at % (1 << 20) || size % (1 << 20))
            fail("Unaligned file extent");
        occupied.push_back({at, size});
    }

  public:
    Vhdx(const std::string &path, uint64_t length, bool write = false)
        : writable(write), fileSize(length) {
        handle.fd = open(path.c_str(), writable ? O_RDWR : O_RDONLY);
        if (handle.fd < 0 || flock(handle.fd, (writable ? LOCK_EX : LOCK_SH) | LOCK_NB))
            fail("Image already in use or cannot be opened");
        if (length < (1 << 20) || std::memcmp(bytes(0, 8).data(), "vhdxfile", 8))
            fail("Invalid signature");
        auto a = bytes(65536, 4096), b = bytes(131072, 4096);
        bool av = checked(a, "head"), bv = checked(b, "head");
        if (!av && !bv)
            fail("No valid header CRC32C");
        if (av && bv && le(a, 8, 8) == le(b, 8, 8))
            fail("Ambiguous header sequence");
        bool second = !av || (bv && le(b, 8, 8) > le(a, 8, 8));
        currentHeader = second ? b : a;
        headerAt = second ? 131072 : 65536;
        const auto &selected = currentHeader;
        if (le(selected, 66, 2) != 1)
            fail("Unsupported format version");
        reserve(0, 1 << 20);
        logSize = le(selected, 68, 4);
        logAt = le(selected, 72, 8);
        if (logSize)
            reserve(logAt, logSize);
        if (dirty())
            replay();
        a = bytes(192 * 1024, 65536);
        b = bytes(256 * 1024, 65536);
        av = checked(a, "regi");
        bv = checked(b, "regi");
        if (!av && !bv)
            fail("No valid region table CRC32C");
        if (av && bv && a != b)
            fail("Conflicting region tables");
        auto &regions = av ? a : b;
        auto count = le(regions, 8, 4);
        if (count > 2047)
            fail("Too many regions");
        const Guid batId{0x66, 0x77, 0xc2, 0x2d, 0x23, 0xf6, 0,    0x42,
                         0x9d, 0x64, 0x11, 0x5e, 0x9b, 0xfd, 0x4a, 8};
        const Guid metaId{6,    0xa2, 0x7c, 0x8b, 0x90, 0x47, 0x9a, 0x4b,
                          0xb8, 0xfe, 0x57, 0x5f, 5,    0xf,  0x88, 0x6e};
        std::map<Guid, Region> regionMap;
        for (size_t i = 0; i < count; ++i) {
            size_t at = 16 + i * 32;
            auto id = guid(regions, at);
            Region r{le(regions, at + 16, 8), le(regions, at + 24, 4)};
            if (!regionMap.emplace(id, r).second)
                fail("Duplicate region");
            reserve(r.at, r.size);
            if (id != batId && id != metaId && (le(regions, at + 28, 4) & 1))
                fail("Unknown required region");
        }
        if (!regionMap.contains(batId) || !regionMap.contains(metaId))
            fail("Missing BAT or metadata region");
        auto meta = regionMap.at(metaId), bat = regionMap.at(batId);
        batAt = bat.at;
        if (meta.size > 16 * (1 << 20) || bat.size > 256 * (1 << 20))
            fail("Metadata exceeds reader limit");
        auto metadata = bytes(meta.at, meta.size);
        if (std::memcmp(metadata.data(), "metadata", 8))
            fail("Invalid metadata signature");
        count = le(metadata, 10, 2);
        if (count > 2047)
            fail("Too many metadata items");
        const Guid paramsId{0x37, 0x67, 0xa1, 0xca, 0x36, 0xfa, 0x43, 0x4d,
                            0xb3, 0xb6, 0x33, 0xf0, 0xaa, 0x44, 0xe7, 0x6b};
        const Guid sizeId{0x24, 0x42, 0xa5, 0x2f, 0x1b, 0xcd, 0x76, 0x48,
                          0xb2, 0x11, 0x5d, 0xbe, 0xd8, 0x3b, 0xf4, 0xb8};
        const Guid sectorId{0x1d, 0xbf, 0x41, 0x81, 0x6f, 0xa9, 9,    0x47,
                            0xba, 0x47, 0xf2, 0x33, 0xa8, 0xfa, 0xab, 0x5f};
        const Guid physicalId{0xc7, 0x48, 0xa3, 0xcd, 0x5d, 0x44, 0x71, 0x44,
                              0x9c, 0xc9, 0xe9, 0x88, 0x52, 0x51, 0xc5, 0x56};
        const Guid diskId{0xab, 0x12, 0xca, 0xbe, 0xe6, 0xb2, 0x23, 0x45,
                          0x93, 0xef, 0xc3, 9,    0xe0, 0,    0xc7, 0x46};
        std::map<Guid, Region> items;
        for (size_t i = 0; i < count; ++i) {
            size_t at = 32 + i * 32;
            auto id = guid(metadata, at);
            Region item{le(metadata, at + 16, 4), le(metadata, at + 20, 4)};
            if (item.at < 65536 || item.at > metadata.size() ||
                item.size > metadata.size() - item.at)
                fail("Invalid metadata item extent");
            if (!items.emplace(id, item).second)
                fail("Duplicate metadata item");
            if ((le(metadata, at + 24, 4) & 4) && id != paramsId && id != sizeId &&
                id != sectorId && id != physicalId && id != diskId)
                fail("Unknown required metadata");
        }
        auto field = [&](Guid id, size_t size, size_t offset, size_t width) {
            if (!items.contains(id) || items.at(id).size != size)
                fail("Missing or invalid mandatory metadata");
            return le(metadata, items.at(id).at + offset, width);
        };
        std::vector<Region> itemRanges;
        for (const auto &[id, item] : items)
            itemRanges.push_back(item);
        std::sort(itemRanges.begin(), itemRanges.end(), [](auto x, auto y) { return x.at < y.at; });
        for (size_t i = 1; i < itemRanges.size(); ++i)
            if (itemRanges[i].at < itemRanges[i - 1].at + itemRanges[i - 1].size)
                fail("Overlapping metadata items");
        (void)field(diskId, 16, 0, 8);
        blockSize = field(paramsId, 8, 0, 4);
        if (field(paramsId, 8, 4, 4) & 2)
            fail("Differencing VHDX parents are not supported");
        virtualSize = field(sizeId, 8, 0, 8);
        auto sector = field(sectorId, 4, 0, 4), physical = field(physicalId, 4, 0, 4);
        if ((sector != 512 && sector != 4096) || (physical != 512 && physical != 4096))
            fail("Invalid sector size");
        if (!virtualSize || virtualSize % sector || virtualSize > (64ull << 40))
            fail("Invalid virtual disk size");
        if (blockSize < (1 << 20) || blockSize > (256 << 20) || (blockSize & (blockSize - 1)))
            fail("Invalid block size");
        ratio = (uint64_t(1) << 23) * sector / blockSize;
        uint64_t blocks = (virtualSize + blockSize - 1) / blockSize;
        uint64_t entries = blocks + (blocks - 1) / ratio;
        if (entries > bat.size / 8)
            fail("BAT too small for virtual disk");
        auto allocation = bytes(bat.at, entries * 8);
        payloads.reserve(blocks);
        for (uint64_t i = 0; i < blocks; ++i) {
            if (i && i % ratio == 0 && (le(allocation, (i + i / ratio - 1) * 8, 8) & 7))
                fail("Sector bitmap present in parentless image");
            auto entry = le(allocation, (i + i / ratio) * 8, 8);
            auto state = entry & 7;
            uint64_t offset = entry & 0xfffffffffff00000ull;
            if (entry & 0xffff8)
                fail("Reserved BAT bits are set");
            if (state == 6) {
                reserve(offset, blockSize);
                payloads.push_back(offset);
            } else if (state <= 3)
                payloads.push_back(0); // defined/undefined holes read as zero
            else
                fail("Unsupported payload block state");
        }
        std::sort(occupied.begin(), occupied.end(), [](auto x, auto y) { return x.at < y.at; });
        for (size_t i = 1; i < occupied.size(); ++i)
            if (occupied[i].at < occupied[i - 1].at + occupied[i - 1].size)
                fail("Overlapping file extents");
    }
    Vhdx(const Vhdx &) = delete;
    Vhdx &operator=(const Vhdx &) = delete;
    void flush() {
        if (writable && !failed)
            sync();
    }
    void write(uint64_t at, std::span<const uint8_t> data) {
        if (!writable || failed)
            fail("Image is not writable");
        if (at > virtualSize || data.size() > virtualSize - at)
            fail("Write beyond virtual disk");
        try {
            identity();
            while (!data.empty()) {
                size_t within = at % blockSize,
                       count = std::min<uint64_t>(data.size(), blockSize - within);
                auto index = at / blockSize;
                auto offset = payloads.at(index);
                if (!offset && std::all_of(data.begin(), data.begin() + count,
                                           [](auto v) { return v == 0; })) {
                    at += count;
                    data = data.subspan(count);
                    continue;
                }
                if (!offset) {
                    if (fileSize > (64ull << 40) - blockSize - (1 << 20))
                        fail("Image growth exceeds limit");
                    offset = (fileSize + (1 << 20) - 1) & ~uint64_t((1 << 20) - 1);
                    auto end = offset + blockSize;
                    if (ftruncate(handle.fd, end))
                        fail("Cannot allocate payload block");
                    fileSize = end;
                    store(offset + within, data.first(count));
                    sync();
                    uint64_t entry = batAt + (index + index / ratio) * 8;
                    auto sectorAt = entry & ~uint64_t(4095);
                    auto sector = bytes(sectorAt, 4096);
                    put(sector, entry - sectorAt, offset | 6, 8);
                    journal(sectorAt, sector);
                    payloads[index] = offset;
                } else
                    store(offset + within, data.first(count));
                at += count;
                data = data.subspan(count);
            }
            sync();
        } catch (...) {
            failed = true;
            throw;
        }
    }
    uint64_t size() const {
        return virtualSize;
    }
    void read(uint64_t at, std::span<uint8_t> out) {
        if (at > virtualSize || out.size() > virtualSize - at)
            fail("Read beyond virtual disk");
        while (!out.empty()) {
            size_t within = at % blockSize,
                   count = std::min<uint64_t>(out.size(), blockSize - within);
            auto offset = payloads.at(at / blockSize);
            if (offset)
                readAt(offset + within, out.first(count));
            else
                std::fill_n(out.begin(), count, 0);
            out = out.subspan(count);
            at += count;
        }
    }
};
} // namespace pc
