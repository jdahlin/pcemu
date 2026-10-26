#include "disk.hpp"
#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace pc;
int main() {
    auto root = std::filesystem::temp_directory_path() /
                ("pcemu-disk-progress-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    try {
        auto base = (root / "base.raw").string(), overlay = (root / "disk.qcow2").string();
        {
            std::ofstream f(base, std::ios::binary);
            std::array<char, 8192> zeros{};
            f.write(zeros.data(), zeros.size());
        }
        Disk::createOverlay(overlay, base);
        std::array<uint8_t, 512> expected{};
        expected[0] = 42;
        {
            Disk disk(overlay);
            assert(disk.write(1, 1, expected.data()));
            disk.flush();
        }
        auto bytes = std::filesystem::file_size(overlay);
        for (unsigned i=0; i<3; ++i) {
            Disk disk(overlay,false,true);
            std::array<uint8_t,512> data{};
            assert(disk.read(1,1,data.data()) && data==expected);
        }
        unsigned callbacks = 0;
        bool cancelled = false;
        try {
            Disk disk(overlay, false, false, 0, [&](uint64_t, uint64_t) {
                // Overlay open, parent open, then actual record indexing.
                if (++callbacks == 1)
                    throw std::runtime_error("cancelled");
            });
        } catch (const std::runtime_error &e) {
            cancelled = std::string(e.what()) == "cancelled";
        }
        assert(cancelled && callbacks == 1);
        assert(std::filesystem::file_size(overlay) == bytes);
        callbacks = 0;
        uint64_t last = 0, totalBytes = 0;
        Disk disk(overlay, false, false, 0, [&](uint64_t indexed, uint64_t total) {
            ++callbacks;
            if (total) {
                assert(indexed >= last && indexed <= total);
                last = indexed;
                totalBytes = total;
            }
        });
        assert(last == bytes && totalBytes == bytes);
        std::array<uint8_t, 512> actual{};
        assert(callbacks >= 2 && disk.read(1, 1, actual.data()) && actual == expected);
        std::filesystem::remove_all(root);
        std::cout << "QCOW2 reopen and startup cancellation passed\n";
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
}
