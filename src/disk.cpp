#include "disk.hpp"
#include "profile.hpp"
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/mman.h>
#include <unistd.h>
#include <zstd.h>
namespace pc {
namespace {
namespace fs = std::filesystem;
constexpr char cowMagic[8] = {'P', 'C', 'E', 'M', 'U', 'C', 'O', 'W'};
uint64_t hashBlock(const uint8_t *p, size_t n) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
void put(std::ostream &out, uint64_t value, unsigned bytes) {
    for (unsigned n = 0; n < bytes; n++)
        out.put(char(value >> (n * 8)));
}
uint64_t get(std::istream &in, unsigned bytes) {
    uint64_t value = 0;
    for (unsigned n = 0; n < bytes; n++) {
        int ch = in.get();
        if (ch < 0)
            throw std::runtime_error("Truncated COW disk record");
        value |= uint64_t(uint8_t(ch)) << (n * 8);
    }
    return value;
}
struct ReadMapping {
    const uint8_t *data;
    size_t size;
    ReadMapping(const std::string &path, size_t bytes) : size(bytes) {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0)
            throw std::runtime_error("Cannot open COW index mapping");
        void *mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (mapped == MAP_FAILED)
            throw std::runtime_error("Cannot map COW disk index");
        data = static_cast<const uint8_t *>(mapped);
    }
    ~ReadMapping() {
        munmap(const_cast<uint8_t *>(data), size);
    }
    ReadMapping(const ReadMapping &) = delete;
    ReadMapping &operator=(const ReadMapping &) = delete;
};
uint64_t mappedInteger(const uint8_t *p, unsigned bytes) {
    uint64_t value = 0;
    for (unsigned n = 0; n < bytes; n++)
        value |= uint64_t(p[n]) << (n * 8);
    return value;
}
} // namespace
Disk::Disk(const std::string &name, bool fd, bool ro, unsigned depth)
    : path(fs::absolute(name).lexically_normal().string()), floppy(fd), readOnly(ro) {
    if (depth > 64)
        throw std::runtime_error("COW disk parent chain too deep");
    file.open(path, std::ios::binary | std::ios::in | (ro ? std::ios::openmode(0) : std::ios::out));
    if (!file)
        throw std::runtime_error("Cannot open disk: " + path);
    fileBytes = fs::file_size(path);
    char signature[8]{};
    file.read(signature, 8);
    file.clear();
    file.seekg(0);
    if (!std::memcmp(signature, cowMagic, 8)) {
        file.seekg(8);
        auto version = get(file, 4), blockSize = get(file, 4);
        sectors = get(file, 8);
        auto parentSize = get(file, 4);
        if (version != 1 || blockSize != 4096 || !sectors || parentSize == 0 || parentSize > 4096)
            throw std::runtime_error("Unsupported COW disk header");
        std::string parentName(parentSize, '\0');
        file.read(parentName.data(), parentSize);
        auto stored = get(file, 8);
        if (hashBlock(reinterpret_cast<const uint8_t *>(parentName.data()), parentName.size()) !=
            stored)
            throw std::runtime_error("COW parent path checksum mismatch");
        headerSize = uint64_t(file.tellg());
        auto parentPath = fs::path(parentName);
        if (parentPath.is_relative())
            parentPath = fs::path(path).parent_path() / parentPath;
        parent = std::make_unique<Disk>(parentPath.string(), fd, true, depth + 1);
        if (sectors != parent->sectors)
            throw std::runtime_error("COW parent logical size mismatch");
        heads = parent->heads;
        spt = parent->spt;
        // Only record headers are needed to rebuild the index. A read-only,
        // constructor-scoped mapping avoids a seek/read syscall for every 4 KiB
        // record without allocating or decompressing the whole disk image.
        ReadMapping mapping(path, fileBytes);
        uint64_t at = headerSize;
        while (at < fileBytes) {
            if (fileBytes - at < 24)
                throw std::runtime_error("Truncated COW disk record");
            auto record = mapping.data + at;
            auto block = mappedInteger(record, 8);
            auto size = mappedInteger(record + 8, 4);
            auto raw = mappedInteger(record + 12, 4);
            if (block >= (sectors + 7) / 8 || raw != 4096 || !size ||
                size > ZSTD_compressBound(4096) || fileBytes - at < 24 + size)
                throw std::runtime_error("Invalid COW block record");
            if (auto old = blocks.find(block); old != blocks.end())
                liveBytes -= 24 + old->second.size;
            blocks[block] = {at, uint32_t(size)};
            liveBytes += 24 + size;
            at += 24 + size;
        }
    } else {
        if (!fileBytes || fileBytes % 512)
            throw std::runtime_error("Invalid raw disk size");
        sectors = fileBytes / 512;
        if (fd) {
            heads = 2;
            spt = sectors == 2400 ? 15 : sectors == 1440 ? 9 : 18;
        } else if (fileBytes >= 512) {
            uint8_t mbr[512]{};
            file.read(reinterpret_cast<char *>(mbr), 512);
            uint32_t lba;
            std::memcpy(&lba, mbr + 454, 4);
            if (lba && lba < sectors) {
                uint8_t bpb[512]{};
                file.seekg(uint64_t(lba) * 512);
                file.read(reinterpret_cast<char *>(bpb), 512);
                unsigned h = bpb[26] | (bpb[27] << 8), s = bpb[24] | (bpb[25] << 8);
                if (h && h <= 255 && s && s <= 63) {
                    heads = h;
                    spt = s;
                }
            }
        }
    }
    file.clear();
}
void Disk::createOverlay(const std::string &destination, const std::string &parentName) {
    auto absolute = fs::absolute(parentName).lexically_normal().string();
    if (absolute.size() > 4096 || fs::exists(destination))
        throw std::runtime_error("Invalid COW destination");
    Disk base(absolute, false, true);
    std::ofstream out(destination, std::ios::binary);
    out.write(cowMagic, 8);
    put(out, 1, 4);
    put(out, 4096, 4);
    put(out, base.sectors, 8);
    put(out, absolute.size(), 4);
    out.write(absolute.data(), absolute.size());
    put(out, hashBlock(reinterpret_cast<const uint8_t *>(absolute.data()), absolute.size()), 8);
    out.close();
    if (!out)
        throw std::runtime_error("Cannot create COW disk");
}
void Disk::flush() {
    file.clear();
    file.flush();
    if (!file)
        throw std::runtime_error("Cannot flush disk");
}
void Disk::readBlock(uint64_t index, std::array<uint8_t, 4096> &data) {
    if (cachedBlock == index) {
        profile::event(profile::CowCacheHit);
        data = cachedData;
        return;
    }
    if (auto it = blocks.find(index); it != blocks.end()) {
        auto &entry = it->second;
        file.clear();
        file.seekg(entry.offset);
        auto block = get(file, 8);
        auto size = get(file, 4);
        auto raw = get(file, 4);
        auto sum = get(file, 8);
        if (block != index || size != entry.size || raw != 4096)
            throw std::runtime_error("COW record changed");
        std::vector<uint8_t> packed(size);
        file.read(reinterpret_cast<char *>(packed.data()), size);
        if (!file)
            throw std::runtime_error("Cannot read COW block");
        auto actual = ZSTD_decompress(data.data(), data.size(), packed.data(), packed.size());
        profile::event(profile::CowDecompress);
        if (ZSTD_isError(actual) || actual != 4096 || (hashBlock(data.data(), 4096) ^ index) != sum)
            throw std::runtime_error("COW block checksum mismatch");
    } else {
        data.fill(0);
        auto lba = index * 8;
        if (!parent->read(lba, unsigned(std::min(uint64_t(8), sectors - lba)), data.data()))
            throw std::runtime_error("Cannot read COW parent");
    }
    cachedBlock = index;
    cachedData = data;
}
bool Disk::read(uint64_t lba, unsigned count, uint8_t *data) {
    profile::event(profile::DiskReadSectors, count);
    if (lba >= sectors || count > sectors - lba)
        return false;
    try {
        if (!parent) {
            file.clear();
            file.seekg(lba * 512);
            file.read(reinterpret_cast<char *>(data), size_t(count) * 512);
            return bool(file);
        }
        while (count) {
            std::array<uint8_t, 4096> block;
            readBlock(lba / 8, block);
            unsigned at = lba % 8, n = std::min(count, 8 - at);
            std::memcpy(data, block.data() + at * 512, n * 512);
            data += n * 512;
            lba += n;
            count -= n;
        }
        return true;
    } catch (const std::exception &) {
        return false;
    }
}
bool Disk::write(uint64_t lba, unsigned count, const uint8_t *data) {
    profile::event(profile::DiskWriteSectors, count);
    if (readOnly || lba >= sectors || count > sectors - lba)
        return false;
    try {
        if (!parent) {
            file.clear();
            file.seekp(lba * 512);
            file.write(reinterpret_cast<const char *>(data), size_t(count) * 512);
            file.flush();
            return bool(file);
        }
        while (count) {
            auto index = lba / 8;
            std::array<uint8_t, 4096> block;
            readBlock(index, block);
            unsigned at = lba % 8, n = std::min(count, 8 - at);
            if (!std::memcmp(block.data() + at * 512, data, n * 512)) {
                data += n * 512;
                lba += n;
                count -= n;
                continue;
            }
            std::memcpy(block.data() + at * 512, data, n * 512);
            std::vector<uint8_t> packed(ZSTD_compressBound(block.size()));
            auto size = ZSTD_compress(packed.data(), packed.size(), block.data(), block.size(), 1);
            if (ZSTD_isError(size))
                throw std::runtime_error("COW compression failed");
            file.clear();
            file.seekp(fileBytes);
            put(file, index, 8);
            put(file, size, 4);
            put(file, 4096, 4);
            put(file, hashBlock(block.data(), 4096) ^ index, 8);
            file.write(reinterpret_cast<const char *>(packed.data()), size);
            file.flush();
            if (!file)
                throw std::runtime_error("Cannot write COW block");
            if (auto old = blocks.find(index); old != blocks.end())
                liveBytes -= 24 + old->second.size;
            blocks[index] = {fileBytes, uint32_t(size)};
            liveBytes += 24 + size;
            fileBytes += 24 + size;
            cachedBlock = index;
            cachedData = block;
            data += n * 512;
            lba += n;
            count -= n;
        }
        if (fileBytes > 8 * 1024 * 1024 && fileBytes - headerSize > 2 * liveBytes)
            compact();
        return true;
    } catch (const std::exception &) {
        return false;
    }
}
void Disk::compact() {
    auto temporary = path + ".compact";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    std::vector<uint8_t> header(headerSize);
    file.clear();
    file.seekg(0);
    file.read(reinterpret_cast<char *>(header.data()), header.size());
    out.write(reinterpret_cast<char *>(header.data()), header.size());
    auto updated = blocks;
    uint64_t offset = headerSize;
    for (auto &[index, entry] : updated) {
        (void)index;
        std::vector<uint8_t> record(24 + entry.size);
        file.seekg(entry.offset);
        file.read(reinterpret_cast<char *>(record.data()), record.size());
        out.write(reinterpret_cast<char *>(record.data()), record.size());
        entry.offset = offset;
        offset += record.size();
    }
    out.close();
    if (!file || !out) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw std::runtime_error("COW compaction failed");
    }
    fs::rename(temporary, path);
    file.close();
    file.open(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file)
        throw std::runtime_error("Cannot reopen compacted COW disk");
    blocks = std::move(updated);
    fileBytes = offset;
}

} // namespace pc
