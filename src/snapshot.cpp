#include "machine.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <type_traits>
#include <unistd.h>
#include <zstd.h>
#ifdef __APPLE__
#include <sys/clonefile.h>
#endif
namespace pc {
namespace {
namespace fs = std::filesystem;
constexpr uint8_t magic[8] = {'P', 'C', 'E', 'M', 'U', 'S', 'N', 'P'};
uint64_t checksum(const uint8_t *p, size_t n) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++)
        hash = (hash ^ p[i]) * 1099511628211ull;
    return hash;
}
struct Writer {
    static constexpr bool loading = false;
    std::vector<uint8_t> data;
    void bytes(const void *p, size_t n) {
        auto b = static_cast<const uint8_t *>(p);
        data.insert(data.end(), b, b + n);
    }
    template <class T>
        requires std::is_integral_v<T>
    void value(T &v) {
        auto bits = uint64_t(v);
        for (unsigned i = 0; i < sizeof(T); i++)
            data.push_back(uint8_t(bits >> (i * 8)));
    }
    template <class T, size_t N> void value(T (&v)[N]) {
        for (auto &x : v)
            value(x);
    }
    template <class T, size_t N> void value(std::array<T, N> &v) {
        for (auto &x : v)
            value(x);
    }
    template <class T> void value(std::deque<T> &v) {
        if (v.size() > 65536)
            throw std::runtime_error("Snapshot input queue too large");
        auto n = uint32_t(v.size());
        value(n);
        for (auto &x : v)
            value(x);
    }
    void value(std::vector<uint8_t> &v) {
        if (v.size() > 262144)
            throw std::runtime_error("Snapshot ROM too large");
        auto n = uint32_t(v.size());
        value(n);
        bytes(v.data(), n);
    }
    template <class... T> void operator()(T &...v) {
        (value(v), ...);
    }
};
struct Reader {
    static constexpr bool loading = true;
    const std::vector<uint8_t> &data;
    size_t position = 0, end;
    explicit Reader(const std::vector<uint8_t> &d) : data(d), end(d.size() - 8) {}
    void bytes(void *p, size_t n) {
        if (n > end - position)
            throw std::runtime_error("Truncated snapshot state");
        std::memcpy(p, data.data() + position, n);
        position += n;
    }
    template <class T>
        requires std::is_integral_v<T>
    void value(T &v) {
        if (sizeof(T) > end - position)
            throw std::runtime_error("Truncated snapshot field");
        uint64_t bits = 0;
        for (unsigned i = 0; i < sizeof(T); i++)
            bits |= uint64_t(data[position++]) << (i * 8);
        if constexpr (std::is_same_v<T, bool>) {
            if (bits > 1)
                throw std::runtime_error("Invalid snapshot boolean");
        }
        v = T(bits);
    }
    template <class T, size_t N> void value(T (&v)[N]) {
        for (auto &x : v)
            value(x);
    }
    template <class T, size_t N> void value(std::array<T, N> &v) {
        for (auto &x : v)
            value(x);
    }
    template <class T> void value(std::deque<T> &v) {
        uint32_t n;
        value(n);
        if (n > 65536)
            throw std::runtime_error("Snapshot input queue too large");
        v.resize(n);
        for (auto &x : v)
            value(x);
    }
    void value(std::vector<uint8_t> &v) {
        uint32_t n;
        value(n);
        if (n > 262144)
            throw std::runtime_error("Snapshot ROM too large");
        v.resize(n);
        bytes(v.data(), n);
    }
    template <class... T> void operator()(T &...v) {
        (value(v), ...);
    }
};
void cloneDisk(const fs::path &source, const fs::path &destination) {
#ifdef __APPLE__
    if (::clonefile(source.c_str(), destination.c_str(), 0) != 0)
#endif
        fs::copy_file(source, destination);
    fs::permissions(destination, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
}
fs::path uniqueDirectory(const fs::path &parent, const std::string &prefix) {
    fs::create_directories(parent);
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned n = 0; n < 100; n++) {
        auto path = parent / (prefix + "-" + std::to_string(getpid()) + "-" +
                              std::to_string(stamp) + "-" + std::to_string(n));
        if (fs::create_directory(path))
            return path;
    }
    throw std::runtime_error("Cannot create snapshot working directory");
}
struct DiskInfo {
    bool present = false;
    uint64_t sectors = 0;
    uint32_t heads = 0, spt = 0;
    explicit DiskInfo(const std::unique_ptr<Disk> &d) {
        if (d) {
            present = true;
            sectors = d->sectors;
            heads = d->heads;
            spt = d->spt;
        }
    }
    DiskInfo() = default;
    template <class A> void state(A &a) {
        a(present, sectors, heads, spt);
    }
    void validate(const fs::path &path) const {
        if (present &&
            (!sectors || sectors > UINT64_MAX / 512 || !heads || heads > 255 || !spt || spt > 63))
            throw std::runtime_error("Snapshot disk size/geometry mismatch");
        if (present && Disk(path.string(), false, true).sectors != sectors)
            throw std::runtime_error("Snapshot disk logical size mismatch");
    }
};
} // namespace

