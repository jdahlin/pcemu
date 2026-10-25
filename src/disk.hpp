#pragma once
#include <array>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "vhdx.hpp"
#include "qcow2.hpp"
namespace pc {
struct Disk {
    std::string path;
    std::fstream file;
    uint64_t sectors = 0;
    unsigned heads = 16, spt = 63;
    bool floppy = false;
    using OpenProgress = std::function<void(uint64_t indexedBytes, uint64_t totalBytes)>;
    explicit Disk(const std::string &path, bool isFloppy = false, bool readOnly = false,
                  unsigned depth = 0, OpenProgress progress = {});
    ~Disk() noexcept;
    static void createOverlay(const std::string &path, const std::string &parent);
    bool read(uint64_t lba, unsigned count, uint8_t *data);
    bool write(uint64_t lba, unsigned count, const uint8_t *data);
    void flush();
    std::string backingPath() const { return parent ? parent->path : std::string{}; }
    bool isOverlay() const {
        return bool(parent);
    }

  private:
    struct Block {
        uint64_t offset = 0;
        uint32_t size = 0;
    };
    std::unique_ptr<Disk> parent;
    std::unique_ptr<Vhdx> vhdx;
    std::unique_ptr<Qcow2> qcow2;
    std::unordered_map<uint64_t, Block> blocks;
    bool readOnly = false;
    uint64_t headerSize = 0, liveBytes = 0, fileBytes = 0;
    uint64_t cachedBlock = UINT64_MAX;
    std::array<uint8_t, 4096> cachedData{};
    void readBlock(uint64_t index, std::array<uint8_t, 4096> &data);
    void compact();
    bool loadIndex();
    void saveIndex();
};
} // namespace pc
