#include "jit.hpp"
#include "machine.hpp"
#include "ps2_test_support.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <zstd.h>
using namespace pc;
namespace fs = std::filesystem;
static unsigned checks = 0;
static void check(bool ok, const char *message) {
    ++checks;
    if (!ok)
        throw std::runtime_error(message);
}
static std::vector<uint8_t> unpack(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> packed((std::istreambuf_iterator<char>(file)), {});
    auto size = ZSTD_getFrameContentSize(packed.data(), packed.size());
    if (size > 128 * 1024 * 1024)
        throw std::runtime_error("Bad test snapshot frame");
    std::vector<uint8_t> data(size);
    check(ZSTD_decompress(data.data(), data.size(), packed.data(), packed.size()) == size,
          "snapshot is a valid zstd frame");
    return data;
}
static void command(ATA &ata, uint8_t op, unsigned lba, unsigned count = 2) {
    ata.write_register(0x1f6, 0xe0, 1);
    ata.write_register(0x1f2, count, 1);
    ata.write_register(0x1f3, lba, 1);
    ata.write_register(0x1f4, 0, 1);
    ata.write_register(0x1f5, 0, 1);
    ata.write_register(0x1f7, op, 1);
}
int main() {
    auto root = fs::temp_directory_path() /
                ("pcemu-snapshot-test-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    try {
        auto raw = root / "base.raw";
        {
            std::ofstream file(raw, std::ios::binary);
            file.seekp(4099ull * 512 - 1);
            file.put(0);
        }
        // Compacted imports may have a zero-filled parent: geometry must
        // follow their effective partition BPB, including nested overrides.
        auto geometry = root / "geometry.qcow2";
        Disk::createOverlay(geometry.string(), raw.string());
        std::array<uint8_t, 512> mbr{}, bpb{};
        mbr[454] = 63;
        bpb[24] = 17;
        bpb[26] = 128;
        {
            Disk image(geometry.string());
            check(image.write(0, 1, mbr.data()) && image.write(63, 1, bpb.data()),
                  "stage compacted disk partition and geometry");
        }
        {
            Disk image(geometry.string(), false, true);
            check(image.heads == 128 && image.spt == 17,
                  "compressed geometry comes from effective BPB rather than zero parent");
        }
        auto nestedGeometry = root / "nested-geometry.qcow2";
        Disk::createOverlay(nestedGeometry.string(), geometry.string());
        {
            Disk image(nestedGeometry.string());
            bpb[24] = 32;
            bpb[26] = 64;
            check(image.write(63, 1, bpb.data()), "stage child geometry override");
        }
        {
            Disk image(nestedGeometry.string(), false, true);
            check(image.heads == 64 && image.spt == 32,
                  "child BPB overrides compressed parent geometry");
            Disk floppy(nestedGeometry.string(), true, true);
            check(floppy.heads == 2 && floppy.spt == 18,
                  "floppy geometry ignores hard-disk partition BPB");
            Disk parent(raw.string(), false, true);
            check(parent.heads == 16 && parent.spt == 63,
                  "geometry detection leaves zero parent unchanged");
        }
        Disk::createOverlay((root / "a.qcow2").string(), raw.string());
        Disk::createOverlay((root / "b.qcow2").string(), raw.string());
        std::array<uint8_t, 1536> input{}, output{};
        for (unsigned n = 0; n < input.size(); n++)
            input[n] = uint8_t(n * 73 + 29);
        {
            Disk a((root / "a.qcow2").string()), b((root / "b.qcow2").string());
            check(a.isOverlay() && fs::file_size(root / "a.qcow2") < 1024*1024,
                  "empty overlay uses only a header");
            check(a.write(7, 3, input.data()) && a.read(7, 3, output.data()) && output == input,
                  "compressed overlay handles writes crossing 4 KiB blocks");
            check(b.read(7, 3, output.data()) && output == std::array<uint8_t, 1536>{},
                  "sibling overlay does not inherit another run's writes");
            check(a.write(4098, 1, input.data()) && a.read(4098, 1, output.data()) &&
                      !std::memcmp(input.data(), output.data(), 512),
                  "COW handles partial final blocks");
            check(!a.read(4098, 2, output.data()), "COW rejects out-of-range transfers");
        }
        {
            Disk reopened((root / "a.qcow2").string());
            check(reopened.read(7, 3, output.data()) && output == input,
                  "COW index rebuild survives reopen");
            auto size = fs::file_size(root / "a.qcow2");
            check(reopened.write(7, 3, input.data()) && fs::file_size(root / "a.qcow2") == size,
                  "unchanged writes allocate no new clusters");
        }
        {
            {
                Disk readonly((root / "a.qcow2").string(), false, true);
                check(!readonly.write(7, 1, input.data()), "sealed overlays reject writes");
            }
            std::array<uint8_t, 4096> random{};
            uint32_t state = 13;
            for (auto &v : random) {
                state = state * 1664525u + 1013904223u;
                v = state >> 24;
            }
            Disk a((root / "a.qcow2").string());
            for (unsigned n = 0; n < 128; n++) {
                std::memcpy(random.data(), &n, 4);
                check(a.write(24, 8, random.data()), "compressed block rewrite succeeds");
            }
            check(fs::file_size(root / "a.qcow2") < 8 * 1024 * 1024,
                  "QCOW2 rewrites reuse allocated clusters");
            std::array<uint8_t, 4096> actual{};
            check(a.read(24, 8, actual.data()) && actual == random,
                  "QCOW2 rewrites preserve the latest block");
        }
        auto floppy = root / "floppy.raw";
        {
            std::ofstream file(floppy, std::ios::binary);
            file.seekp(1474560 - 1);
            file.put(0);
        }
        Machine m(raw.string(), floppy.string(), 2 * 1024 * 1024);
        for (unsigned n = 0; n < 8; n++)
            m.cpu.r[n] = 0xabc00000u + n;
        m.cpu.ip = 0x12345;
        m.cpu.flags = 0x23246;
        m.cpu.dr[7] = 0x12345678;
        m.cpu.instructions = 12345678;
        m.cpu.interruptBlockedThrough = 12345679;
        m.cpu.s[FS] = {0x123400, 0xffff, 0x31, false, true};
        m.cpu.repeatResume = {true, 7, 0x1234, 0x20, 0x202, 0x246, 1, 2, 3, 4};
        m.cpu.tlb[17] = {0x19, m.cpu.tlbEpoch, 0x5000, 0x1a00, 6, true};
        m.cpu.write(0x5000, 0xaabbccdd, 4);
        m.pic[0].irr = 5;
        m.pic[1].isr = 0x40;
        m.pic[1].mask = 0x80;
        m.pit.counters[1].writeHigh = true;
        m.pit.counters[1].writeValue = 0x1234;
        m.pit.counters[2].pausedAt = 9182;
        m.pit.counters[0].countLatched = true;
        m.vga.planes[2][0x1234] = 0x78;
        m.vga.latch[1] = 0x39;
        m.vga.dac[9][2] = 0x27;
        m.vga.attrData = true;
        m.vga.attrIndex = 0x3f;
        m.vga.dacReadMode = true;
        m.vga.seqIndex = 0xff;
        ps2State(m).keyboardFull = true;
        ps2State(m).keyboardData = 0x1c;
        ps2State(m).keyboardNextAt = 12345690;
        ps2State(m).keyboardReplies = {0xfa, 0xaa};
        ps2State(m).scanCodes = {0x38, 0x1e, 0x9e, 0xb8};
        m.keys = {0x1e61};
        m.ports[0x1234] = 0x55;
        m.out(0x61, 3, 1);
        m.debugConsole.bind_output_sink([](uint8_t) {});
        m.out(0xe9, 'A', 1);
        m.rtc.state.index = 8;
        m.ioReads = 412;
        m.ticks = 921;
        command(m.ata, 0x20, 2);
        for (unsigned n = 0; n < 73; n++)
            m.ata.read_register(0x1f0, 2);
        m.absoluteMouse.enabled = m.absoluteMouse.absolute = true;
        m.absoluteMouse.motion(1234, 5678, 0, 1);
        m.hostCursor.active = m.hostCursor.uploading = true;
        m.hostCursor.received = 2; m.hostCursor.pending[0] = 17;
        m.hostCursor.revision = 42;
        m.saveSnapshot((root / "point").string());
        check(m.disk->isOverlay() && m.floppy->isOverlay(),
              "first checkpoint switches live disks to QCOW2 deltas");
        check(fs::file_size(root / "point/state.bin") < 100000,
              "zero RAM pages and zstd keep state compact");
        auto first = unpack(root / "point/state.bin");
        std::array<uint8_t, 18> legacyTlb = {0x19,0,0,0, 0,0,0,0,
                                           0,0x50,0,0, 0,0x1a,0,0, 6,1};
        for (unsigned n = 0; n < 4; ++n) legacyTlb[4+n] = m.cpu.tlbEpoch >> (n*8);
        check(std::search(first.begin(), first.end(), legacyTlb.begin(), legacyTlb.end()) != first.end(),
              "TLB snapshot retains its legacy 18-byte record");
        for(unsigned legacyVersion : {6u,7u}) {
            auto legacy = first;
            legacy.erase(legacy.end() - 9); // Remove v8 PE-entry state.
            if(legacyVersion==6)legacy.erase(legacy.end()-9); // Remove v7 DAC direction.
            legacy[8] = legacyVersion;
            // v6 did not preserve the Attribute source bit: locate the index
            // after the deliberately unique sequencer-index FFh marker.
            const uint8_t marker[] = {0,0x39,0,0,0xff,0,0,0x3f};
            auto position = std::search(legacy.begin(), legacy.end(), std::begin(marker), std::end(marker));
            check(position != legacy.end(), "locate VGA index fields in legacy fixture");
            if(legacyVersion==6)position[7]&=31;
            uint64_t sum = 14695981039346656037ull;
            for (auto it=legacy.begin(); it!=legacy.end()-8; ++it) sum=(sum^*it)*1099511628211ull;
            for (unsigned n=0;n<8;++n) legacy[legacy.size()-8+n]=sum>>(n*8);
            auto directory = root / ("legacy-v"+std::to_string(legacyVersion));
            fs::copy(root / "point", directory, fs::copy_options::recursive);
            std::vector<uint8_t> packed(ZSTD_compressBound(legacy.size()));
            auto size=ZSTD_compress(packed.data(),packed.size(),legacy.data(),legacy.size(),1);
            check(!ZSTD_isError(size), "encode version-6 compatibility fixture");
            {std::ofstream out(directory / "state.bin",std::ios::binary);out.write(reinterpret_cast<char*>(packed.data()),size);}
            Machine compatibility("","",2*1024*1024);
            compatibility.loadSnapshot(directory.string());
            check(compatibility.vga.attrIndex==0x3f && compatibility.vga.dacReadMode==(legacyVersion>=7) && !compatibility.cpu.protectedEntry,
                  "old snapshot preserves DAC compatibility and defaults PE-entry state");
        }
        m.cpu.r[AX] = 0;
        m.cpu.mem[0x5000] = 0;
        ps2State(m).keyboardReplies.clear();
        m.absoluteMouse.reset();
        m.loadSnapshot((root / "point").string());
        check(m.vga.attrIndex == 0x3f && m.vga.dacReadMode,
              "VGA Attribute source bit and DAC read direction survive snapshot restore");
        check(m.hostCursor.active && m.hostCursor.uploading && m.hostCursor.received == 2 &&
              m.hostCursor.pending[0] == 17 && m.hostCursor.revision == 42,
              "native cursor state and interrupted upload survive restore");
        check(m.absoluteMouse.active() && m.absoluteMouse.count == 4 &&
              m.absoluteMouse.words[1] == 1234 && m.absoluteMouse.words[2] == 5678,
              "absolute mouse negotiation and pending event survive restore");
        check(m.cpu.r[AX] == 0xabc00000 && m.cpu.mem[0x5000] == 0xdd && m.cpu.executionEpoch == 1,
              "CPU RAM and cache identity restore");
        check(m.disk->isOverlay() && fs::file_size(m.disk->path) < 1024*1024,
              "restore allocates only an empty compressed delta");
        check(m.cpu.tlb[17].base == 0x5000 && m.cpu.tlb[17].pteAddress == 0x1a00 &&
                  m.cpu.tlb[17].permissions == 6 && m.cpu.tlb[17].dirty,
              "legacy snapshot fields restore frame/permission/dirty state");
        m.saveSnapshot((root / "roundtrip").string());
        auto second = unpack(root / "roundtrip/state.bin");
        // The tail is RTC(8), DAC direction(1), PE-entry state(1), checksum(8).
        // Exclude only the advancing RTC and derived checksum, retaining both
        // Boolean latches in the exact round-trip comparison.
        check(first.size() == second.size() &&
                  std::equal(first.begin(), first.end() - 18, second.begin()) &&
                  std::equal(first.end()-10,first.end()-8,second.end()-10),
              "every serialized CPU/device/queue field round-trips exactly (RTC excluded)");
        check(m.systemPortB.read_register(false, false) == 3 && m.debugConsole.read_data_register() == 'A',
              "system/debug device latches restore from the existing port-state encoding");
        check(m.cpu.input(0x60, 1) == 0x1c && !m.ps2.state().keyboardFull,
              "restored PS2 controller delivers and consumes its latched byte");
        m.cpu.instructions += 4000;
        check(m.cpu.input(0x60, 1) == 0xfa,
              "restored PS2 replies resume at the preserved serial deadline");
        for (unsigned n = 73; n < 512; n++)
            check(m.cpu.input(0x1f0, 2) == 0, "routed pending PIO read resumes after snapshot rebinding");
        m.cpu.output(0x1f2, 7, 1);
        check(m.ata.read_register(0x1f2, 1) == 7, "restored ATA port writes bind the destination machine");
        m.cpu.output(0x70, 9, 1);
        check(m.rtc.state.index == 9, "restored I/O callbacks bind the destination machine");
        command(m.ata, 0x30, 10);
        std::array<uint8_t, 1024> expected{};
        for (unsigned n = 0; n < 37; n++) {
            uint16_t v = 0x6000 + n;
            std::memcpy(expected.data() + n * 2, &v, 2);
            m.ata.write_register(0x1f0, v, 2);
        }
        m.saveSnapshot((root / "write").string());
        m.loadSnapshot((root / "write").string());
        for (unsigned n = 37; n < 512; n++) {
            uint16_t v = 0x7000 + n;
            std::memcpy(expected.data() + n * 2, &v, 2);
            m.ata.write_register(0x1f0, v, 2);
        }
        std::array<uint8_t, 1024> actual{};
        check(m.disk->read(10, 2, actual.data()) && actual == expected,
              "pending multi-sector PIO write restores its buffered prefix and remaining sectors");
        auto changed = m.disk->path;
        m.loadSnapshot((root / "write").string());
        check(m.disk->path != changed && m.disk->read(10, 2, actual.data()) &&
                  actual == std::array<uint8_t, 1024>{},
              "repeated restore isolates a task's writes from the checkpoint");
        m.cpu.r[AX] = 0xdeadbeef;
        fs::copy(root / "point", root / "corrupt", fs::copy_options::recursive);
        {
            std::fstream file(root / "corrupt/state.bin",
                              std::ios::binary | std::ios::in | std::ios::out);
            file.seekp(16);
            file.put(0xff);
        }
        bool rejected = false;
        try {
            m.loadSnapshot((root / "corrupt").string());
        } catch (const std::exception &) {
            rejected = true;
        }
        check(rejected && m.cpu.r[AX] == 0xdeadbeef,
              "corrupt compressed state is rejected before changing the live VM");
        Machine code("", "", 2 * 1024 * 1024);
        code.cpu.s[CS].big = true;
        const uint8_t program[] = {0xb8, 1, 0, 0, 0, 0x83, 0xc0, 1, 0xeb, 0xf6};
        std::copy(std::begin(program), std::end(program), code.cpu.mem.begin());
        Jit jit(code.cpu, true, 0);
        jit.run();
        code.saveSnapshot((root / "code").string());
        code.cpu.write(1, 9, 4);
        code.cpu.ip = 0;
        jit.run();
        code.loadSnapshot((root / "code").string());
        jit.run();
        check(code.cpu.r[AX] == 2 && code.cpu.executionEpoch == 1,
              "an existing JIT discards stale code and embedded pointers after restore");
        {
            auto entry=std::make_unique<Machine>("","",2*1024*1024);
            auto &c=entry->cpu;c.segment(CS,0x1013);c.cr[0]=0x11;c.protectedEntry=true;
            const uint8_t readCr0[]={0x0f,0x20,0xc0};
            std::copy(std::begin(readCr0),std::end(readCr0),c.mem.begin()+c.s[CS].base);
            entry->saveSnapshot((root/"pe-entry").string());
            c.protectedEntry=false;entry->loadSnapshot((root/"pe-entry").string());
            check(entry->cpu.protectedEntry&&entry->cpu.cpl()==0&&entry->cpu.s[CS].selector==0x1013,
                  "snapshot v8 preserves CPL0 entry with unchanged real-mode CS");
            Jit transition(entry->cpu,true,0);
            auto before=entry->cpu.instructions;transition.run();
            check(entry->cpu.instructions==before+1&&entry->cpu.r[AX]==0x11&&transition.nativeInstructions==0,
                  "restored PE-entry state takes privilege-correct interpreter boundary");
        }
        fs::remove_all(root);
        std::cout << "Passed " << checks << " snapshot/COW checks\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "; retained fixture " << root << '\n';
        return 1;
    }
}