// An explicit, versioned little-endian format: no std::function, host pointer,
// compiler padding, JIT code or native C++ object representation is persisted.
class SnapshotCodec {
    template <class A> static void state(Machine &m, A &a, uint32_t version) {
        auto &c = m.cpu;
        a(c.r, c.ip, c.flags);
        for (auto &s : c.s)
            a(s.base, s.limit, s.selector, s.big, s.unusable);
        a(c.cr, c.dr, c.gdtBase, c.idtBase, c.ldtBase, c.gdtLimit, c.idtLimit, c.ldtLimit,
          c.ldtSelector, c.trSelector, c.trBase, c.trLimit, c.tr32, c.halted, c.a20, c.instructions,
          c.interruptBlockedThrough, c.systemAccess);
        for (auto &e : c.tlb)
            a(e.tag, e.epoch, e.base, e.pteAddress, e.permissions, e.dirty);
        a(c.tlbEpoch, c.tlbCR3);
        auto &r = c.repeatResume;
        a(r.valid, r.selector, r.base, r.ip, r.initialFlags, r.lastFlags, r.ax, r.cx, r.si, r.di);
        for (size_t at = 0; at < c.mem.size(); at += 4096) {
            auto n = std::min(size_t(4096), c.mem.size() - at);
            bool nonzero = false;
            if constexpr (!A::loading)
                nonzero = std::any_of(c.mem.begin() + at, c.mem.begin() + at + n,
                                      [](uint8_t x) { return x != 0; });
            a(nonzero);
            if (nonzero)
                a.bytes(c.mem.data() + at, n);
            else if constexpr (A::loading)
                std::fill_n(c.mem.begin() + at, n, 0);
        }
        if (version >= 5) {
            auto &mouse = m.absoluteMouse;
            a(mouse.words, mouse.count, mouse.enabled, mouse.absolute, mouse.error);
            if (mouse.count > mouse.words.size() || (mouse.absolute && !mouse.enabled))
                throw std::runtime_error("Invalid absolute mouse snapshot state");
        }
        if (version >= 6) {
            auto &cursor = m.hostCursor;
            a(cursor.shape, cursor.pending, cursor.palette, cursor.size, cursor.received,
              cursor.active, cursor.visible, cursor.uploading, cursor.revision);
            if (cursor.size > HostCursor::capacity || cursor.received > HostCursor::capacity ||
                (cursor.visible && (!cursor.active || cursor.size < 12 || HostCursor::length(cursor.shape) != cursor.size)))
                throw std::runtime_error("Invalid host cursor snapshot state");
        }
        auto &v = m.vga;
        if constexpr (A::loading) v.scanout = {};
        a(v.planes, v.seq, v.gfx, v.crtc, v.attr, v.latch, v.seqIndex, v.gfxIndex, v.crtcIndex,
          v.attrIndex, v.attrData, v.graphics, v.dac, v.dacMask, v.dacRead, v.dacWrite,
          v.dacComponent, v.width, v.height, v.misc);
        auto &d = m.ata;
        a(d.irq, d.control, d.error, d.count, d.sector, d.cylLow, d.cylHigh, d.device, d.status,
          d.features, d.buffer, d.position, d.remaining, d.heads, d.spt, d.lba, d.writing,
          d.identify);
        for (auto &p : m.pic)
            a(p.mask, p.irr, p.isr, p.base, p.init, p.lowest, p.single, p.needICW4, p.autoEOI,
              p.readISR, p.pollRead, p.specialMask, p.rotateAuto);
        for (auto &p : m.pit.counters)
            a(p.reload, p.loadedAt, p.lastEdge, p.pausedAt, p.writeValue, p.latched, p.rw, p.mode,
              p.status, p.writeHigh, p.readHigh, p.countLatched, p.latchHigh, p.statusLatched,
              p.gate, p.nullCount, p.bcd);
        a(m.ps2.state_.keyLine, m.ataLine, m.pic.state_.cascadeLine, m.ps2.state_.keyboardFull, m.ps2.state_.keyboardData, m.ps2.state_.keyboardNextAt,
          m.ps2.state_.keyboardCommandByte, m.ps2.state_.keyboardControllerPending, m.ps2.state_.keyboardParameter, m.ps2.state_.keyboardLEDs,
          m.ps2.state_.keyboardScanSet, m.ps2.state_.keyboardOutputPort, m.ps2.state_.keyboardEnabled, m.ps2.state_.keyboardLastCommand,
          m.ps2.state_.keyboardReplies, m.pic.state_.ataInService, m.pic.state_.timerPending, m.pic.state_.timerInService, m.keys, m.ps2.state_.scanCodes,
          m.pic.state_.picBase, m.pic.state_.picInit, m.pic.state_.keyboardInService, m.waitingKey, m.resetRequested,
          m.bootFromFloppy, m.vgaOnReset, m.resets, m.vgaRom, m.ticks, m.videoMode, m.columns,
          m.rows, m.cursor, m.activePage, m.ports, m.rtc.state.index, m.ioReads);
        if (version >= 2) {
            if (version >= 3) a(m.pci.address, m.pci.display_.config, m.pci.display_.barProbe, m.pci.display_.romProbe);
            a(v.dispiIndex, v.dispi);
            a.bytes(v.vram.data(), v.vram.size());
            auto &mouse = m.ps2.state_.mouse;
            a(m.ps2.state_.mouseLine, m.ps2.state_.keyboardAux, m.biosKeyPrefix, m.biosPauseBytes, m.biosMouseCount,
              m.biosMousePacket, mouse.enabled, mouse.remote, mouse.wrap, mouse.scaling2,
              mouse.resolution, mouse.rate, mouse.id, mouse.parameter, mouse.buttons,
              mouse.reportedButtons, mouse.lastSent, mouse.rates, mouse.dx, mouse.dy, mouse.wheel,
              mouse.nextAt, mouse.replies, mouse.packets);
        }
        if (version >= 4) {
            auto &n = m.nic;
            a(m.nicLine, n.memory, n.physical, n.multicast, n.tally, n.command, n.start,
              n.stop, n.boundary, n.current, n.transmitPage, n.transmitStatus, n.receiveStatus,
              n.isr, n.imr, n.rcr, n.tcr, n.dcr, n.remote, n.remoteCount, n.transmitCount,
              n.local, n.transmitted, n.received, n.dropped);
            uint32_t packets = n.outgoing.size();
            a(packets);
            if (packets > 64) throw std::runtime_error("Invalid NIC wire queue");
            if constexpr (A::loading) n.outgoing.resize(packets);
            for (auto &packet : n.outgoing) {
                a(packet);
                if (packet.size() < 14 || packet.size() > 1518)
                    throw std::runtime_error("Invalid NIC wire packet");
            }
        }
        int64_t rtc = m.rtcTime();
        a(rtc);
        if (version >= 7) {
            a(v.dacReadMode);
        } else if constexpr (A::loading) {
            // Older snapshots stored only the five-bit Attribute index and
            // had no DAC direction latch. Their display was always enabled.
            v.attrIndex |= 0x20;
            v.dacReadMode = false;
        }
        if(version>=8)a(c.protectedEntry);
        else if constexpr(A::loading)c.protectedEntry=false;
        // IMPLEMENTATION NOTE: schema 9 persists binary64 register bit patterns
        // as explicit little-endian integers, plus physical tags/TOP/environment.
        // No host floating-point object layout enters the file. Older snapshots
        // had no coprocessor state, so restore the same control/status/tag state
        // as FNINIT rather than retaining the destination machine's live stack.
        // FNINIT defaults: Intel 325383-085US, FINIT/FNINIT:
        // https://cdrdv2-public.intel.com/835757/325383-sdm-vol-2abcd.pdf#page=506
        if (version >= 9)
            a(c.fp, c.fpControl, c.fpStatus, c.fpTag, c.fpIP, c.fpData, c.fpCS, c.fpDS, c.fpOpcode);
        else if constexpr (A::loading) {
            std::fill_n(c.fp, 8, uint64_t(0));
            c.fpControl = 0x37f; c.fpStatus = 0; c.fpTag = 0xffff;
            c.fpIP = c.fpData = 0; c.fpCS = c.fpDS = c.fpOpcode = 0;
        }
        if(version>=10) {
            auto &dma=m.dma.state;
            for(auto &channel:dma.channels)
                a(channel.baseAddress,channel.baseCount,channel.address,channel.count,channel.mode,channel.page);
            a(dma.command,dma.mask,dma.softwareRequest,dma.hardwareRequest,dma.terminalCount,dma.highByte);
        } else if constexpr(A::loading) m.dma={};
        if(version>=11) {
            auto &sb=m.soundBlaster.state;auto &counts=m.soundBlaster.counters;
            a(sb.mixer,sb.response,sb.mixer_index,sb.response_head,sb.response_count,
              sb.command,sb.argument_count,sb.arguments_needed,sb.arguments,sb.time_constant,sb.test_register,
              sb.block_size,sb.remaining,sb.last_ticks,sb.phase,sb.pending_samples,sb.reset_ready_at,sb.host_phase,
              sb.left,sb.right,sb.active,sb.paused,sb.auto_initialize,sb.exit_auto,sb.high_speed,
              sb.silence,sb.speaker,sb.irq_pending,sb.reset_high,sb.reset_pending,sb.right_next,
              counts.dma_bytes,counts.blocks,counts.pcm_frames,counts.non_silent_frames,counts.dropped_frames,counts.pcm_hash,
              m.soundBlasterLine);
            if constexpr(A::loading)m.soundBlaster.output.clear();
        } else if constexpr(A::loading) {m.soundBlaster={};m.soundBlasterLine=false;}
        if(version>=12)a(m.idlePitTicks);
        else if constexpr(A::loading)m.idlePitTicks=0;
        if constexpr (A::loading) {
            if (rtc < -2208988800ll || rtc > 32503680000ll)
                throw std::runtime_error("Invalid snapshot RTC");
            m.rtc.set_time(rtc);
        }
    }
    static void validate(const Machine &m) {
        const auto &sb=m.soundBlaster.state;
        if(sb.response_head>=sb.response.size() || sb.response_count>sb.response.size() ||
           sb.argument_count>2 || sb.arguments_needed>2 ||
           (sb.arguments_needed && sb.argument_count>=sb.arguments_needed) || !sb.block_size || sb.block_size>65536 ||
           sb.remaining>65536 || (sb.active&&!sb.remaining) ||
           sb.phase>=SoundBlasterDevice::PitFrequencyHz*(256-unsigned(sb.time_constant)) || sb.host_phase>=1000000)
            throw std::runtime_error("Invalid snapshot Sound Blaster state");
        const auto &dma=m.dma.state;
        if((dma.mask|dma.softwareRequest|dma.hardwareRequest|dma.terminalCount)&~15)
            throw std::runtime_error("Invalid snapshot DMA state");
        for(const auto &channel:dma.channels)
            if(channel.mode&3)throw std::runtime_error("Invalid snapshot DMA mode");

        if (m.cpu.mem.size() < 2 * 1024 * 1024 || m.cpu.mem.size() > 64 * 1024 * 1024 ||
            m.cpu.systemAccess || !m.cpu.tlbEpoch || m.ata.position > 512 ||
            m.ata.remaining > 256 || !m.ata.heads || m.ata.heads > 255 || !m.ata.spt ||
            m.ata.spt > 63 || m.vga.width < 160 || m.vga.width > VGA::maxWidth ||
            m.vga.height < 100 || m.vga.height > VGA::maxHeight || m.vga.attrIndex >= 64 ||
            m.vga.dacComponent >= 3 || !m.columns || m.columns > 160 || !m.rows || m.rows > 256 ||
            m.activePage > 7)
            throw std::runtime_error("Invalid snapshot device state");
        if (m.vga.dispi[5] >= VGA::vramSize / 65536 || m.vga.vram.size() != VGA::vramSize ||
            ((m.vga.dispi[4] & 1) &&
             (!m.vga.dispi[6] || m.vga.dispi[6] < m.vga.dispi[1] ||
              uint64_t(m.vga.dispi[6]) * m.vga.dispi[2] * ((m.vga.dispi[3] + 7) / 8) >
                  VGA::vramSize)))
            throw std::runtime_error("Invalid snapshot framebuffer state");
        if (m.biosMouseCount > 2 || !m.ps2.state_.mouse.rate || m.ps2.state_.mouse.resolution > 3 ||
            (m.ps2.state_.mouse.id != 0 && m.ps2.state_.mouse.id != 3) || m.ps2.state_.mouse.buttons > 7 ||
            m.ps2.state_.mouse.reportedButtons > 7)
            throw std::runtime_error("Invalid snapshot mouse state");
        for (auto &e : m.cpu.tlb)
            if (e.permissions & ~6u)
                throw std::runtime_error("Invalid snapshot TLB permissions");
    }

