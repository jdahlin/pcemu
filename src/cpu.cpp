#include "cpu.hpp"
#include "profile.hpp"
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>
namespace pc {
struct SystemAccess {
    CPU &c;
    explicit SystemAccess(CPU &c) : c(c) {
        c.systemAccess++;
    }
    ~SystemAccess() {
        c.systemAccess--;
    }
};
uint32_t CPU::systemRead(uint32_t a, int w) {
    SystemAccess access(*this);
    return read(a, w);
}
static uint32_t mask(int w) {
    return w == 4 ? 0xffffffffu : (1u << (w * 8)) - 1;
}
static int32_t signedValue(uint32_t v, int w) {
    return w == 1 ? int8_t(v) : w == 2 ? int16_t(v) : int32_t(v);
}
static uint32_t loadRAM(const uint8_t *p, int width) {
    if (width == 1)
        return *p;
    if (width == 2) {
        uint16_t value;
        std::memcpy(&value, p, 2);
        return value;
    }
    uint32_t value = 0;
    if (width == 4)
        std::memcpy(&value, p, 4);
    else
        std::memcpy(&value, p, width);
    return value;
}
static void storeRAM(uint8_t *p, uint32_t value, int width) {
    if (width == 1)
        *p = uint8_t(value);
    else if (width == 2)
        std::memcpy(p, &value, 2);
    else if (width == 4)
        std::memcpy(p, &value, 4);
    else
        std::memcpy(p, &value, width);
}
CPU::CPU(size_t ram) : mem(ram), generation((ram + 4095) / 4096), codeUsed((ram + 63) / 64) {}
void CPU::markCode(uint32_t p) {
    if (p < mem.size())
        codeUsed[p >> 6] |= uint64_t(1) << (p & 63);
}
void CPU::written(uint32_t p, unsigned width) {
    if (p >= mem.size())
        return;
    unsigned n = std::min(width, 64 - (p & 63));
    uint64_t bits = n == 64 ? ~uint64_t(0) : ((uint64_t(1) << n) - 1) << (p & 63);
    if (codeUsed[p >> 6] & bits)
        generation[p >> 12]++;
    if (n < width)
        written(p + n, width - n);
}
void CPU::flushTLB() {
    repeatResume.valid = false;
    if (++tlbEpoch == 0) {
        for (auto &entry : tlb)
            entry.epoch = 0;
        tlbEpoch = 1;
    }
    tlbCR3 = cr[3];
}
uint32_t CPU::physical(uint32_t a, bool wr) {
    if (cr[0] & 0x80000000u) {
        if (tlbCR3 != cr[3])
            flushTLB();
        bool user = cpl() == 3 && !systemAccess;
        auto fault = [&](bool present) -> void {
            cr[2] = a;
            throw Fault{14, uint32_t((present ? 1 : 0) | (wr ? 2 : 0) | (user ? 4 : 0)), true};
        };
        uint32_t page = a >> 12;
        auto &entry = tlb[(page ^ (page >> 10)) & (tlb.size() - 1)];
        if (entry.epoch != tlbEpoch || entry.tag != page) {
            auto raw = [&](uint32_t p) {
                if (uint64_t(p) + 4 > mem.size())
                    throw std::runtime_error("page table outside RAM");
                uint32_t value;
                std::memcpy(&value, &mem[p], 4);
                return value;
            };
            auto pdeAddress = (cr[3] & ~4095u) + (a >> 22) * 4;
            if (!a20)
                pdeAddress &= ~(1u << 20);
            auto pde = raw(pdeAddress);
            if (!(pde & 1))
                fault(false);
            auto pteAddress = (pde & ~4095u) + ((a >> 12) & 1023) * 4;
            if (!a20)
                pteAddress &= ~(1u << 20);
            auto pte = raw(pteAddress);
            if (!(pte & 1))
                fault(false);
            auto permissions = (pde & pte) & 6;
            if (user && (!(permissions & 4) || (wr && !(permissions & 2))))
                fault(true);
            pde |= 32;
            pte |= 32;
            written(pdeAddress, 4);
            std::memcpy(&mem[pdeAddress], &pde, 4);
            written(pteAddress, 4);
            std::memcpy(&mem[pteAddress], &pte, 4);
            entry = {page,          tlbEpoch, pte & ~4095u, pteAddress, uint8_t(permissions),
                     bool(pte & 64)};
        }
        if (user && (!(entry.permissions & 4) || (wr && !(entry.permissions & 2))))
            fault(true);
        if (wr && !entry.dirty) {
            written(entry.pteAddress, 1);
            mem[entry.pteAddress] |= 64;
            entry.dirty = true;
        }
        a = entry.base | (a & 4095);
    }
    if (!a20)
        a &= ~(1u << 20);
    return a;
}
uint32_t CPU::read(uint32_t a, int w) {
    if (!(cr[0] & 0x80000000u) && a20 && uint64_t(a) + w <= mem.size() &&
        !(a < 0xb0000 && uint64_t(a) + w > 0xa0000 && videoRead)) {
        return loadRAM(&mem[a], w);
    }
    if ((a & 4095) + unsigned(w) <= 4096) {
        auto p = physical(a);
        if (uint64_t(p) + w <= mem.size() &&
            !(p < 0xb0000 && uint64_t(p) + w > 0xa0000 && videoRead)) {
            return loadRAM(&mem[p], w);
        }
    }
    uint32_t v = 0;
    for (int i = 0; i < w; i++) {
        auto p = physical(a + i);
        auto byte = ((p >= 0xa0000 && p < 0xb0000) || (p >= 0xe0000000 && p < 0xe1000000)) && videoRead ? videoRead(p)
                    : p < mem.size()                         ? mem[p]
                                                             : 0xff;
        v |= uint32_t(byte) << (i * 8);
    }
    return v;
}
void CPU::write(uint32_t a, uint32_t v, int w) {
    if (watchWrite && uint64_t(a) < uint64_t(watchStart) + watchLength &&
        uint64_t(a) + w > watchStart)
        watchWrite(a, v, w);
    if (!(cr[0] & 0x80000000u) && a20 && uint64_t(a) + w <= mem.size() &&
        !(a < 0xb0000 && uint64_t(a) + w > 0xa0000 && videoWrite)) {
        storeRAM(&mem[a], v, w);
        written(a, w);
        return;
    }
    if ((a & 4095) + unsigned(w) <= 4096) {
        auto p = physical(a, true);
        if (uint64_t(p) + w <= mem.size() &&
            !(p < 0xb0000 && uint64_t(p) + w > 0xa0000 && videoWrite)) {
            storeRAM(&mem[p], v, w);
            written(p, w);
            return;
        }
    }
    // Preflight every page before writing a cross-page operand: #PF must not
    // leave the first bytes modified when the second page is absent/read-only.
    uint32_t pages[4];
    for (int i = 0; i < w; i++)
        pages[i] = physical(a + i, true);
    for (int i = 0; i < w; i++) {
        auto p = pages[i];
        if (((p >= 0xa0000 && p < 0xb0000) || (p >= 0xe0000000 && p < 0xe1000000)) && videoWrite)
            videoWrite(p, v >> (i * 8));
        else if (p < mem.size()) {
            mem[p] = v >> (i * 8);
            written(p, 1);
        }
    }
}
uint32_t CPU::get(int reg, int w) const {
    return w == 1 ? (r[reg & 3] >> ((reg & 4) ? 8 : 0)) & 255 : r[reg] & mask(w);
}
void CPU::set(int reg, uint32_t v, int w) {
    if (w == 1) {
        int sh = (reg & 4) ? 8 : 0;
        auto &x = r[reg & 3];
        x = (x & ~(255u << sh)) | ((v & 255) << sh);
    } else
        r[reg] = (r[reg] & ~mask(w)) | (v & mask(w));
}
uint32_t CPU::address(int seg, uint32_t off) const {
    if ((cr[0] & 1) && !(flags & 0x20000) && s[seg].unusable)
        throw Fault{uint8_t(seg == SS ? 12 : 13), 0, true};
    return s[seg].base + off;
}
bool CPU::descriptorVisible(uint16_t selector, uint32_t high) const {
    // Conforming code is visible independently of DPL; all other
    // descriptors must satisfy both the current and requested privilege.
    if ((high & 0x1c00) == 0x1c00)
        return true;
    unsigned dpl = (high >> 13) & 3;
    return dpl >= cpl() && dpl >= (selector & 3);
}
void CPU::segment(int seg, uint16_t sel) {
    auto &d = s[seg];
    if (!(cr[0] & 1) || (flags & 0x20000)) {
        d.selector = sel;
        d.unusable = false;
        d.base = uint32_t(sel) << 4;
        d.limit = 0xffff;
        d.big = false;
        return;
    }
    if (!(sel & ~3)) {
        if (seg == CS || seg == SS)
            throw Fault{13, 0, true};
        d.selector = sel;
        d.unusable = true;
        d.base = 0;
        d.limit = 0;
        d.big = false;
        return;
    }
    auto base = (sel & 4) ? ldtBase : gdtBase;
    auto limit = (sel & 4) ? ldtLimit : gdtLimit;
    auto off = uint32_t(sel & ~7);
    if (off + 7 > limit)
        throw Fault{13, uint32_t(sel & ~3u), true};
    uint32_t lo = systemRead(base + off, 4), hi = systemRead(base + off + 4, 4);
    // Type checks precede presence checks. Windows uses invalid nonpresent
    // descriptors for stale selectors; those require #GP, not demand-load #NP.
    unsigned type = (hi >> 8) & 15;
    if (!(hi & 0x1000) || (seg == CS && !(type & 8)) ||
        (seg == SS && ((type & 8) || !(type & 2))) ||
        (seg != CS && seg != SS && (type & 8) && !(type & 2)))
        throw Fault{13, uint32_t(sel & ~3u), true};
    if (!(hi & 0x8000))
        throw Fault{uint8_t(seg == SS ? 12 : 11), uint32_t(sel & ~3u), true};
    d.selector = sel;
    d.unusable = false;
    d.base = (lo >> 16) | ((hi & 255) << 16) | (hi & 0xff000000);
    d.limit = (lo & 65535) | (hi & 0xf0000);
    if (hi & 0x800000)
        d.limit = (d.limit << 12) | 4095;
    d.big = hi & 0x400000;
}
uint32_t CPU::fetch(int w) {
    auto v = read(address(CS, ip), w);
    ip = (ip + w) & (s[CS].big ? 0xffffffff : 0xffff);
    return v;
}
void CPU::push(uint32_t v, int w) {
    uint32_t p = (r[SP] - w) & (s[SS].big ? 0xffffffff : 0xffff);
    set(SP, p, s[SS].big ? 4 : 2);
    write(address(SS, p), v, w);
}
uint32_t CPU::pop(int w) {
    auto p = get(SP, s[SS].big ? 4 : 2), v = read(address(SS, p), w);
    set(SP, p + w, s[SS].big ? 4 : 2);
    return v;
}
CPU::Operand CPU::decode(uint8_t m, bool a32, int over) {
    profile::operand(m);
    if ((m >> 6) == 3)
        return {true, uint8_t(m & 7), 0};
    uint32_t off = 0;
    int seg = DS, rm = m & 7, mod = m >> 6;
    if (a32) {
        if (rm == 4) {
            auto sib = fetch(1);
            int base = sib & 7, idx = (sib >> 3) & 7;
            if (idx != 4)
                off = r[idx] << (sib >> 6);
            if (base == 5 && mod == 0)
                off += fetch(4);
            else {
                off += r[base];
                if (base == BP || base == SP)
                    seg = SS;
            }
        } else if (rm == 5 && mod == 0)
            off = fetch(4);
        else {
            off = r[rm];
            if (rm == BP)
                seg = SS;
        }
        if (mod == 1)
            off += int8_t(fetch(1));
        if (mod == 2)
            off += fetch(4);
    } else {
        switch (rm) {
        case 0:
            off = r[BX] + r[SI];
            break;
        case 1:
            off = r[BX] + r[DI];
            break;
        case 2:
            off = r[BP] + r[SI];
            seg = SS;
            break;
        case 3:
            off = r[BP] + r[DI];
            seg = SS;
            break;
        case 4:
            off = r[SI];
            break;
        case 5:
            off = r[DI];
            break;
        case 6:
            if (mod == 0)
                off = fetch(2);
            else {
                off = r[BP];
                seg = SS;
            }
            break;
        case 7:
            off = r[BX];
            break;
        }
        if (mod == 1)
            off += int8_t(fetch(1));
        if (mod == 2)
            off += fetch(2);
        off &= 65535;
    }
    return {false, 0, over == -2 ? off : address(over < 0 ? seg : over, off)};
}
uint32_t CPU::load(Operand o, int w) {
    return o.reg ? get(o.index, w) : read(o.linear, w);
}
void CPU::store(Operand o, uint32_t v, int w) {
    if (o.reg)
        set(o.index, v, w);
    else
        write(o.linear, v, w);
}
uint32_t CPU::alu(int op, uint32_t a, uint32_t b, int w) {
    uint32_t m = mask(w), sign = 1u << (w * 8 - 1), v = 0;
    uint64_t full = 0;
    bool carry = false, overflow = false;
    a &= m;
    b &= m;
    uint32_t c = flags & CF;
    switch (op) {
    case 0:
    case 2:
        full = uint64_t(a) + b + (op == 2 ? c : 0);
        v = full & m;
        carry = full > m;
        overflow = (~(a ^ b) & (a ^ v) & sign);
        break;
    case 3:
    case 5:
    case 7:
        full = uint64_t(b) + (op == 3 ? c : 0);
        v = (uint64_t(a) - full) & m;
        carry = uint64_t(a) < full;
        overflow = ((a ^ b) & (a ^ v) & sign);
        break;
    case 1:
        v = a | b;
        break;
    case 4:
        v = a & b;
        break;
    case 6:
        v = a ^ b;
        break;
    }
    flags = (flags & ~(CF | PF | AF | ZF | SF | OF)) | (carry ? CF : 0) | (overflow ? OF : 0) |
            (!v ? ZF : 0) | ((v & sign) ? SF : 0) | (__builtin_parity(v & 255) ? 0 : PF) |
            (((a ^ b ^ v) & 16) ? AF : 0) | 2;
    return v;
}
bool CPU::condition(int c) const {
    bool of = flags & OF, cf = flags & CF, zf = flags & ZF, sf = flags & SF, pf = flags & PF;
    switch (c & 14) {
    case 0:
        return of ^ (c & 1);
    case 2:
        return cf ^ (c & 1);
    case 4:
        return zf ^ (c & 1);
    case 6:
        return (cf || zf) ^ (c & 1);
    case 8:
        return sf ^ (c & 1);
    case 10:
        return pf ^ (c & 1);
    case 12:
        return (sf != of) ^ (c & 1);
    default:
        return (zf || (sf != of)) ^ (c & 1);
    }
}
uint32_t CPU::shift(int op, uint32_t v, unsigned n, int w) {
    n &= 31;
    if (!n)
        return v;
    uint32_t sign = 1u << (w * 8 - 1), m = mask(w), orig = v;
    bool c = false;
    if (op < 2)
        n %= w * 8;
    else if (op < 4 && w < 4)
        n %= (w * 8 + 1);
    if (!n)
        return v;
    for (unsigned i = 0; i < n; i++) {
        bool oldc = flags & CF;
        switch (op) {
        case 0:
            c = v & sign;
            v = ((v << 1) | c) & m;
            break;
        case 1:
            c = v & 1;
            v = (v >> 1) | (c ? sign : 0);
            break;
        case 2:
            c = v & sign;
            v = ((v << 1) | oldc) & m;
            break;
        case 3:
            c = v & 1;
            v = (v >> 1) | (oldc ? sign : 0);
            break;
        case 4:
        case 6:
            c = v & sign;
            v = (v << 1) & m;
            break;
        case 5:
            c = v & 1;
            v >>= 1;
            break;
        case 7:
            c = v & 1;
            v = (v >> 1) | (v & sign);
            break;
        }
        flags = (flags & ~CF) | (c ? CF : 0);
    }
    if (op >= 4)
        flags = (flags & ~(SF | ZF | PF)) | ((v & sign) ? SF : 0) | (!v ? ZF : 0) |
                (__builtin_parity(v & 255) ? 0 : PF);
    if (n == 1) {
        bool o = op == 5                ? (orig & sign)
                 : op == 7              ? false
                 : (op == 1 || op == 3) ? ((v ^ (v << 1)) & sign)
                                        : bool(v & sign) != c;
        flags = (flags & ~OF) | (o ? OF : 0);
    }
    return v;
}
void CPU::farTransfer(uint16_t sel, uint32_t off, bool call, int w) {
    if ((cr[0] & 1) && !(flags & 0x20000)) {
        auto table = (sel & 4) ? ldtBase : gdtBase;
        auto descriptor = table + (sel & ~7u);
        auto lo = systemRead(descriptor, 4), hi = systemRead(descriptor + 4, 4);
        int type = (hi >> 8) & 15;
        if (!(hi & 0x1000)) {
            if (type != 4 && type != 12)
                throw std::runtime_error("unsupported far transfer to system descriptor at " +
                                         location());
            if (!call)
                throw std::runtime_error("JMP to call gate is not implemented");
            int gw = type == 12 ? 4 : 2;
            uint16_t target = lo >> 16;
            uint32_t targetOff = (lo & 65535) | (gw == 4 ? (hi & 0xffff0000) : 0);
            auto targetHi = systemRead(((target & 4) ? ldtBase : gdtBase) + (target & ~7u) + 4, 4);
            unsigned oldLevel = s[CS].selector & 3, newLevel = (targetHi >> 13) & 3;
            if (newLevel < oldLevel) {
                if (!trSelector)
                    throw std::runtime_error("call gate requires TSS");
                uint32_t oldSP = get(SP, gw), oldSS = s[SS].selector;
                unsigned stackOffset = tr32 ? 4 + newLevel * 8 : 2 + newLevel * 4;
                auto newSP = systemRead(trBase + stackOffset, tr32 ? 4 : 2),
                     newSS = systemRead(trBase + stackOffset + (tr32 ? 4 : 2), 2);
                std::vector<uint32_t> params;
                for (unsigned n = 0; n < (hi & 31); n++)
                    params.push_back(read(
                        address(SS, (oldSP + n * gw) & (s[SS].big ? 0xffffffffu : 65535u)), gw));
                SystemAccess innerStack(*this);
                segment(SS, newSS);
                set(SP, newSP, tr32 ? 4 : 2);
                push(oldSS, gw);
                push(oldSP, gw);
                for (auto it = params.rbegin(); it != params.rend(); it++)
                    push(*it, gw);
                push(s[CS].selector, gw);
                push(ip, gw);
                segment(CS, (target & ~3) | newLevel);
                ip = targetOff;
                return;
            }
            push(s[CS].selector, gw);
            push(ip, gw);
            segment(CS, (target & ~3) | newLevel);
            ip = targetOff;
            return;
        }
    }
    if (call) {
        push(s[CS].selector, w);
        push(ip, w);
    }
    segment(CS, sel);
    ip = off;
}
void CPU::farReturn(int w, uint16_t extra, bool iret) {
    bool vm = flags & 0x20000;
    if (iret && vm && ((flags >> 12) & 3) != 3)
        throw Fault{13, 0, true};
    auto off = pop(w), sel = pop(w);
    uint32_t f = iret ? pop(w) : flags;
    unsigned old = cpl();
    if (iret && (cr[0] & 1) && !vm && old == 0 && w == 4 && (f & 0x20000)) {
        auto newSP = pop(4), newSS = pop(4), newES = pop(4), newDS = pop(4), newFS = pop(4),
             newGS = pop(4);
        flags = (f & 0x00037fd7) | 2;
        segment(CS, sel);
        ip = off & 65535;
        segment(SS, newSS);
        r[SP] = newSP;
        segment(ES, newES);
        segment(DS, newDS);
        segment(FS, newFS);
        segment(GS, newGS);
        return;
    }
    if ((cr[0] & 1) && !vm && (sel & 3) > old) {
        if (!iret)
            set(SP, get(SP, s[SS].big ? 4 : 2) + extra, s[SS].big ? 4 : 2);
        auto newSP = pop(w), newSS = pop(w);
        segment(SS, newSS);
        // The return operand size selects the saved pointer width. SS.B selects
        // stack addressing, not the width of ESP restored by a 32-bit return.
        set(SP, newSP, w);
        if (!iret)
            set(SP, get(SP, s[SS].big ? 4 : 2) + extra, s[SS].big ? 4 : 2);
    } else if (!iret)
        set(SP, get(SP, s[SS].big ? 4 : 2) + extra, s[SS].big ? 4 : 2);
    segment(CS, sel);
    ip = off;
    if (iret) {
        uint32_t change = w == 4 ? 0x00017fd7 : 0x7fd7;
        if ((cr[0] & 1) && old != 0)
            change &= ~0x3000u;
        if ((cr[0] & 1) && old > ((flags >> 12) & 3))
            change &= ~uint32_t(IF);
        flags = (flags & ~change) | (f & change) | 2;
    }
}
void CPU::interrupt(uint8_t n, bool software) {
    repeatResume.valid = false;
    bool vm = flags & 0x20000;
    if (!(cr[0] & 1)) {
        if (bios && bios(n))
            return;
        push(flags, 2);
        push(s[CS].selector, 2);
        push(ip, 2);
        flags &= ~(IF | TF);
        uint32_t vec = read(n * 4, 4);
        segment(CS, vec >> 16);
        ip = vec & 65535;
        halted = false;
        return;
    }
    // BIOS ROM service stubs are host-backed firmware, including when a V86
    // monitor reflects a BIOS interrupt into its virtual 8086 address space.
    if (vm && software && n == 0xfe && s[CS].selector == 0xf000 && bios && bios(n))
        return;
    if (vm && software && ((flags >> 12) & 3) != 3)
        throw Fault{13, 0, true};
    SystemAccess internal(*this);
    uint32_t off = n * 8;
    if (off + 7 > idtLimit)
        throw Fault{13, off + 2, true};
    uint32_t lo = read(idtBase + off, 4), hi = read(idtBase + off + 4, 4);
    int w = (hi & 0x800) ? 4 : 2;
    if (!(hi & 0x8000))
        throw Fault{11, off + 2, true};
    unsigned oldLevel = cpl();
    if (software && oldLevel > ((hi >> 13) & 3))
        throw Fault{13, off + 2, true};
    uint16_t sel = lo >> 16;
    auto codeHi = read(((sel & 4) ? ldtBase : gdtBase) + (sel & ~7u) + 4, 4);
    unsigned newLevel = (codeHi & 0x400) ? oldLevel : (codeHi >> 13) & 3;
    uint32_t savedFlags = flags, oldSS = s[SS].selector, oldSP = get(SP, w), oldCS = s[CS].selector;
    uint16_t oldData[4] = {s[ES].selector, s[DS].selector, s[FS].selector, s[GS].selector};
    if (vm && (w != 4 || newLevel != 0 || (codeHi & 0x400)))
        throw Fault{13, uint32_t(sel & ~3), true};
    if (newLevel < oldLevel) {
        if (!trSelector)
            throw Fault{10, 0, true};
        unsigned stackOffset = tr32 ? 4 + newLevel * 8 : 2 + newLevel * 4;
        auto newSP = read(trBase + stackOffset, tr32 ? 4 : 2),
             newSS = read(trBase + stackOffset + (tr32 ? 4 : 2), 2);
        flags &= ~0x20000u;
        segment(SS, newSS);
        set(SP, newSP, tr32 ? 4 : 2);
        if (vm) {
            push(oldData[3], 4);
            push(oldData[2], 4);
            push(oldData[1], 4);
            push(oldData[0], 4);
        }
        push(oldSS, w);
        push(oldSP, w);
    }
    push(savedFlags, w);
    push(oldCS, w);
    push(ip, w);
    if (!(hi & 0x100))
        flags &= ~IF;
    flags &= ~(TF | 0x4000u | 0x20000u);
    if (vm) {
        segment(ES, 0);
        segment(DS, 0);
        segment(FS, 0);
        segment(GS, 0);
    }
    segment(CS, (sel & ~3) | newLevel);
    ip = (lo & 65535) | (w == 4 ? (hi & 0xffff0000) : 0);
    halted = false;
}
void CPU::ioPermission(uint16_t port, int width) {
    if (!(cr[0] & 1) || (!(flags & 0x20000) && cpl() <= ((flags >> 12) & 3)))
        return;
    if (!tr32 || trLimit < 0x67)
        throw Fault{13, 0, true};
    SystemAccess internal(*this);
    auto bitmap = read(trBase + 0x66, 2);
    for (int i = 0; i < width; i++) {
        uint32_t bit = uint32_t(port) + i, off = bitmap + bit / 8;
        if (off > trLimit || (read(trBase + off, 1) & (1u << (bit & 7))))
            throw Fault{13, 0, true};
    }
}
std::string CPU::location() const {
    char b[80];
    snprintf(b, sizeof b, "%04x:%08x", s[CS].selector, ip);
    return b;
}
void CPU::step() {
    // Faults restart the instruction. Demand-loaded NE segments depend on this.
    uint32_t savedRegs[8];
    Segment savedSegments[6];
    std::memcpy(savedRegs, r, sizeof r);
    std::memcpy(savedSegments, s, sizeof s);
    auto savedIP = ip, savedFlags = flags;
    try {
        stepImpl();
    } catch (const Fault &fault) {
        if (!fault.partialRepeat)
            std::memcpy(r, savedRegs, sizeof r);
        std::memcpy(s, savedSegments, sizeof s);
        ip = savedIP;
        flags = fault.partialRepeat ? fault.repeatFlags : savedFlags;
        interruptBlockedThrough = 0;
        if (observeFault)
            observeFault(fault);
        try {
            int frameWidth =
                (cr[0] & 1) && (systemRead(idtBase + fault.vector * 8 + 4, 4) & 0x800) ? 4 : 2;
            interrupt(fault.vector);
            if (fault.hasError && (cr[0] & 1))
                push(fault.error, frameWidth);
        } catch (...) {
            throw std::runtime_error(
                "fault delivery failed: vector=" + std::to_string(fault.vector) +
                " error=" + std::to_string(fault.error) + " at " + location());
        }
    }
}
void CPU::stepImpl() {
    if (halted)
        return;
    uint32_t start = ip;
    bool o32 = s[CS].big, a32 = s[CS].big;
    int over = -1, rep = 0;
    uint8_t op;
    int prefixes = 0;
    for (;;) {
        if (++prefixes > 15)
            throw std::runtime_error("instruction exceeds 15 bytes");
        op = fetch(1);
        if (op == 0x66)
            o32 = !s[CS].big;
        else if (op == 0x67)
            a32 = !s[CS].big;
        else if (op == 0x26)
            over = ES;
        else if (op == 0x2e)
            over = CS;
        else if (op == 0x36)
            over = SS;
        else if (op == 0x3e)
            over = DS;
        else if (op == 0x64)
            over = FS;
        else if (op == 0x65)
            over = GS;
        else if (op == 0xf2 || op == 0xf3)
            rep = op;
        else if (op == 0xf0) {
        } else
            break;
    }
    int w = o32 ? 4 : 2, aw = a32 ? 4 : 2;
    profile::opcode(op, s[CS].selector, s[CS].base, cr[3], start);
    auto rel = [&](int32_t d) { ip = (ip + d) & (o32 ? 0xffffffff : 65535); };
    auto fail = [&]() {
        char b[160];
        snprintf(b, sizeof b, "unsupported opcode %02x at %04x:%08x", op, s[CS].selector, start);
        throw std::runtime_error(b);
    };
    instructions++;
    if (op < 0x40 && (op & 7) <= 5) {
        int group = op >> 3, form = op & 7, bw = (form & 1) ? w : 1;
        uint32_t a, b;
        Operand dst;
        if (form < 4) {
            uint8_t m = fetch(1);
            auto rm = decode(m, a32, over);
            Operand reg{true, uint8_t((m >> 3) & 7), 0};
            dst = (form & 2) ? reg : rm;
            auto src = (form & 2) ? rm : reg;
            a = load(dst, bw);
            b = load(src, bw);
        } else {
            dst = {true, AX, 0};
            a = get(AX, bw);
            b = fetch(bw);
        }
        auto v = alu(group, a, b, bw);
        if (group != 7)
            store(dst, v, bw);
        return;
    }
    if (op >= 0x40 && op <= 0x4f) {
        int reg = op & 7;
        auto c = flags & CF;
        set(reg, alu(op < 0x48 ? 0 : 5, get(reg, w), 1, w), w);
        flags = (flags & ~CF) | c;
        return;
    }
    if (op >= 0x50 && op <= 0x57) {
        push(get(op & 7, w), w);
        return;
    }
    if (op >= 0x58 && op <= 0x5f) {
        set(op & 7, pop(w), w);
        return;
    }
    if (op >= 0x70 && op <= 0x7f) {
        auto d = int8_t(fetch(1));
        if (condition(op & 15))
            rel(d);
        return;
    }
    if (op >= 0x90 && op <= 0x97) {
        auto v = get(op & 7, w);
        set(op & 7, get(AX, w), w);
        set(AX, v, w);
        return;
    }
    if (op >= 0xb0 && op <= 0xbf) {
        int bw = op < 0xb8 ? 1 : w;
        set(op & 7, fetch(bw), bw);
        return;
    }
    switch (op) {
    case 0x06:
    case 0x0e:
    case 0x16:
    case 0x1e:
        push(s[op >> 3].selector, w);
        break;
    case 0x07:
    case 0x17:
    case 0x1f:
        segment(op >> 3, pop(w));
        if (op == 0x17)
            interruptBlockedThrough = instructions;
        break;
    case 0x27:
    case 0x2f: {
        uint32_t al = get(AX, 1), old = al;
        bool c = flags & CF, a = flags & AF;
        if ((al & 15) > 9 || a) {
            al = (op == 0x27 ? al + 6 : al - 6) & 255;
            flags |= AF;
        } else
            flags &= ~AF;
        if (old > 0x99 || c) {
            al = (op == 0x27 ? al + 0x60 : al - 0x60) & 255;
            flags |= CF;
        } else
            flags &= ~CF;
        auto keep = flags & (CF | AF);
        alu(4, al, 255, 1);
        flags = (flags & ~(CF | AF)) | keep;
        set(AX, al, 1);
        break;
    }
    case 0x37:
    case 0x3f: {
        auto ax = get(AX, 2);
        if ((ax & 15) > 9 || (flags & AF)) {
            ax = op == 0x37 ? ax + 0x106 : ax - 0x106;
            flags |= CF | AF;
        } else
            flags &= ~(CF | AF);
        set(AX, ax & 0xff0f, 2);
        break;
    }
    case 0x60: {
        auto sp = get(SP, w);
        for (int i = 0; i < 8; i++)
            push(i == SP ? sp : get(i, w), w);
        break;
    }
    case 0x61:
        for (int i = 7; i >= 0; i--) {
            auto v = pop(w);
            if (i != SP)
                set(i, v, w);
        }
        break;
    case 0x63: {
        if (!(cr[0] & 1) || (flags & 0x20000))
            throw Fault{6, 0, false};
        auto m = fetch(1);
        auto dst = decode(m, a32, over);
        auto v = load(dst, 2), rpl = get((m >> 3) & 7, 2) & 3;
        bool changed = (v & 3) < rpl;
        if (changed)
            store(dst, (v & ~3u) | rpl, 2);
        flags = (flags & ~ZF) | (changed ? ZF : 0);
        break;
    }
    case 0x68:
        push(fetch(w), w);
        break;
    case 0x6a:
        push(int8_t(fetch(1)), w);
        break;
    case 0x69:
    case 0x6b: {
        auto m = fetch(1);
        auto src = decode(m, a32, over);
        int64_t a = signedValue(load(src, w), w),
                b = op == 0x6b ? int8_t(fetch(1)) : signedValue(fetch(w), w), v = a * b;
        set((m >> 3) & 7, v, w);
        bool ov = v != signedValue(v, w);
        flags = (flags & ~(CF | OF)) | (ov ? CF | OF : 0);
        break;
    }
    case 0x80:
    case 0x81:
    case 0x82:
    case 0x83: {
        auto m = fetch(1);
        auto dst = decode(m, a32, over);
        int bw = (op == 0x80 || op == 0x82) ? 1 : w, group = (m >> 3) & 7;
        auto b = op == 0x83 ? uint32_t(int8_t(fetch(1))) : fetch(bw),
             v = alu(group, load(dst, bw), b, bw);
        if (group != 7)
            store(dst, v, bw);
        break;
    }
    case 0x84:
    case 0x85: {
        auto m = fetch(1);
        auto dst = decode(m, a32, over);
        int bw = op & 1 ? w : 1;
        alu(4, load(dst, bw), get((m >> 3) & 7, bw), bw);
        break;
    }
    case 0x86:
    case 0x87: {
        auto m = fetch(1);
        auto dst = decode(m, a32, over);
        int bw = op & 1 ? w : 1, reg = (m >> 3) & 7;
        auto v = load(dst, bw);
        store(dst, get(reg, bw), bw);
        set(reg, v, bw);
        break;
    }
    case 0x88:
    case 0x89:
    case 0x8a:
    case 0x8b: {
        auto m = fetch(1);
        auto rm = decode(m, a32, over);
        int bw = op & 1 ? w : 1, reg = (m >> 3) & 7;
        if (op & 2)
            set(reg, load(rm, bw), bw);
        else
            store(rm, get(reg, bw), bw);
        break;
    }
    case 0x8c:
    case 0x8e: {
        auto m = fetch(1);
        auto rm = decode(m, a32, over);
        int seg = (m >> 3) & 7;
        if (seg >= 6)
            fail();
        if (op == 0x8c)
            store(rm, s[seg].selector, rm.reg ? w : 2);
        else {
            segment(seg, load(rm, 2));
            if (seg == SS)
                interruptBlockedThrough = instructions;
        }
        break;
    }
    case 0x8d: {
        auto m = fetch(1);
        auto rm = decode(m, a32, -2);
        if (rm.reg)
            fail();
        set((m >> 3) & 7, rm.linear, w);
        break;
    }
    case 0x8f: {
        auto m = fetch(1);
        if ((m & 0x38) != 0)
            fail();
        // ESP-based destinations use the post-pop stack pointer. In particular,
        // Win95's 16/32-bit thunks use POP dword [ESP+8] to reshape far returns.
        auto value = pop(w);
        auto rm = decode(m, a32, over);
        store(rm, value, w);
        break;
    }
    case 0x98:
        set(AX, o32 ? uint32_t(int32_t(int16_t(r[AX]))) : uint32_t(int16_t(int8_t(r[AX]))), w);
        break;
    case 0x99:
        set(DX, signedValue(get(AX, w), w) < 0 ? mask(w) : 0, w);
        break;
    case 0x9a: {
        auto off = fetch(w);
        auto seg = fetch(2);
        farTransfer(seg, off, true, w);
        break;
    }
    case 0x9b:
        break;
    case 0x9c:
        if ((flags & 0x20000) && ((flags >> 12) & 3) != 3)
            throw Fault{13, 0, true};
        push(flags & (o32 ? ~0x30000u : 0xffffu), w);
        break;
    case 0x9d: {
        if ((flags & 0x20000) && ((flags >> 12) & 3) != 3)
            throw Fault{13, 0, true};
        auto f = pop(w);
        uint32_t change = o32 ? 0x00007fd7 : 0x7fd7;
        if ((cr[0] & 1) && cpl() != 0)
            change &= ~0x3000u;
        if ((cr[0] & 1) && cpl() > ((flags >> 12) & 3))
            change &= ~uint32_t(IF);
        flags = (flags & ~change) | (f & change) | 2;
        break;
    }
    case 0x9e:
        flags = (flags & ~0xd5) | (get(4, 1) & 0xd5) | 2;
        break;
    case 0x9f:
        set(4, flags, 1);
        break;
    case 0xa0:
    case 0xa1:
    case 0xa2:
    case 0xa3: {
        auto off = fetch(aw);
        int bw = op & 1 ? w : 1;
        auto addr = address(over < 0 ? DS : over, off);
        if (op & 2)
            write(addr, get(AX, bw), bw);
        else
            set(AX, read(addr, bw), bw);
        break;
    }
    case 0xa8:
    case 0xa9: {
        int bw = op & 1 ? w : 1;
        alu(4, get(AX, bw), fetch(bw), bw);
        break;
    }
    case 0x6c:
    case 0x6d:
    case 0x6e:
    case 0x6f:
    case 0xa4:
    case 0xa5:
    case 0xa6:
    case 0xa7:
    case 0xaa:
    case 0xab:
    case 0xac:
    case 0xad:
    case 0xae:
    case 0xaf: {
        int bw = op & 1 ? w : 1;
        if (op >= 0x6c && op <= 0x6f)
            ioPermission(get(DX, 2), bw);
        uint32_t count = rep ? get(CX, aw) : 1, delta = (flags & DF) ? -bw : bw;
        // REP is interruptible and bounded so UI/device scheduling remains live.
        uint32_t batch = count > 4096 ? 4096 : count;
        bool done = false;
        auto initialFlags = flags;
        if (rep && repeatResume.valid && repeatResume.selector == s[CS].selector &&
            repeatResume.base == s[CS].base && repeatResume.ip == start &&
            repeatResume.lastFlags == flags && repeatResume.ax == r[AX] &&
            repeatResume.cx == r[CX] && repeatResume.si == r[SI] && repeatResume.di == r[DI])
            initialFlags = repeatResume.initialFlags;
        repeatResume.valid = false;
        try {
            while (batch--) {
                auto si = get(SI, aw), di = get(DI, aw);
                unsigned kind = op & 0xfe;
                auto src = (kind == 0x6e || kind == 0xa4 || kind == 0xa6 || kind == 0xac)
                               ? address(over < 0 ? DS : over, si)
                               : 0;
                auto dst =
                    (kind == 0x6c || kind == 0xa4 || kind == 0xa6 || kind == 0xaa || kind == 0xae)
                        ? address(ES, di)
                        : 0;
                // Repeated forward fills can reuse a validated translation until
                // the next linear page. Never speculate a page walk or a write:
                // the scalar path retains first-fault and dirty-bit ordering.
                if (rep && kind == 0xaa && !(flags & DF) && a20 && !watchWrite) {
                    uint32_t elements = std::min(batch + 1, (4096u - (dst & 4095)) / bw);
                    elements = std::min(elements, uint32_t((uint64_t(mask(aw)) + 1 - di) / bw));
                    uint32_t physicalStart = dst;
                    bool ready = true;
                    if (cr[0] & 0x80000000u) {
                        uint32_t page = dst >> 12;
                        const auto &entry = tlb[(page ^ (page >> 10)) & (tlb.size() - 1)];
                        ready = tlbCR3 == cr[3] && entry.epoch == tlbEpoch &&
                                entry.tag == page && entry.dirty &&
                                (cpl() != 3 || systemAccess || (entry.permissions & 6) == 6);
                        physicalStart = entry.base | (dst & 4095);
                    }
                    uint32_t bytes = elements * bw;
                    ready = ready && elements > 1 && uint64_t(physicalStart) + bytes <= mem.size() &&
                            !(physicalStart < 0xb0000 && uint64_t(physicalStart) + bytes > 0xa0000);
                    if (ready) {
                        for (uint32_t word = physicalStart >> 6;
                             word <= (physicalStart + bytes - 1) >> 6; ++word)
                            if (codeUsed[word]) {
                                ready = false;
                                break;
                            }
                    }
                    if (ready) {
                        auto value = get(AX, bw);
                        if (bw == 1)
                            std::memset(&mem[physicalStart], value, bytes);
                        else
                            for (uint32_t offset = 0; offset < bytes; offset += bw)
                                storeRAM(&mem[physicalStart + offset], value, bw);
                        set(DI, di + bytes, aw);
                        count -= elements;
                        set(CX, count, aw);
                        batch -= elements - 1;
                        continue;
                    }
                }
                // Reuse translations only within the current linear page.
                // Never speculate a page walk or dirty-bit update: scalar
                // execution retains first-fault and partial-repeat ordering.
                if (rep && kind == 0xa4 && !(flags & DF) && a20 && !watchWrite) {
                    uint32_t elements = std::min(batch + 1, (4096u - (dst & 4095)) / bw);
                    elements = std::min(elements, uint32_t((uint64_t(mask(aw)) + 1 - di) / bw));
                    elements = std::min(elements, (4096u - (src & 4095)) / bw);
                    elements = std::min(elements, uint32_t((uint64_t(mask(aw)) + 1 - si) / bw));
                    uint32_t bytes = elements * bw;
                    auto ramSpan = [&](uint32_t linear, bool write, uint32_t &physicalStart) {
                        physicalStart = linear;
                        if (cr[0] & 0x80000000u) {
                            uint32_t page = linear >> 12;
                            const auto &entry = tlb[(page ^ (page >> 10)) & (tlb.size() - 1)];
                            unsigned permissions = write ? 6 : 4;
                            if (tlbCR3 != cr[3] || entry.epoch != tlbEpoch || entry.tag != page ||
                                (write && !entry.dirty) ||
                                (cpl() == 3 && !systemAccess &&
                                 (entry.permissions & permissions) != permissions))
                                return false;
                            physicalStart = entry.base | (linear & 4095);
                        }
                        if (uint64_t(physicalStart) + bytes > mem.size() ||
                            (physicalStart < 0xb0000 && uint64_t(physicalStart) + bytes > 0xa0000))
                            return false;
                        if (write)
                            for (uint32_t word = physicalStart >> 6;
                                 word <= (physicalStart + bytes - 1) >> 6; ++word)
                                if (codeUsed[word])
                                    return false;
                        return true;
                    };
                    uint32_t destination = 0, source = 0;
                    bool ready = elements > 1 && ramSpan(dst, true, destination);
                    if (ready)
                        // memcpy is valid only for disjoint physical ranges.
                        // Overlapping MOVS can propagate values; memmove would
                        // silently give different architectural results.
                        ready = ramSpan(src, false, source) &&
                                (uint64_t(source) + bytes <= destination ||
                                 uint64_t(destination) + bytes <= source);
                    if (ready) {
                        std::memcpy(&mem[destination], &mem[source], bytes);
                        set(SI, si + bytes, aw);
                        set(DI, di + bytes, aw);
                        count -= elements;
                        set(CX, count, aw);
                        batch -= elements - 1;
                        continue;
                    }
                }
                switch (op & 0xfe) {
                case 0x6c:
                    write(dst, input ? input(get(DX, 2), bw) : mask(bw), bw);
                    set(DI, di + delta, aw);
                    break;
                case 0x6e:
                    if (output)
                        output(get(DX, 2), read(src, bw), bw);
                    set(SI, si + delta, aw);
                    break;
                case 0xa4:
                    write(dst, read(src, bw), bw);
                    set(SI, si + delta, aw);
                    set(DI, di + delta, aw);
                    break;
                case 0xa6:
                    alu(7, read(src, bw), read(dst, bw), bw);
                    set(SI, si + delta, aw);
                    set(DI, di + delta, aw);
                    break;
                case 0xaa:
                    write(dst, get(AX, bw), bw);
                    set(DI, di + delta, aw);
                    break;
                case 0xac:
                    set(AX, read(src, bw), bw);
                    set(SI, si + delta, aw);
                    break;
                case 0xae:
                    alu(7, get(AX, bw), read(dst, bw), bw);
                    set(DI, di + delta, aw);
                    break;
                }
                --count;
                if (rep)
                    set(CX, count, aw);
                if (rep && ((op & 0xfe) == 0xa6 || (op & 0xfe) == 0xae) &&
                    bool(flags & ZF) != (rep == 0xf3)) {
                    done = true;
                    break;
                }
            }
        } catch (Fault fault) {
            // REP faults restart at the prefixed instruction, but SI/DI/CX
            // (and LODS' accumulator) retain all completed elements. EFLAGS
            // are still restored by step(), including repeated CMPS/SCAS.
            fault.partialRepeat = bool(rep);
            fault.repeatFlags = initialFlags;
            throw fault;
        }
        if (rep && count && !done) {
            ip = start;
            // A host scheduling yield is not an architectural interruption.
            // Keep the original comparison flags across multiple batches.
            repeatResume = {true,  s[CS].selector, s[CS].base, start, initialFlags,
                            flags, r[AX],          r[CX],      r[SI], r[DI]};
        }
        break;
    }
    case 0xc0:
    case 0xc1:
    case 0xd0:
    case 0xd1:
    case 0xd2:
    case 0xd3: {
        auto m = fetch(1);
        auto dst = decode(m, a32, over);
        int bw = op & 1 ? w : 1;
        auto n = op < 0xd0 ? fetch(1) : op < 0xd2 ? 1 : get(CX, 1);
        store(dst, shift((m >> 3) & 7, load(dst, bw), n, bw), bw);
        break;
    }
    case 0xc2:
    case 0xc3: {
        auto extra = op == 0xc2 ? fetch(2) : 0;
        ip = pop(w);
        set(SP, get(SP, s[SS].big ? 4 : 2) + extra, s[SS].big ? 4 : 2);
        break;
    }
    case 0xc4:
    case 0xc5: {
        auto m = fetch(1);
        auto src = decode(m, a32, over);
        if (src.reg)
            fail();
        auto val = read(src.linear, w), sel = read(src.linear + w, 2);
        segment(op == 0xc4 ? ES : DS, sel);
        set((m >> 3) & 7, val, w);
        break;
    }
    case 0xc6:
    case 0xc7: {
        auto m = fetch(1);
        auto dst = decode(m, a32, over);
        if (m & 0x38)
            fail();
        int bw = op & 1 ? w : 1;
        store(dst, fetch(bw), bw);
        break;
    }
    case 0xc8: {
        auto size = fetch(2), level = fetch(1) & 31;
        push(get(BP, w), w);
        auto frame = get(SP, s[SS].big ? 4 : 2);
        for (unsigned i = 1; i < level; i++) {
            set(BP, get(BP, w) - w, w);
            push(read(address(SS, get(BP, w)), w), w);
        }
        if (level)
            push(frame, w);
        set(BP, frame, w);
        set(SP, get(SP, s[SS].big ? 4 : 2) - size, s[SS].big ? 4 : 2);
        break;
    }
    case 0xc9:
        set(SP, get(BP, s[SS].big ? 4 : 2), s[SS].big ? 4 : 2);
        set(BP, pop(w), w);
        break;
    case 0xca:
    case 0xcb: {
        auto extra = op == 0xca ? fetch(2) : 0;
        farReturn(w, extra, false);
        break;
    }
    case 0xcc:
        interrupt(3, true);
        break;
    case 0xcd: {
        auto n = fetch(1);
        interrupt(n, true);
        break;
    }
    case 0xce:
        if (flags & OF)
            interrupt(4, true);
        break;
    case 0xcf: {
        farReturn(w, 0, true);
        break;
    }
    case 0xd4: {
        auto b = fetch(1);
        if (!b)
            throw std::runtime_error("divide by zero");
        auto a = get(AX, 1);
        set(4, a / b, 1);
        set(AX, a % b, 1);
        auto h = get(4, 1);
        alu(4, get(AX, 1), 255, 1);
        set(4, h, 1);
        break;
    }
    case 0xd5: {
        auto b = fetch(1);
        auto v = (get(4, 1) * b + get(AX, 1)) & 255;
        set(AX, v, 2);
        alu(4, v, 255, 1);
        break;
    }
    case 0xd6:
        set(AX, (flags & CF) ? 255 : 0, 1);
        break;
    case 0xd7:
        set(AX, read(address(over < 0 ? DS : over, (get(BX, aw) + get(AX, 1)) & mask(aw)), 1), 1);
        break;
    case 0xd8:
    case 0xd9:
    case 0xda:
    case 0xdb:
    case 0xdc:
    case 0xdd:
    case 0xde:
    case 0xdf: {
        // This machine has no 80387 attached. With EM/TS clear the ESC bus
        // cycle completes without an FPU response or register/memory write.
        // Hardware-presence probes test an unchanged status-word sentinel.
        if (cr[0] & 12)
            throw Fault{7, 0, false};
        auto m = fetch(1);
        decode(m, a32, over);
        break;
    }
    case 0xe0:
    case 0xe1:
    case 0xe2:
    case 0xe3: {
        auto d = int8_t(fetch(1));
        bool take;
        if (op == 0xe3)
            take = get(CX, aw) == 0;
        else {
            set(CX, get(CX, aw) - 1, aw);
            take = get(CX, aw) != 0;
            if (op != 0xe2)
                take &= bool(flags & ZF) == (op == 0xe1);
        }
        if (take)
            rel(d);
        break;
    }
    case 0xe4:
    case 0xe5:
    case 0xe6:
    case 0xe7:
    case 0xec:
    case 0xed:
    case 0xee:
    case 0xef: {
        auto port = op < 0xe8 ? fetch(1) : get(DX, 2);
        int bw = op & 1 ? w : 1;
        ioPermission(port, bw);
        if (op & 2) {
            if (output)
                output(port, get(AX, bw), bw);
        } else
            set(AX, input ? input(port, bw) : mask(bw), bw);
        break;
    }
    case 0xe8: {
        auto d = signedValue(fetch(w), w);
        push(ip, w);
        rel(d);
        break;
    }
    case 0xe9: {
        auto d = signedValue(fetch(w), w);
        rel(d);
        break;
    }
    case 0xea: {
        auto off = fetch(w), seg = fetch(2);
        farTransfer(seg, off, false, w);
        break;
    }
    case 0xeb: {
        auto d = int8_t(fetch(1));
        rel(d);
        break;
    }
    case 0xf1:
        interrupt(1);
        break;
    case 0xf4:
        if (cpl() != 0)
            throw Fault{13, 0, true};
        halted = true;
        break;
    case 0xf5:
        flags ^= CF;
        break;
    case 0xf6:
    case 0xf7: {
        auto m = fetch(1);
        auto dst = decode(m, a32, over);
        int bw = op & 1 ? w : 1, g = (m >> 3) & 7;
        auto v = load(dst, bw), a = get(AX, bw);
        if (g < 2)
            alu(4, v, fetch(bw), bw);
        else if (g == 2)
            store(dst, ~v, bw);
        else if (g == 3)
            store(dst, alu(5, 0, v, bw), bw);
        else if (g == 4 || g == 5) {
            uint64_t p = g == 4 ? uint64_t(a) * v
                                : uint64_t(int64_t(signedValue(a, bw)) * signedValue(v, bw));
            if (bw == 1)
                set(AX, p, 2);
            else {
                set(AX, p, bw);
                set(DX, p >> (bw * 8), bw);
            }
            bool c = g == 4 ? (p >> (bw * 8)) != 0 : int64_t(p) != signedValue(p, bw);
            flags = (flags & ~(CF | OF)) | (c ? CF | OF : 0);
        } else {
            if (!v)
                throw std::runtime_error("divide by zero at " + location());
            uint64_t dividend = bw == 1 ? get(AX, 2) : (uint64_t(get(DX, bw)) << (bw * 8)) | a;
            if (g == 6) {
                auto q = dividend / v;
                if (q > mask(bw))
                    throw std::runtime_error("divide overflow at " + location());
                auto rem = dividend % v;
                set(AX, q, bw);
                set(bw == 1 ? 4 : DX, rem, bw == 1 ? 1 : bw);
            } else {
                int64_t num = bw == 1   ? int16_t(dividend)
                              : bw == 2 ? int32_t(dividend)
                                        : int64_t(dividend),
                        den = signedValue(v, bw);
                if (num == INT64_MIN && den == -1)
                    throw std::runtime_error("divide overflow");
                int64_t q = num / den, rem = num % den;
                if (q != signedValue(q, bw))
                    throw std::runtime_error("divide overflow at " + location());
                set(AX, q, bw);
                set(bw == 1 ? 4 : DX, rem, bw == 1 ? 1 : bw);
            }
        }
        break;
    }
    case 0xf8:
        flags &= ~CF;
        break;
    case 0xf9:
        flags |= CF;
        break;
    case 0xfa:
        if (cpl() > ((flags >> 12) & 3))
            throw Fault{13, 0, true};
        flags &= ~IF;
        break;
    case 0xfb:
        if (cpl() > ((flags >> 12) & 3))
            throw Fault{13, 0, true};
        flags |= IF;
        interruptBlockedThrough = instructions;
        break;
    case 0xfc:
        flags &= ~DF;
        break;
    case 0xfd:
        flags |= DF;
        break;
    case 0xfe:
    case 0xff: {
        auto m = fetch(1);
        auto dst = decode(m, a32, over);
        int bw = op == 0xfe ? 1 : w, g = (m >> 3) & 7;
        auto v = load(dst, bw);
        if (g < 2) {
            auto c = flags & CF;
            store(dst, alu(g ? 5 : 0, v, 1, bw), bw);
            flags = (flags & ~CF) | c;
        } else if (op == 0xff) {
            if (g == 2) {
                push(ip, w);
                ip = v;
            } else if (g == 3 || g == 5) {
                if (dst.reg)
                    fail();
                auto seg = read(dst.linear + w, 2);
                farTransfer(seg, v, g == 3, w);
            } else if (g == 4)
                ip = v;
            else if (g == 6)
                push(v, w);
            else
                fail();
        } else
            fail();
        break;
    }
    case 0x0f: {
        uint8_t op2 = fetch(1);
        profile::extended(op2);
        if (op2 >= 0x80 && op2 <= 0x8f) {
            auto d = signedValue(fetch(w), w);
            if (condition(op2 & 15))
                rel(d);
            break;
        }
        if (op2 >= 0x90 && op2 <= 0x9f) {
            auto m = fetch(1);
            auto dst = decode(m, a32, over);
            store(dst, condition(op2 & 15), 1);
            break;
        }
        switch (op2) {
        case 0x00: {
            if (!(cr[0] & 1) || (flags & 0x20000))
                throw Fault{6, 0, false};
            auto m = fetch(1);
            auto dst = decode(m, a32, over);
            int g = (m >> 3) & 7;
            if ((g == 2 || g == 3) && cpl() != 0)
                throw Fault{13, 0, true};
            if (g < 2) {
                store(dst, g ? trSelector : ldtSelector, 2);
                break;
            }
            auto sel = load(dst, 2);
            if (g == 2 || g == 3) {
                auto off = gdtBase + (sel & ~7);
                auto lo = read(off, 4), hi = read(off + 4, 4);
                auto base = (lo >> 16) | ((hi & 255) << 16) | (hi & 0xff000000),
                     limit = (lo & 65535) | (hi & 0xf0000);
                if (hi & 0x800000)
                    limit = (limit << 12) | 4095;
                if (g == 2) {
                    ldtSelector = sel;
                    ldtBase = base;
                    ldtLimit = limit;
                } else {
                    trSelector = sel;
                    trBase = base;
                    trLimit = limit;
                    tr32 = hi & 0x800;
                    write(off + 4, hi | 0x200, 4);
                }
            } else if (g == 4 || g == 5) {
                auto lim = (sel & 4) ? ldtLimit : gdtLimit;
                bool valid = (sel & ~3u) && ((sel & ~7u) + 7 <= lim);
                if (valid) {
                    auto base = (sel & 4) ? ldtBase : gdtBase;
                    auto hi = systemRead(base + (sel & ~7u) + 4, 4);
                    unsigned type = (hi >> 8) & 15;
                    valid = (hi & 0x1000) && descriptorVisible(sel, hi) &&
                            (g == 4 ? (!(type & 8) || (type & 2)) : (!(type & 8) && (type & 2)));
                }
                flags = (flags & ~ZF) | (valid ? ZF : 0);
            } else
                fail();
            break;
        }
        case 0x02:
        case 0x03: {
            if (!(cr[0] & 1) || (flags & 0x20000))
                throw Fault{6, 0, false};
            auto m = fetch(1);
            auto src = decode(m, a32, over);
            auto sel = load(src, 2), off = sel & ~7u;
            auto limit = (sel & 4) ? ldtLimit : gdtLimit;
            bool valid = (sel & ~3u) && off + 7 <= limit;
            uint32_t lo = 0, hi = 0;
            if (valid) {
                auto base = (sel & 4) ? ldtBase : gdtBase;
                lo = systemRead(base + off, 4);
                hi = systemRead(base + off + 4, 4);
                unsigned type = (hi >> 8) & 15;
                bool systemType =
                    op2 == 2 ? (type != 0 && type != 8 && type != 10 && type != 13)
                             : (type == 1 || type == 2 || type == 3 || type == 9 || type == 11);
                valid = descriptorVisible(sel, hi) && ((hi & 0x1000) || systemType);
            }
            flags = (flags & ~ZF) | (valid ? ZF : 0);
            if (valid) {
                uint32_t v = op2 == 2 ? hi & 0x00f0ff00u : (lo & 65535) | (hi & 0xf0000);
                if (op2 == 3 && (hi & 0x800000))
                    v = (v << 12) | 4095;
                set((m >> 3) & 7, v, w);
            }
            break;
        }
        case 0x01: {
            auto m = fetch(1);
            auto dst = decode(m, a32, over);
            int g = (m >> 3) & 7;
            if ((g == 2 || g == 3 || g == 6 || g == 7) && cpl() != 0)
                throw Fault{13, 0, true};
            if (g < 4) {
                if (dst.reg)
                    fail();
                if (g < 2) {
                    write(dst.linear, g ? idtLimit : gdtLimit, 2);
                    write(dst.linear + 2, g ? idtBase : gdtBase, 4);
                } else {
                    auto lim = read(dst.linear, 2),
                         base = read(dst.linear + 2, 4) & (o32 ? 0xffffffff : 0xffffff);
                    if (g == 2) {
                        gdtBase = base;
                        gdtLimit = lim;
                    } else {
                        idtBase = base;
                        idtLimit = lim;
                    }
                }
            } else if (g == 4)
                store(dst, cr[0], 2);
            else if (g == 6)
                cr[0] = (cr[0] & ~15u) | (load(dst, 2) & 15) | (cr[0] & 1);
            else if (g == 7)
                flushTLB();
            else
                fail();
            break;
        }
        // The advertised CPU is a 386. Windows probes these later/undefined
        // encodings with an installed #UD handler during CPU detection.
        case 0x0b:
        case 0xa2:
        case 0xa6:
        case 0xa7:
        case 0xff:
            throw Fault{6, 0, false};
        case 0x06:
            if (cpl() != 0)
                throw Fault{13, 0, true};
            cr[0] &= ~8u;
            break;
        case 0x21:
        case 0x23: {
            if (cpl() != 0)
                throw Fault{13, 0, true};
            auto m = fetch(1);
            if ((m >> 6) != 3)
                throw Fault{6, 0, false};
            unsigned reg = (m >> 3) & 7;
            if (reg == 4)
                reg = 6;
            if (reg == 5)
                reg = 7;
            if (op2 == 0x21)
                r[m & 7] = dr[reg];
            else
                dr[reg] = r[m & 7];
            break;
        }
        case 0x20:
        case 0x22: {
            if (cpl() != 0)
                throw Fault{13, 0, true};
            auto m = fetch(1);
            if ((m >> 6) != 3 || ((m >> 3) & 7) > 3)
                fail();
            if (op2 == 0x20)
                r[m & 7] = cr[(m >> 3) & 7];
            else {
                auto index = (m >> 3) & 7;
                cr[index] = r[m & 7];
                if (index == 0 || index == 3)
                    flushTLB();
            }
            break;
        }
        case 0xa0:
            push(s[FS].selector, w);
            break;
        case 0xa1:
            segment(FS, pop(w));
            break;
        case 0xa8:
            push(s[GS].selector, w);
            break;
        case 0xa9:
            segment(GS, pop(w));
            break;
        case 0xa3:
        case 0xab:
        case 0xb3:
        case 0xbb:
        case 0xba: {
            auto m = fetch(1);
            auto dst = decode(m, a32, over);
            int kind = op2 == 0xba   ? (m >> 3) & 7
                       : op2 == 0xa3 ? 4
                       : op2 == 0xab ? 5
                       : op2 == 0xb3 ? 6
                                     : 7;
            if (kind < 4)
                fail();
            int32_t index = op2 == 0xba ? int32_t(fetch(1)) : signedValue(get((m >> 3) & 7, w), w);
            unsigned bit = uint32_t(index) & (w * 8 - 1);
            if (!dst.reg && op2 != 0xba) {
                int32_t word = index >= 0
                                   ? index / (w * 8)
                                   : -int32_t((uint32_t(-int64_t(index)) + w * 8 - 1) / (w * 8));
                dst.linear += word * w;
            }
            auto value = load(dst, w);
            flags = (flags & ~CF) | ((value & (1u << bit)) ? CF : 0);
            if (kind == 5)
                store(dst, value | (1u << bit), w);
            else if (kind == 6)
                store(dst, value & ~(1u << bit), w);
            else if (kind == 7)
                store(dst, value ^ (1u << bit), w);
            break;
        }
        case 0xa4:
        case 0xa5:
        case 0xac:
        case 0xad: {
            auto m = fetch(1);
            auto dst = decode(m, a32, over);
            unsigned count = (op2 & 1 ? get(CX, 1) : fetch(1)) & 31;
            if (!count)
                break;
            auto value = load(dst, w), src = get((m >> 3) & 7, w);
            unsigned bits = w * 8;
            uint64_t pair =
                (op2 & 8) ? (uint64_t(src) << bits) | value : (uint64_t(value) << bits) | src;
            uint32_t result;
            bool carry;
            if (op2 & 8) {
                result = (pair >> count) & mask(w);
                carry = (pair >> (count - 1)) & 1;
            } else {
                result = (pair << count >> bits) & mask(w);
                carry = (pair >> (bits * 2 - count)) & 1;
            }
            auto old = value;
            alu(4, result, mask(w), w);
            flags = (flags & ~CF) | (carry ? CF : 0);
            if (count == 1)
                flags = (flags & ~OF) | (((old ^ result) & (1u << (bits - 1))) ? OF : 0);
            store(dst, result, w);
            break;
        }
        case 0xaf: {
            auto m = fetch(1);
            auto src = decode(m, a32, over);
            int reg = (m >> 3) & 7;
            int64_t v = int64_t(signedValue(get(reg, w), w)) * signedValue(load(src, w), w);
            set(reg, v, w);
            bool ov = v != signedValue(v, w);
            flags = (flags & ~(CF | OF)) | (ov ? CF | OF : 0);
            break;
        }
        case 0xb2:
        case 0xb4:
        case 0xb5: {
            auto m = fetch(1);
            auto src = decode(m, a32, over);
            if (src.reg)
                fail();
            auto v = read(src.linear, w), sel = read(src.linear + w, 2);
            segment(op2 == 0xb2 ? SS : op2 == 0xb4 ? FS : GS, sel);
            set((m >> 3) & 7, v, w);
            break;
        }
        case 0xb6:
        case 0xb7:
        case 0xbe:
        case 0xbf: {
            auto m = fetch(1);
            auto src = decode(m, a32, over);
            int bw = op2 & 1 ? 2 : 1;
            auto v = load(src, bw);
            set((m >> 3) & 7, op2 & 8 ? uint32_t(signedValue(v, bw)) : v, w);
            break;
        }
        case 0xbc:
        case 0xbd: {
            auto m = fetch(1);
            auto src = decode(m, a32, over);
            auto v = load(src, w);
            if (!v)
                flags |= ZF;
            else {
                flags &= ~ZF;
                set((m >> 3) & 7, op2 == 0xbc ? __builtin_ctz(v) : 31 - __builtin_clz(v), w);
            }
            break;
        }
        default: {
            char b[140];
            snprintf(b, sizeof b, "unsupported opcode 0f %02x at %04x:%08x", op2, s[CS].selector,
                     start);
            throw std::runtime_error(b);
        }
        }
        break;
    }
    default:
        fail();
    }
}
} // namespace pc
