#include "disk.hpp"
#include "profile.hpp"
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/mman.h>
#include <sys/stat.h>
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
Disk::Disk(const std::string &name, bool fd, bool ro, unsigned depth, OpenProgress progress)
    : path(fs::absolute(name).lexically_normal().string()), floppy(fd), readOnly(ro) {
    if (progress) progress(0, 0);
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
        if (!ro) throw std::runtime_error("Legacy PCEMUCOW is read-only; migrate this disk to QCOW2");
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
        parent = std::make_unique<Disk>(parentPath.string(), fd, true, depth + 1, progress);
        if (sectors != parent->sectors)
            throw std::runtime_error("COW parent logical size mismatch");
        heads = parent->heads;
        spt = parent->spt;
        // Only record headers are needed to rebuild the index. A read-only,
        // constructor-scoped mapping avoids a seek/read syscall for every 4 KiB
        // record without allocating or decompressing the whole disk image.
        if (!loadIndex()) {
            ReadMapping mapping(path, fileBytes);
            uint64_t at = headerSize, records = 0;
            while (at < fileBytes) {
                if (progress && (records++ % 4096) == 0) progress(at, fileBytes);
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
        }
        if (progress) progress(fileBytes, fileBytes);
    } else if (!std::memcmp(signature, "QFI\xfb", 4)) {
        auto big = [&](uint64_t at, unsigned n) {
            file.seekg(at); uint64_t value=0;
            while (n--) { int c=file.get(); if (c<0) throw std::runtime_error("Truncated QCOW2 header"); value=(value<<8)|uint8_t(c); }
            return value;
        };
        auto backingAt=big(8,8), backingSize=big(16,4);
        if (backingAt) {
            if (!backingSize || backingSize>1023 || backingAt>fileBytes || backingSize>fileBytes-backingAt)
                throw std::runtime_error("Invalid QCOW2 backing path");
            std::string name(backingSize,'\0'); file.seekg(backingAt); file.read(name.data(),name.size());
            fs::path backing(name);
            qcowBacking=fs::absolute(backing.is_absolute()?backing:fs::path(path).parent_path()/backing).lexically_normal().string();
        }
        if (!ro || !qcowBacking.empty()) {
            qcowWrite=std::make_unique<Qcow2Nbd>(path,ro);
            sectors=qcowWrite->size()/512;
        } else {
            qcow2=std::make_unique<Qcow2>(path,fileBytes);
            sectors=qcow2->size()/512;
        }
        if (fd) { heads=2; spt=sectors==2400?15:sectors==1440?9:18; }
    } else if (!std::memcmp(signature, "vhdxfile", 8)) {
        if (fd) throw std::runtime_error("VHDX cannot be used as a floppy");
        vhdx = std::make_unique<Vhdx>(path, fileBytes, !ro);
        sectors = vhdx->size() / 512;
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
    if ((parent || qcow2 || qcowWrite) && !fd) {
        // A compressed import can replace its zero parent's boot sectors.
        // Inheriting that parent's default CHS geometry breaks legacy boot
        // loaders even when every guest-visible byte was preserved.
        std::array<uint8_t, 512> mbr{}, bpb{};
        if (read(0, 1, mbr.data())) {
            uint32_t lba = uint32_t(mbr[454]) | (uint32_t(mbr[455]) << 8) |
                           (uint32_t(mbr[456]) << 16) | (uint32_t(mbr[457]) << 24);
            if (lba && lba < sectors && read(lba, 1, bpb.data())) {
                unsigned h = bpb[26] | (bpb[27] << 8), s = bpb[24] | (bpb[25] << 8);
                if (h && h <= 255 && s && s <= 63) {
                    heads = h;
                    spt = s;
                }
            }
        }
    }
    if (progress) progress(fileBytes,fileBytes);
}
namespace {
// A cache is usable only for exactly the file that produced it. In particular,
// same-size overwrites and atomically replaced/compacted disks invalidate it.
std::array<uint64_t, 7> indexIdentity(const std::string &path) {
    struct stat info{};
    if (stat(path.c_str(), &info))
        throw std::runtime_error("Cannot stat COW disk");
#ifdef __APPLE__
    auto modified = info.st_mtimespec, changed = info.st_ctimespec;
#else
    auto modified = info.st_mtim, changed = info.st_ctim;
#endif
    return {uint64_t(info.st_dev), uint64_t(info.st_ino), uint64_t(info.st_size),
            uint64_t(modified.tv_sec), uint64_t(modified.tv_nsec),
            uint64_t(changed.tv_sec), uint64_t(changed.tv_nsec)};
}
constexpr size_t indexHeaderBytes = 8 + 7 * 8 + 3 * 8;
}
Disk::~Disk() noexcept {
    if (qcowWrite && !readOnly) { try { flush(); } catch (...) {} }

}
bool Disk::loadIndex() {
    try {
        std::ifstream input(path + ".idx", std::ios::binary | std::ios::ate);
        if (!input) return false;
        auto length = input.tellg();
        if (length < std::streamoff(indexHeaderBytes + 8) ||
            uint64_t(length) > indexHeaderBytes + ((sectors + 7) / 8) * 20 + 8)
            return false;
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        input.seekg(0);
        input.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
        if (!input || std::memcmp(bytes.data(), "PCIDX001", 8) ||
            mappedInteger(bytes.data() + bytes.size() - 8, 8) !=
                hashBlock(bytes.data(), bytes.size() - 8)) return false;
        auto identity = indexIdentity(path);
        size_t at = 8;
        for (auto field : identity) {
            if (mappedInteger(bytes.data() + at, 8) != field) return false;
            at += 8;
        }
        if (mappedInteger(bytes.data() + at, 8) != sectors ||
            mappedInteger(bytes.data() + at + 8, 8) != headerSize) return false;
        auto count = mappedInteger(bytes.data() + at + 16, 8);
        if (count != (bytes.size() - indexHeaderBytes - 8) / 20 ||
            bytes.size() != indexHeaderBytes + count * 20 + 8) return false;
        decltype(blocks) cached;
        cached.reserve(count);
        uint64_t live = 0;
        for (at = indexHeaderBytes; at < bytes.size() - 8; at += 20) {
            auto block = mappedInteger(bytes.data() + at, 8);
            auto offset = mappedInteger(bytes.data() + at + 8, 8);
            auto size = mappedInteger(bytes.data() + at + 16, 4);
            if (block >= (sectors + 7) / 8 || offset < headerSize ||
                offset > fileBytes || fileBytes - offset < 24 + size || !size ||
                size > ZSTD_compressBound(4096) ||
                !cached.emplace(block, Block{offset, uint32_t(size)}).second)
                return false;
            live += 24 + size;
        }
        blocks = std::move(cached);
        liveBytes = live;
        return true;
    } catch (...) { return false; }
}
void Disk::createOverlay(const std::string &destination, const std::string &parentName) {
    auto absolute=fs::absolute(parentName).lexically_normal().string();
    if (fs::exists(destination)) throw std::runtime_error("QCOW2 destination already exists");
    std::ifstream input(absolute,std::ios::binary); std::array<char,8> magic{};
    input.read(magic.data(),magic.size());
    if (!std::memcmp(magic.data(),cowMagic,8))
        throw std::runtime_error("Migrate the legacy backing disk to QCOW2 before creating a new overlay");
    std::string format=!std::memcmp(magic.data(),"QFI\xfb",4)?"qcow2":
        !std::memcmp(magic.data(),"vhdxfile",8)?"vhdx":"raw";
    runStorageTool({"qemu-img","create","-q","-f","qcow2","-F",format,"-b",absolute,
                    "-o","compat=1.1,cluster_size=65536,compression_type=zstd",destination});
}

void Disk::flush() {
    if (qcowWrite) { if (!readOnly) qcowWrite->flush(); return; }
    if (vhdx) { vhdx->flush(); return; }
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
        if (qcowWrite) {
            while (count) { unsigned n=std::min(count,2048u); qcowWrite->read(lba*512,n*512,data); lba+=n; count-=n; data+=n*512; }
            return true;
        }
        if (qcow2) {
            qcow2->read(lba * 512, {data, size_t(count) * 512});
            return true;
        }
        if (vhdx) {
            vhdx->read(lba * 512, {data, size_t(count) * 512});
            return true;
        }
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
        if (qcowWrite) {
            while (count) { unsigned n=std::min(count,2048u); qcowWrite->write(lba*512,n*512,data); lba+=n; count-=n; data+=n*512; }
            return true;
        }
        if (vhdx) { vhdx->write(lba * 512, {data, size_t(count) * 512}); return true; }
        if (!parent) {
            file.clear();
            file.seekp(lba * 512);
            file.write(reinterpret_cast<const char *>(data), size_t(count) * 512);
            file.flush();
            return bool(file);
        }
        throw std::runtime_error("Legacy PCEMUCOW is read-only");
    } catch (const std::exception &) {
        return false;
    }
}

} // namespace pc
