#include "disk.hpp"
#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc == 2) {
        pc::Disk disk(argv[1], false, true);
        std::array<uint8_t, 512> b{};
        assert(disk.read(0, 1, b.data()) && b[0] == 42);
        assert(disk.read(7, 1, b.data()) && b[0] == 99);
        assert(disk.read(8, 1, b.data()) && b[0] == 0);
        return 0;
    }
    auto root = std::filesystem::temp_directory_path() / ("pcemu-qcow2-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    auto path = (root / "base.qcow2").string();
    for (unsigned version : {2, 3}) {
        std::vector<uint8_t> image(4096 * 4);
        auto put = [&](size_t at, uint64_t value, unsigned width) {
            for (unsigned i = 0; i < width; ++i) image[at + width - 1 - i] = value >> (8 * i);
        };
        put(0, 0x514649fb, 4); put(4, version, 4); put(20, 12, 4);
        put(24, 16384, 8); put(36, 1, 4); put(40, 4096, 8);
        if (version == 3) { put(96, 4, 4); put(100, 104, 4); }
        put(4096, 8192 | (1ull << 63), 8);
        put(8192, 12288 | (1ull << 63), 8);
        if (version == 3) put(8200, 1, 8);
        image[12288] = 42; image[12288 + 3584] = 99;
        auto save = [&] { std::ofstream f(path, std::ios::binary); f.write(reinterpret_cast<char *>(image.data()), image.size()); };
        save();
        {
            pc::Disk disk(path, false, true);
            std::array<uint8_t, 1024> b{};
            assert(disk.sectors == 32);
            assert(disk.read(7, 2, b.data()) && b[0] == 99 && b[512] == 0);
            assert(disk.read(16, 2, b.data()) && b[0] == 0);
            assert(!disk.read(32, 1, b.data()));
            assert(!disk.write(0, 1, b.data()));
        }
        bool rejected = false;
        try { pc::Disk writable(path); } catch (const std::runtime_error &) { rejected = true; }
        assert(rejected);
        for (auto mutation : {0, 1, 2, 3}) {
            auto original = image;
            if (mutation == 0) put(20, 8, 4);
            if (mutation == 1) put(8, 128, 8);
            if (mutation == 2) put(40, 4097, 8);
            if (mutation == 3) put(4, 1, 4);
            save(); rejected = false;
            try { pc::Disk bad(path, false, true); } catch (const std::runtime_error &) { rejected = true; }
            assert(rejected);
            image = original;
        }
        if (version == 3) {
            put(72, 1, 8); save();
            rejected = false;
            try { pc::Disk dirty(path, false, true); } catch (const std::runtime_error &) { rejected = true; }
            assert(rejected);
            put(72, 0, 8);
        }
        put(8192, 1ull << 62, 8); save();
        {
            pc::Disk compressed(path, false, true);
            std::array<uint8_t, 512> b{};
            assert(!compressed.read(0, 1, b.data()));
        }
    }
    auto raw = (root/"writable.raw").string();
    { std::ofstream f(raw,std::ios::binary); f.seekp(1024*1024-1); f.put(0); }
    auto overlay = (root/"write.qcow2").string();
    pc::Disk::createOverlay(overlay,raw);
    {
        pc::Disk disk(overlay);
        std::array<uint8_t,1024> data{}; data[0]=42; data[512]=99;
        assert(disk.isOverlay() && disk.backingPath()==raw);
        assert(disk.write(127,2,data.data())); disk.flush();
    }
    {
        pc::Disk disk(overlay,false,true);
        std::array<uint8_t,1024> data{};
        assert(disk.read(127,2,data.data()) && data[0]==42 && data[512]==99);
        assert(!disk.write(0,1,data.data()));
        pc::Disk original(raw,false,true);
        assert(original.read(127,2,data.data()) && data[0]==0 && data[512]==0);
    }
    pc::runStorageTool({"qemu-img","check",overlay});
    std::filesystem::remove_all(root);
    std::cout << "QCOW2 tests passed\n";
}
