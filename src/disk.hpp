#pragma once
#include <array>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
namespace pc {
struct CPU;
struct Disk {
    std::string path;
    std::fstream file;
    uint64_t sectors = 0;
    unsigned heads = 16, spt = 63;
    bool floppy = false;
    explicit Disk(const std::string &path, bool isFloppy = false, bool readOnly = false,
                  unsigned depth = 0);
    static void createOverlay(const std::string &path, const std::string &parent);
    bool read(uint64_t lba, unsigned count, uint8_t *data);
    bool write(uint64_t lba, unsigned count, const uint8_t *data);
    void flush();
    std::string backingPath() const { return parent ? parent->path : std::string{}; }
    bool isOverlay() const {
        return bool(parent);
    }
    bool transfer(CPU &cpu, bool write, uint64_t lba, unsigned count, uint32_t buffer);

  private:
    struct Block {
        uint64_t offset = 0;
        uint32_t size = 0;
    };
    std::unique_ptr<Disk> parent;
    std::unordered_map<uint64_t, Block> blocks;
    bool readOnly = false;
    uint64_t headerSize = 0, liveBytes = 0, fileBytes = 0;
    uint64_t cachedBlock = UINT64_MAX;
    std::array<uint8_t, 4096> cachedData{};
    void readBlock(uint64_t index, std::array<uint8_t, 4096> &data);
    void compact();
};
} // namespace pc