  public:
    static void save(Machine &m, const std::string &directory) {
        validate(m);
        if (m.cpu.nativeFallback)
            throw std::runtime_error("Snapshot requires an instruction boundary");
        auto target = fs::absolute(directory).lexically_normal();
        if (fs::exists(target))
            throw std::runtime_error("Snapshot already exists: " + target.string());
        auto temporary = uniqueDirectory(target.parent_path(), "." + target.filename().string());
        try {
            DiskInfo disks[] = {DiskInfo(m.disk), DiskInfo(m.floppy)};
            auto copy = [&](Disk *disk, const char *name) {
                if (!disk)
                    return;
                disk->flush();
                auto destination = temporary / name;
                cloneDisk(disk->path, destination);
                fs::permissions(destination, fs::perms::owner_read, fs::perm_options::replace);
            };
            copy(m.disk.get(), "disk.raw");
            copy(m.floppy.get(), "floppy.raw");
            Writer writer;
            writer.data.reserve(m.cpu.mem.size() + 1024 * 1024);
            writer.bytes(magic, sizeof(magic));
            uint32_t version = 12, ram = m.cpu.mem.size();
            writer(version, ram);
            for (auto &d : disks)
                d.state(writer);
            state(m, writer, version);
            auto sum = checksum(writer.data.data(), writer.data.size());
            writer(sum);
            std::vector<uint8_t> packed(ZSTD_compressBound(writer.data.size()));
            auto packedSize = ZSTD_compress(packed.data(), packed.size(), writer.data.data(),
                                            writer.data.size(), 1);
            if (ZSTD_isError(packedSize))
                throw std::runtime_error("Snapshot zstd compression failed");
            std::ofstream file(temporary / "state.bin", std::ios::binary);
            file.write(reinterpret_cast<const char *>(packed.data()), packedSize);
            file.close();
            if (!file)
                throw std::runtime_error("Cannot write snapshot state");
            fs::rename(temporary, target);
        } catch (...) {
            std::error_code ignored;
            fs::remove_all(temporary, ignored);
            throw;
        }
        // First checkpoint seals the raw backing; subsequent live writes are compressed deltas.
        if ((m.disk && !m.disk->isOverlay()) || (m.floppy && !m.floppy->isOverlay())) {
            auto work = uniqueDirectory(target.parent_path() / ".runs", target.filename().string());
            auto convert = [&](std::unique_ptr<Disk> &disk, const char *name, bool floppy) {
                if (!disk || disk->isOverlay())
                    return;
                Disk::createOverlay((work / name).string(), (target / name).string());
                auto child = std::make_unique<Disk>((work / name).string(), floppy);
                child->heads = disk->heads;
                child->spt = disk->spt;
                disk = std::move(child);
            };
            convert(m.disk, "disk.raw", false);
            m.ata.disk = m.disk.get();
            convert(m.floppy, "floppy.raw", true);
        }
    }
    static void load(Machine &m, const std::string &directory) {
        auto source = fs::absolute(directory).lexically_normal();
        auto bytes = fs::file_size(source / "state.bin");
        if (bytes < 32 || bytes > 128 * 1024 * 1024)
            throw std::runtime_error("Invalid snapshot size");
        std::vector<uint8_t> packed(bytes);
        std::ifstream file(source / "state.bin", std::ios::binary);
        file.read(reinterpret_cast<char *>(packed.data()), packed.size());
        if (!file)
            throw std::runtime_error("Cannot read snapshot state");
        auto expanded = ZSTD_getFrameContentSize(packed.data(), packed.size());
        if (expanded < 32 || expanded > 128 * 1024 * 1024)
            throw std::runtime_error("Invalid snapshot zstd size");
        std::vector<uint8_t> data(expanded);
        auto actual = ZSTD_decompress(data.data(), data.size(), packed.data(), packed.size());
        if (ZSTD_isError(actual) || actual != data.size())
            throw std::runtime_error("Snapshot decompression failed");
        uint64_t stored = 0;
        for (unsigned n = 0; n < 8; n++)
            stored |= uint64_t(data[data.size() - 8 + n]) << (n * 8);
        if (checksum(data.data(), data.size() - 8) != stored)
            throw std::runtime_error("Snapshot checksum mismatch");
        Reader reader(data);
        uint8_t signature[8];
        reader.bytes(signature, sizeof(signature));
        uint32_t version, ram;
        reader(version, ram);
        if (std::memcmp(signature, magic, sizeof(magic)) || (version < 1 || version > 12) ||
            ram < 2 * 1024 * 1024 || ram > 64 * 1024 * 1024)
            throw std::runtime_error("Unsupported snapshot format or RAM size");
        DiskInfo disks[2];
        for (auto &d : disks)
            d.state(reader);
        disks[0].validate(source / "disk.raw");
        disks[1].validate(source / "floppy.raw");
        Machine staged("", "", ram);
        state(staged, reader, version);
        if (!disks[0].present &&
            (staged.ata.writing || staged.ata.remaining || (staged.ata.status & 8)))
            throw std::runtime_error("Snapshot ATA transfer has no disk");
        if (reader.position != reader.end)
            throw std::runtime_error("Unexpected snapshot trailing data");
        validate(staged);
        auto work = uniqueDirectory(source.parent_path() / ".runs", source.filename().string());
        try {
            auto open = [&](DiskInfo &info, const char *name,
                            bool floppy) -> std::unique_ptr<Disk> {
                if (!info.present)
                    return {};
                Disk::createOverlay((work / name).string(), (source / name).string());
                auto disk = std::make_unique<Disk>((work / name).string(), floppy);
                disk->sectors = info.sectors;
                disk->heads = info.heads;
                disk->spt = info.spt;
                return disk;
            };
            staged.disk = open(disks[0], "disk.raw", false);
            staged.floppy = open(disks[1], "floppy.raw", true);
        } catch (...) {
            std::error_code ignored;
            fs::remove_all(work, ignored);
            throw;
        }
        // Commit only after the complete state and both disk clones validate.
        auto watch = std::move(m.cpu.watchWrite);
        auto observe = std::move(m.cpu.observeFault);
        auto watchStart = m.cpu.watchStart, watchLength = m.cpu.watchLength;
        auto epoch = m.cpu.executionEpoch + 1;
        auto verbose = m.verbose;
        m = std::move(staged);
        m.cpu.executionEpoch = epoch;
        m.cpu.watchWrite = std::move(watch);
        m.cpu.observeFault = std::move(observe);
        m.cpu.watchStart = watchStart;
        m.cpu.watchLength = watchLength;
        m.verbose = verbose;
        m.bindDevices();
    }
};
void Machine::saveSnapshot(const std::string &directory) {
    SnapshotCodec::save(*this, directory);
}
void Machine::loadSnapshot(const std::string &directory) {
    SnapshotCodec::load(*this, directory);
}
} // namespace pc
