#include "nvme.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <vector>
namespace pc {
namespace {
uint64_t get(std::span<const uint8_t> b, size_t o, unsigned n) {
    uint64_t v = 0; for (unsigned i = 0; i < n; ++i) v |= uint64_t(b[o + i]) << (8 * i); return v;
}
void put(std::span<uint8_t> b, size_t o, uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) b[o + i] = uint8_t(v >> (8 * i));
}
uint64_t mask(unsigned n) { return n == 8 ? UINT64_MAX : (1ull << (8 * n)) - 1; }
void access(uint64_t o, unsigned n, uint64_t size) {
    if ((n != 1 && n != 2 && n != 4 && n != 8) || o % n || o > size || n > size - o)
        throw std::runtime_error("Invalid NVMe register access");
}
}
Nvme::Nvme(Disk &d, std::span<uint8_t> r, uint64_t b, std::function<void(bool)> irq)
    : disk(d), ram(r), ramBase(b), interrupt(std::move(irq)) {
    put(config, 0, 0x00101b36, 4); // Red Hat PCI vendor; original PCEmu device model.
    put(config, 8, 0x01080201, 4); // Mass storage / NVM / NVMe, revision 1.
    config[0x3c] = 0xff; config[0x3d] = 1;
}
uint64_t Nvme::configRead(uint64_t o, unsigned n) const {
    access(o & 0xfff, n, 4096);
    if (o >= 256) return mask(n);
    auto bytes = config;
    put(bytes, 4, pciCommand, 2);
    bool pending = false;
    for (const auto &q : cq) pending |= q.irq && q.pending;
    put(bytes, 6, pending ? 8 : 0, 2);
    put(bytes, 0x10, probe ? 0xffffc000 : bar, 4);
    return get(bytes, o, n);
}
void Nvme::configWrite(uint64_t o, uint64_t v, unsigned n) {
    access(o & 0xfff, n, 4096);
    if (o >= 256) return;
    if (o == 0x10 && n == 4) { probe = uint32_t(v) == UINT32_MAX; if (!probe) bar = v & 0xffffc000; return; }
    for (unsigned i = 0; i < n; ++i) {
        const auto at = o + i; const uint8_t byte = v >> (8 * i);
        if (at == 4) pciCommand = (pciCommand & 0xff00) | (byte & 7);
        if (at == 5) pciCommand = (pciCommand & 0xff) | (uint16_t(byte & 4) << 8);
        if (at == 0xc || at == 0xd || at == 0x3c) config[at] = byte;
    }
    updateIRQ();
}
bool Nvme::contains(uint64_t a, unsigned n) const {
    return (pciCommand & 2) && bar >= 0x10000000 && bar < 0x3f000000 &&
        a >= bar && a - bar < 0x4000 && n <= 0x4000 - (a - bar);
}
std::span<uint8_t> Nvme::memory(uint64_t a, size_t n) {
    if (a < ramBase || a - ramBase > ram.size() || n > ram.size() - (a - ramBase)) return {};
    return ram.subspan(a - ramBase, n);
}
void Nvme::updateIRQ() {
    bool pending = false;
    for (const auto &q : cq) pending |= q.irq && q.pending;
    interrupt(pending && !(interruptMask & 1) && !(pciCommand & 0x400));
}
uint64_t Nvme::read(uint64_t a, unsigned n) const {
    if (!contains(a, n)) throw std::runtime_error("NVMe BAR is not enabled/mapped");
    auto o = a - bar; access(o, n, 0x4000);
    std::array<uint8_t, 64> registers{};
    // MQES=255, contiguous queues, 500ms timeout, NVM, 4 KiB pages only.
    put(registers, 0, 255ull | (1ull << 16) | (1ull << 24) | (1ull << 37), 8);
    put(registers, 8, 0x00010100, 4);
    put(registers, 0xc, interruptMask, 4); put(registers, 0x10, interruptMask, 4);
    put(registers, 0x14, cc, 4); put(registers, 0x1c, csts, 4);
    put(registers, 0x24, aqa, 4); put(registers, 0x28, asq, 8); put(registers, 0x30, acq, 8);
    return o + n <= registers.size() ? get(registers, o, n) : 0;
}
void Nvme::enable() {
    ++resets; asyncPending = 0; queueFeature = 0x000e000e; asyncConfig = 0; coalescing = 0;
    sq = {}; cq = {};
    // Only round-robin NVM, 4 KiB pages, 64-byte SQ / 16-byte CQ entries.
    if ((cc & 0x3fff) != 1 || ((cc >> 16) & 0xff) != 0x46 ||
        (aqa & 0xf000f000) || (aqa & 0xfff) >= maxEntries || (aqa >> 16) >= maxEntries ||
        (asq & 4095) || (acq & 4095) ||
        memory(asq, ((aqa & 0xfff) + 1) * 64).empty() ||
        memory(acq, ((aqa >> 16) + 1) * 16).empty()) { csts = 2; return; }
    sq[0].base = asq; sq[0].size = (aqa & 0xfff) + 1;
    cq[0].base = acq; cq[0].size = (aqa >> 16) + 1; cq[0].irq = true;
    csts = 1;
}
void Nvme::write(uint64_t a, uint64_t v, unsigned n) {
    if (!contains(a, n)) throw std::runtime_error("NVMe BAR is not enabled/mapped");
    auto o = a - bar; access(o, n, 0x4000);
    if (o == 0xc && n == 4) interruptMask |= v;
    else if (o == 0x10 && n == 4) interruptMask &= ~v;
    else if (o == 0x14 && n == 4) {
        bool wasEnabled = cc & 1; cc = v;
        if (!(cc & 1)) { csts = 0; sq = {}; cq = {}; }
        else if (!wasEnabled) enable();
        if ((cc & 1) && (cc & 0xc000)) { disk.flush(); csts = (csts & ~12u) | 8; }
    } else if (!(cc & 1)) {
        if (o == 0x24 && n == 4) aqa = v;
        if ((o == 0x28 || o == 0x30) && n == 8) (o == 0x28 ? asq : acq) = v;
        if (o >= 0x28 && o <= 0x34 && n == 4) {
            auto &address = o < 0x30 ? asq : acq;
            unsigned shift = (o & 4) * 8;
            address = (address & ~(0xffffffffull << shift)) | ((v & UINT32_MAX) << shift);
        }
    } else if (o >= 0x1000 && o < 0x1000 + queueCount * 8 && n == 4 && (csts & 1) && (pciCommand & 4)) {
        unsigned id = (o - 0x1000) / 8;
        if (o & 4) {
            auto &q = cq[id];
            if (!q.size || v >= q.size) { csts |= 2; return; }
            unsigned advanced = (unsigned(v) + q.size - q.head) % q.size;
            if (advanced > q.pending) { csts |= 2; return; }
            q.head = v; q.pending -= advanced;
            for (unsigned i = 0; i < queueCount; ++i) if (sq[i].size && sq[i].cq == id) process(i);
        } else {
            auto &q = sq[id];
            if (!q.size || v >= q.size) { csts |= 2; return; }
            q.tail = v; process(id);
        }
    }
    updateIRQ();
}
bool Nvme::transfer(std::span<const uint8_t> cmd, std::span<uint8_t> data, bool toGuest) {
    // Validate the entire PRP scatter list before touching memory or disk.
    if (cmd[1] & 0xc0) return false; // Only PRP, no SGL.
    uint64_t address = get(cmd, 24, 8), second = get(cmd, 32, 8);
    if (address & 3) return false;
    size_t left = data.size(), first = std::min(left, size_t(4096 - (address & 4095)));
    std::vector<std::span<uint8_t>> pieces;
    auto add = [&](uint64_t p, size_t bytes) {
        auto span = memory(p, bytes); if (span.size() != bytes) return false;
        pieces.push_back(span); return true;
    };
    if (!add(address, first)) return false;
    left -= first;
    if (left && left <= 4096) {
        if ((second & 4095) || !add(second, left)) return false;
        left = 0;
    } else if (left) {
        // MDTS=5 limits requests to 128 KiB, so a single PRP list suffices.
        if (second & 7) return false;
        auto list = memory(second, ((left + 4095) / 4096) * 8);
        if (list.empty() || list.size() > 4096 - (second & 4095)) return false;
        for (unsigned i = 0; left; ++i) {
            address = get(list, i * 8, 8); size_t bytes = std::min(left, size_t(4096));
            if ((address & 4095) || !add(address, bytes)) return false;
            left -= bytes;
        }
    }
    size_t at = 0;
    for (auto piece : pieces) {
        if (toGuest) std::copy_n(data.begin() + at, piece.size(), piece.begin());
        else std::copy(piece.begin(), piece.end(), data.begin() + at);
        at += piece.size();
    }
    return true;
}
uint16_t Nvme::execute(unsigned qid, std::span<const uint8_t> cmd, uint32_t &result, bool &defer) {
    unsigned op = cmd[0]; uint32_t ns = get(cmd, 4, 4), d10 = get(cmd, 40, 4), d11 = get(cmd, 44, 4);
    if ((cmd[1] & 0xc3) || get(cmd, 16, 8)) return 2;
    if (!qid) {
        if (op == 0xc) { if (asyncPending == 4) return 0x105; ++asyncPending; defer = true; return 0; } // AER: no events generated, retained until reset.
        if (op == 6) {
            std::array<uint8_t, 4096> data{};
            unsigned cns = d10 & 0xff;
            if (cns == 1) {
                put(data, 0, 0x1b36, 2); put(data, 2, 0x1b36, 2);
                std::fill(data.begin() + 4, data.begin() + 72, ' ');
                std::memcpy(data.data() + 4, "PCEMU000000000000001", 20);
                std::memcpy(data.data() + 24, "PCEmu NVMe", 10);
                std::memcpy(data.data() + 64, "0.1     ", 8);
                data[77] = 5; put(data, 78, 1, 2); put(data, 80, 0x10100, 4);
                data[259] = 3; // Four outstanding async event requests.
                data[512] = 0x66; data[513] = 0x44; put(data, 516, 1, 4);
                put(data, 520, 0, 2); // No optional NVM commands, no volatile cache.
            } else if (cns == 0 && ns == 1) {
                put(data, 0, disk.sectors, 8); put(data, 8, disk.sectors, 8); put(data, 16, disk.sectors, 8);
                data[130] = 9; // LBAF0: 512-byte logical blocks, no metadata.
            } else if (cns == 2) { if (ns == 0) put(data, 0, 1, 4); }
            else return cns == 0 ? 0xb : 2;
            return transfer(cmd, data, true) ? 0 : 4;
        }
        if (op == 1 || op == 5) {
            unsigned id = d10 & 0xffff, count = (d10 >> 16) + 1;
            unsigned completion = d11 >> 16;
            auto base = get(cmd, 24, 8);
            if (!id || id >= queueCount || count < 2 || count > maxEntries || !(d11 & 1) ||
                (base & 4095) || memory(base, count * (op == 1 ? 64 : 16)).empty()) return 2;
            if (op == 5) {
                if (id > (queueFeature >> 16) + 1) return 2;
                if (cq[id].size || completion) return 2; // Only interrupt vector zero.
                cq[id] = {}; cq[id].base = base; cq[id].size = count; cq[id].irq = d11 & 2;
            } else {
                if (id > (queueFeature & 0xffff) + 1) return 2;
                if (sq[id].size || completion >= queueCount || !cq[completion].size) return 2;
                sq[id] = {}; sq[id].base = base; sq[id].size = count; sq[id].cq = completion;
            }
            return 0;
        }
        if (op == 0 || op == 4) {
            unsigned id = d10 & 0xffff;
            if (!id || id >= queueCount) return 2;
            if (op == 0) { if (!sq[id].size) return 2; sq[id] = {}; }
            else {
                for (const auto &q : sq) if (q.size && q.cq == id) return 0x10c;
                if (!cq[id].size) return 2;
                cq[id] = {};
            }
            return 0;
        }
        if (op == 9 || op == 0xa) {
            unsigned feature = d10 & 0xff;
            if (d10 & ~0xffu) return 2; // No save/select support.
            if (feature == 7) {
                if (op == 9) {
                    for (unsigned i = 1; i < queueCount; ++i) if (sq[i].size || cq[i].size) return 0x12;
                    queueFeature = std::min(d11 & 0xffffu, queueCount - 2) |
                        (std::min(d11 >> 16, queueCount - 2) << 16);
                }
                result = queueFeature; return 0;
            }
            if (feature == 8) { if (op == 9) coalescing = d11 & 0xffff; result = coalescing; return 0; }
            if (feature == 0xb) { if (op == 9) asyncConfig = d11 & 0xff; result = asyncConfig; return 0; }
            if (feature == 9) return (d11 & 0xffff) ? 2 : 0;
            if (feature == 6 || feature == 1 || feature == 2) return op == 9 && d11 ? 2 : 0;
            return 2;
        }
        if (op == 2) {
            unsigned log = d10 & 0xff, bytes = (((d10 >> 16) & 0xfff) + 1) * 4;
            if (log != 1 && log != 2 && log != 3) return 2;
            std::vector<uint8_t> data(bytes);
            if (log == 2 && bytes >= 3) put(data, 1, 300, 2); // 300 Kelvin.
            if (log == 3 && bytes >= 16) { data[0] = 1; std::memcpy(data.data() + 8, "0.1     ", 8); }
            return transfer(cmd, data, true) ? 0 : 4;
        }
        if (op == 8) { result = 1; return 0; } // Abort: command not found.
        return 1;
    }
    if (ns != 1 && !(op == 0 && ns == UINT32_MAX)) return 0xb;
    if (op == 0) { disk.flush(); return 0; }
    if (op != 1 && op != 2) return 1;
    uint64_t lba = get(cmd, 40, 8); unsigned count = (get(cmd, 48, 4) & 0xffff) + 1;
    if (count > 256 || (get(cmd, 48, 4) & 0x3c000000)) return 2;
    if (lba >= disk.sectors || count > disk.sectors - lba) return 0x80;
    std::vector<uint8_t> data(count * 512);
    if (op == 1) {
        if (!transfer(cmd, data, false)) return 4;
        if (!disk.write(lba, count, data.data())) return 6;
        disk.flush(); // No volatile write cache advertised.
    } else {
        if (!disk.read(lba, count, data.data())) return 6;
        if (!transfer(cmd, data, true)) return 4;
    }
    return 0;
}
void Nvme::process(unsigned id) {
    auto &s = sq[id];
    if (!s.size || !(csts & 1) || (csts & 2)) return;
    auto &c = cq[s.cq];
    while (s.head != s.tail && c.size && c.pending < c.size - 1) {
        auto commandMemory = memory(s.base + s.head * 64, 64);
        std::array<uint8_t, 64> cmd{};
        if (commandMemory.size() != 64) { csts |= 2; return; }
        std::copy(commandMemory.begin(), commandMemory.end(), cmd.begin());
        s.head = (s.head + 1) % s.size;
        uint32_t result = 0; bool defer = false;
        uint16_t status = execute(id, cmd, result, defer); ++requests;
        if (id) ++ioRequests; else ++adminRequests;
        if (status) { ++errors; lastError = (uint32_t(cmd[0]) << 16) | status; }
        if (defer) continue;
        auto completion = memory(c.base + c.tail * 16, 16);
        if (completion.size() != 16) { csts |= 2; return; }
        put(completion, 0, result, 4); put(completion, 4, 0, 4);
        put(completion, 8, s.head, 2); put(completion, 10, id, 2);
        put(completion, 12, get(cmd, 2, 2), 2);
        put(completion, 14, (status << 1) | c.phase, 2);
        if (++c.tail == c.size) { c.tail = 0; c.phase = !c.phase; }
        ++c.pending;
    }
    updateIRQ();
}
std::string Nvme::diagnostics() const {
    std::ostringstream out;
    out << "nvmeAdmin=" << adminRequests << " nvmeIo=" << ioRequests << " nvmeErrors=" << errors
        << " nvmeResets=" << resets << std::hex << " nvmeLastError=0x" << lastError
        << " nvmeCC=0x" << cc << " nvmeCSTS=0x" << csts;
    return out.str();
}
} // namespace pc
