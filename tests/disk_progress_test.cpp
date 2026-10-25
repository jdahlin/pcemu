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
        auto base = (root / "base.raw").string(), overlay = (root / "disk.pcd").string();
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
        assert(std::filesystem::exists(overlay + ".idx"));
        auto checkOpen = [&](unsigned expectedScans) {
            unsigned scans = 0;
            Disk cached(overlay, false, false, 0, [&](uint64_t at, uint64_t total) {
                if (total && at < total) ++scans;
            });
            std::array<uint8_t, 512> data{};
            assert(cached.read(1, 1, data.data()) && data == expected);
            assert(scans == expectedScans);
        };
        checkOpen(0); // clean reopen reads only the small persistent index
        {
            std::fstream cache(overlay + ".idx", std::ios::binary | std::ios::in | std::ios::out);
            cache.put('X');
        }
        checkOpen(1); // corrupt cache falls back to authoritative record headers
        checkOpen(0); // repaired on clean close
        std::filesystem::copy_file(overlay + ".idx", overlay + ".stale");
        {
            Disk disk(overlay);
            assert(disk.write(2, 1, expected.data()));
        }
        std::filesystem::copy_file(overlay + ".stale", overlay + ".idx",
                                  std::filesystem::copy_options::overwrite_existing);
        checkOpen(1); // stale cache must not hide an appended write
        {
            Disk disk(overlay, false, true);
            std::array<uint8_t, 512> data{};
            assert(disk.read(2, 1, data.data()) && data == expected);
        }
        // Metadata changes without any size change also invalidate the cache.
        auto modified = std::filesystem::last_write_time(overlay);
        std::filesystem::last_write_time(overlay, modified - std::chrono::seconds(1));
        checkOpen(1);
        bytes = std::filesystem::file_size(overlay);
        unsigned callbacks = 0;
        bool cancelled = false;
        try {
            Disk disk(overlay, false, false, 0, [&](uint64_t, uint64_t) {
                // Overlay open, parent open, then actual record indexing.
                if (++callbacks == 3)
                    throw std::runtime_error("cancelled");
            });
        } catch (const std::runtime_error &e) {
            cancelled = std::string(e.what()) == "cancelled";
        }
        assert(cancelled && callbacks == 3);
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
        assert(callbacks >= 3 && disk.read(1, 1, actual.data()) && actual == expected);
        std::filesystem::remove_all(root);
        std::cout << "COW cached reopen, corrupt/stale cache recovery and startup cancellation passed\n";
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
}
