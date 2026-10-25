#include "machine.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>

// Offline conversion only. Neither input may be attached to a running guest.
int main(int argc, char **argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "size") {
            pc::Disk disk(argv[2], false, true);
            std::cout << disk.sectors * 512 << '\n';
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "overlay") {
            pc::Disk::createOverlay(argv[2], argv[3]);
            return 0;
        }
        if (argc == 3 && (std::string(argv[1]) == "serve" ||
                          std::string(argv[1]) == "serve-readonly")) {
            bool readOnly = std::string(argv[1]) == "serve-readonly";
            pc::Disk disk(argv[2], false, readOnly);
            // Local pipe protocol: operation byte, LE64 LBA, LE32 sector count.
            // stdout is binary data only; a failed operation closes the pipe.
            for (;;) {
                uint8_t op;
                if (!std::cin.read(reinterpret_cast<char *>(&op), 1))
                    break;
                if (op == 'Q') {
                    disk.flush();
                    break;
                }
                if (op == 'F') {
                    disk.flush();
                    std::cout.put(0).flush();
                    continue;
                }
                uint8_t header[12];
                if (!std::cin.read(reinterpret_cast<char *>(header), 12))
                    throw std::runtime_error("Short disk request");
                uint64_t sector = 0;
                unsigned count = 0;
                for (unsigned i = 0; i < 8; ++i)
                    sector |= uint64_t(header[i]) << (i * 8);
                for (unsigned i = 0; i < 4; ++i)
                    count |= unsigned(header[8 + i]) << (i * 8);
                if ((op != 'R' && op != 'W') || !count || count > 2048 || sector >= disk.sectors ||
                    count > disk.sectors - sector)
                    throw std::runtime_error("Invalid disk request");
                std::vector<uint8_t> data(size_t(count) * 512);
                if (op == 'W' && !std::cin.read(reinterpret_cast<char *>(data.data()), data.size()))
                    throw std::runtime_error("Short disk write");
                bool success = op == 'R' ? disk.read(sector, count, data.data())
                                         : disk.write(sector, count, data.data());
                if (!success)
                    throw std::runtime_error("Disk transfer failed");
                std::cout.put(0);
                if (op == 'R')
                    std::cout.write(reinterpret_cast<const char *>(data.data()), data.size());
                std::cout.flush();
            }
            return 0;
        }
        if (argc != 5 || std::string(argv[1]) != "import-diff")
            throw std::runtime_error("Usage: disk-tool import-diff PARENT RAW DESTINATION");
        pc::Disk parent(argv[2], false, true), source(argv[3], false, true);
        if (parent.sectors != source.sectors)
            throw std::runtime_error("Disk sizes differ");
        pc::Disk::createOverlay(argv[4], argv[2]);
        pc::Disk result(argv[4]);
        std::array<uint8_t, 4096> a{}, b{};
        uint64_t changed = 0;
        for (uint64_t sector = 0; sector < source.sectors; sector += 8) {
            unsigned count = unsigned(std::min<uint64_t>(8, source.sectors - sector));
            if (!parent.read(sector, count, a.data()) || !source.read(sector, count, b.data()))
                throw std::runtime_error("Disk read failed");
            if (!std::equal(a.begin(), a.begin() + count * 512, b.begin())) {
                if (!result.write(sector, count, b.data()))
                    throw std::runtime_error("Disk write failed");
                changed += count * 512;
            }
        }
        result.flush();
        std::cout << changed << " changed bytes; " << std::filesystem::file_size(argv[4])
                  << " compressed bytes\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
