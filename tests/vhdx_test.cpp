#include "disk.hpp"
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace pc;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
static unsigned checks = 0;
static void check(bool value) {
    ++checks;
    if (!value)
        throw std::runtime_error("VHDX check " + std::to_string(checks));
}
template <class F> void rejects(F f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    check(rejected);
}
static void put(Bytes &b, size_t at, uint64_t value, unsigned n) {
    for (unsigned i = 0; i < n; ++i)
        b.at(at + i) = value >> (8 * i);
}
static uint64_t get(const Bytes &b, size_t at, unsigned n) {
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= uint64_t(b.at(at + i)) << (8 * i);
    return v;
}
static uint32_t crc(const Bytes &b) {
    uint32_t v = ~0u;
    for (auto c : b) {
        v ^= c;
        for (unsigned n = 0; n < 8; ++n)
            v = (v >> 1) ^ ((v & 1) ? 0x82f63b78 : 0);
    }
    return ~v;
}
static void sum(Bytes &b) {
    put(b, 4, 0, 4);
    put(b, 4, crc(b), 4);
}
static void signature(Bytes &b, size_t at, const char *s) {
    std::copy_n(s, std::strlen(s), b.begin() + at);
}
static void guid(Bytes &b, size_t at, const char *raw) {
    for (unsigned i = 0; i < 16; ++i)
        b.at(at + i) = std::stoul(std::string(raw + i * 2, 2), nullptr, 16);
}
static void store(const fs::path &p, uint64_t at, const Bytes &b) {
    std::fstream f(p, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(at);
    f.write((const char *)b.data(), b.size());
    if (!f)
        throw std::runtime_error("fixture write");
}
static Bytes load(const fs::path &p, uint64_t at, size_t n) {
    Bytes b(n);
    std::ifstream f(p, std::ios::binary);
    f.seekg(at);
    f.read((char *)b.data(), n);
    if (!f)
        throw std::runtime_error("fixture read");
    return b;
}
static constexpr uint64_t MB = 1024 * 1024;
static void fixture(const fs::path &p, uint32_t sector = 512, bool fixed = false) {
    auto ratio = (uint64_t(1) << 23) * sector / MB;
    {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f.seekp(6 * MB - 1);
        f.put(0);
    }
    Bytes identifier(8);
    signature(identifier, 0, "vhdxfile");
    store(p, 0, identifier);
    for (unsigned i = 0; i < 2; ++i) {
        Bytes h(4096);
        signature(h, 0, "head");
        put(h, 8, i + 1, 8);
        put(h, 66, 1, 2);
        put(h, 68, MB, 4);
        put(h, 72, MB, 8);
        sum(h);
        store(p, (i + 1) * 65536, h);
    }
    Bytes regions(65536);
    signature(regions, 0, "regi");
    put(regions, 8, 2, 4);
    guid(regions, 16, "6677c22d23f600429d64115e9bfd4a08");
    put(regions, 32, 3 * MB, 8);
    put(regions, 40, MB, 4);
    put(regions, 44, 1, 4);
    guid(regions, 48, "06a27c8b90479a4bb8fe575f050f886e");
    put(regions, 64, 2 * MB, 8);
    put(regions, 72, MB, 4);
    put(regions, 76, 1, 4);
    sum(regions);
    store(p, 192 * 1024, regions);
    store(p, 256 * 1024, regions);
    Bytes metadata(MB);
    signature(metadata, 0, "metadata");
    put(metadata, 10, 5, 2);
    const char *ids[] = {"3767a1ca36fa434db3b633f0aa44e76b", "2442a52f1bcd7648b2115dbed83bf4b8",
                         "1dbf41816fa90947ba47f233a8faab5f", "c748a3cd5d4471449cc9e9885251c556",
                         "ab12cabee6b2234593efc309e000c746"};
    unsigned sizes[] = {8, 8, 4, 4, 16}, offsets[] = {65536, 65544, 65552, 65556, 65560};
    for (unsigned i = 0; i < 5; ++i) {
        auto at = 32 + i * 32;
        guid(metadata, at, ids[i]);
        put(metadata, at + 16, offsets[i], 4);
        put(metadata, at + 20, sizes[i], 4);
        put(metadata, at + 24, i ? 6 : 4, 4);
    }
    put(metadata, 65536, MB, 4);
    put(metadata, 65540, fixed ? 1 : 0, 4);
    put(metadata, 65544, (ratio + 1) * MB, 8);
    put(metadata, 65552, sector, 4);
    put(metadata, 65556, 4096, 4);
    store(p, 2 * MB, metadata);
    Bytes bat(MB);
    put(bat, 0, 4 * MB | 6, 8);
    put(bat, (ratio + 1) * 8, 5 * MB | 6, 8);
    store(p, 3 * MB, bat);
    store(p, 4 * MB, Bytes(512, 0xa5));
    store(p, 5 * MB - 512, Bytes(512, 0x5a));
    store(p, 5 * MB, Bytes(512, 0xb6));
}
static void patch(const fs::path &p, uint64_t at, uint64_t value, unsigned n) {
    Bytes b(n);
    put(b, 0, value, n);
    store(p, at, b);
}
static void headerPatch(const fs::path &p, size_t at, uint64_t value, unsigned n) {
    auto h = load(p, 131072, 4096);
    put(h, at, value, n);
    sum(h);
    store(p, 131072, h);
}
static void dirtyFixture(const fs::path &p) {
    fixture(p);
    fs::resize_file(p, 7 * MB);
    store(p, 6 * MB, Bytes(512, 0xd7));
    auto bat = load(p, 3 * MB, 4096);
    put(bat, 8, 6 * MB | 6, 8);
    Bytes log(8192);
    signature(log, 0, "loge");
    put(log, 8, 8192, 4);
    put(log, 16, 1, 8);
    put(log, 24, 1, 4);
    guid(log, 32, "00112233445566778899aabbccddeeff");
    put(log, 48, 7 * MB, 8);
    put(log, 56, 7 * MB, 8);
    signature(log, 64, "desc");
    std::copy_n(bat.begin() + 4092, 4, log.begin() + 68);
    std::copy_n(bat.begin(), 8, log.begin() + 72);
    put(log, 80, 3 * MB, 8);
    put(log, 88, 1, 8);
    signature(log, 4096, "data");
    std::copy_n(bat.begin() + 8, 4084, log.begin() + 4104);
    put(log, 8188, 1, 4);
    sum(log);
    store(p, MB, log);
    auto h = load(p, 131072, 4096);
    guid(h, 48, "00112233445566778899aabbccddeeff");
    sum(h);
    store(p, 131072, h);
}
int main() {
    auto dir = fs::temp_directory_path() / ("pcemu-vhdx-" + std::to_string(getpid()));
    fs::create_directory(dir);
    auto p = dir / "disk.vhdx";
    try {
        check(crc(Bytes{'1', '2', '3', '4', '5', '6', '7', '8', '9'}) == 0xe3069283);
        for (auto sector : {512u, 4096u})
            for (bool fixed : {false, true}) {
                fixture(p, sector, fixed);
                auto original = load(p, 0, 65536);
                auto blocks = ((uint64_t(1) << 23) * sector / MB) + 1;
                {
                    Disk disk(p.string(), false, true);
                    check(disk.sectors == blocks * 2048);
                    Bytes b(1024);
                    check(disk.read(0, 1, b.data()) && b[0] == 0xa5);
                    check(!disk.write(0, 1, b.data()));
                    check(disk.read(2047, 2, b.data()) && b[0] == 0x5a && b[512] == 0);
                    check(disk.read((blocks - 1) * 2048, 1, b.data()) && b[0] == 0xb6);
                    check(!disk.read(disk.sectors, 1, b.data()));
                    Disk other(p.string(), false, true);
                    rejects([&] { Disk writer(p.string()); });
                }
                check(load(p, 0, 65536) == original);
                auto oldHeader = load(p, 131072, 4096);
                {
                    Disk disk(p.string());
                    Bytes b(1024, 0x77);
                    auto sizeBefore = fs::file_size(p);
                    Bytes zero(512);
                    check(disk.write(4096, 1, zero.data()) && fs::file_size(p) == sizeBefore);
                    check(disk.write(2047, 2, b.data()));
                    check(disk.read(2047, 2, b.data()) && b == Bytes(1024, 0x77));
                    rejects([&] { Disk other(p.string()); });
                    disk.flush();
                }
                check(fs::file_size(p) ==
                      7 * MB); // Dynamic allocation is one block, not the virtual size.
                {
                    Disk disk(p.string(), false, true);
                    Bytes b(1024);
                    check(disk.read(2047, 2, b.data()) && b == Bytes(1024, 0x77));
                    check(disk.read(2049, 1, b.data()) && b[0] == 0);
                }
                check(load(p, 5 * MB - 512, 512) ==
                      Bytes(512, 0x77)); // Existing payload changed in place.
                check(!fs::exists(p.string() + ".pcd"));
                auto first = load(p, 65536, 4096), second = load(p, 131072, 4096);
                auto h = get(first, 8, 8) > get(second, 8, 8) ? first : second;
                check(std::all_of(h.begin() + 48, h.begin() + 64, [](auto x) { return x == 0; }));
                check(!std::equal(h.begin() + 32, h.begin() + 48, oldHeader.begin() + 32));
                auto log = load(p, MB, 8192);
                auto checksum = get(log, 4, 4);
                put(log, 4, 0, 4);
                check(crc(log) == checksum);
                check(get(log, 8, 4) == 8192 && get(log, 24, 4) == 1);
                check(get(log, 80, 8) == 3 * MB);
                check(get(load(p, 3 * MB, 16), 8, 8) == (6 * MB | 6));
            }
        dirtyFixture(p);
        rejects([&] { Disk ro(p.string(), false, true); });
        {
            Disk disk(p.string());
            Bytes b(512);
            check(disk.read(2048, 1, b.data()) && b[0] == 0xd7);
        }
        {
            Disk disk(p.string(), false, true);
            Bytes b(512);
            check(disk.read(2048, 1, b.data()) && b[0] == 0xd7);
        }
        dirtyFixture(p);
        patch(p, MB + 100, 1, 1);
        rejects([&] { Disk disk(p.string()); });
        fixture(p);
        patch(p, 131072 + 4, 0, 4);
        {
            Disk d(p.string(), false, true);
            check(d.sectors > 0);
        }
        patch(p, 65536 + 4, 0, 4);
        rejects([&] { Disk d(p.string(), false, true); });
        fixture(p);
        patch(p, 192 * 1024 + 4, 0, 4);
        {
            Disk d(p.string(), false, true);
            check(d.sectors > 0);
        }
        patch(p, 256 * 1024 + 4, 0, 4);
        rejects([&] { Disk d(p.string(), false, true); });
        fixture(p);
        headerPatch(p, 8, 1, 8);
        rejects([&] { Disk d(p.string(), false, true); });
        fixture(p);
        headerPatch(p, 66, 2, 2);
        rejects([&] { Disk d(p.string(), false, true); });
        fixture(p);
        patch(p, 2 * MB + 65540, 2, 4);
        rejects([&] { Disk d(p.string(), false, true); });
        fixture(p);
        patch(p, 3 * MB, 2 * MB | 6, 8);
        rejects([&] { Disk d(p.string(), false, true); });
        fixture(p);
        patch(p, 3 * MB, 100 * MB | 6, 8);
        rejects([&] { Disk d(p.string(), false, true); });
        fixture(p);
        patch(p, 3 * MB, 4 * MB | 7, 8);
        rejects([&] { Disk d(p.string(), false, true); });
        fixture(p);
        patch(p, 3 * MB, 4 * MB | 14, 8);
        rejects([&] { Disk d(p.string(), false, true); });
        fixture(p);
        patch(p, 2 * MB + 48, MB - 1, 4);
        rejects([&] { Disk d(p.string(), false, true); });
        fixture(p);
        patch(p, 2 * MB + 65544, 64ull << 40, 8);
        rejects([&] { Disk d(p.string(), false, true); });
        fixture(p);
        patch(p, 2 * MB + 64 + 16, 65536, 4);
        rejects([&] { Disk d(p.string(), false, true); });
        fixture(p);
        patch(p, 2 * MB + 65552, 1024, 4);
        rejects([&] { Disk d(p.string(), false, true); });
        fixture(p);
        fs::resize_file(p, 5 * MB);
        rejects([&] { Disk d(p.string(), false, true); });
        fixture(p);
        auto cow = dir / "optional.pcd";
        Disk::createOverlay(cow.string(), p.string());
        {
            Disk d(cow.string());
            Bytes b(512, 0x33);
            check(d.write(0, 1, b.data()));
        }
        {
            Disk d(p.string(), false, true);
            Bytes b(512);
            check(d.read(0, 1, b.data()) && b[0] == 0xa5);
        }
        std::cout << checks << " VHDX checks passed\n";
        fs::remove_all(dir);
    } catch (...) {
        std::cerr << "Fixtures retained at " << dir << '\n';
        throw;
    }
}
