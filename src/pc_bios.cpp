#include "machine.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
namespace pc {
bool Machine::bios(uint8_t n) {
    if (n == 0xfe && cpu.s[CS].selector == 0xf000 && cpu.ip == 2) {
        resetRequested = true;
        return true;
    }
    if (verbose && n == 0x21 &&
        (cpu.get(4, 1) == 0x4b || cpu.get(4, 1) == 0x3d || cpu.get(4, 1) == 0x3c)) {
        std::string name;
        auto ptr = cpu.address(DS, cpu.get(DX, 2));
        for (unsigned i = 0; i < 128; i++) {
            auto ch = cpu.read(ptr + i, 1);
            if (!ch)
                break;
            name += char(ch);
        }
        std::fprintf(stderr, "DOS AH=%02x file=%s at %s\n", cpu.get(4, 1), name.c_str(),
                     cpu.location().c_str());
    }

    if (n == 0xfe && cpu.s[CS].selector == 0xf000 &&
        ((cpu.ip >= 0x1002 && cpu.ip < 0x1400) || cpu.ip == 0x2002 || cpu.ip == 0x2025 ||
         cpu.ip == 0x2047 || cpu.ip == 0x20a2)) {
        uint8_t v = cpu.ip == 0x2002   ? 8
                    : cpu.ip == 0x2025 ? 9
                    : cpu.ip == 0x2047 ? 0x74
                    : cpu.ip == 0x20a2 ? 0x15
                                       : (cpu.ip - 0x1002) / 4;
        auto addr = cpu.address(SS, cpu.get(SP, cpu.s[SS].big ? 4 : 2) + (v == 9      ? 6
                                                                          : v == 0x74 ? 24
                                                                                      : 4));
        auto saved = cpu.read(addr, 2);
        // A caller may PUSHF, modify its working flags, then CALL FAR through a
        // saved BIOS vector. Return status belongs to that saved flags image;
        // comparing working flags before/after the service loses explicit
        // assignments that happened to match the incoming working value.
        cpu.flags = (cpu.flags & ~(CF | ZF)) | (saved & (CF | ZF));
        service(v, true);
        // Bootstrap does not return to this ROM stub or its caller's frame.
        if (v == 0x19)
            return true;
        cpu.write(addr, (saved & ~(CF | ZF)) | (cpu.flags & (CF | ZF)), 2);
        return true;
    }
    if (n == 0x74)
        return false; // IRQ12 must execute the far-callback ROM stub.
    // Honor guest hooks (DOS installs INT 21h, keyboard, disk, etc.).
    if (cpu.read(n * 4, 4) != (0xf0001000u + n * 4))
        return false;
    return service(n);
}
bool Machine::service(uint8_t n, bool guestStub) {
    auto ah = cpu.get(4, 1), al = cpu.get(AX, 1);
    auto &c = cpu;
    auto ok = [&]() {
        c.flags &= ~CF;
        c.set(4, 0, 1);
    };
    auto error = [&](uint8_t e) {
        c.flags |= CF;
        c.set(4, e, 1);
    };
    if (verbose && n != 0x10 && n != 0x16 && n != 0x1a && n != 8 && n != 0x28)
        std::fprintf(stderr, "BIOS %02x AX=%04x BX=%04x CX=%04x DX=%04x ES=%04x\n", n, c.get(AX, 2),
                     c.get(BX, 2), c.get(CX, 2), c.get(DX, 2), c.s[ES].selector);
    switch (n) {
    case 8:
        ticks++;
        cpu.write(0x46c, ticks, 4);
        if (!guestStub)
            out(0x20, 0x20, 1);
        if (cpu.read(0x1c * 4, 4) != 0xf0001070)
            cpu.interrupt(0x1c);
        break;
    case 9:
        // The firmware IRQ handler must complete the controller/PIC handshake,
        // including when a protected-mode keyboard driver chains to this vector.
        if ((guestStub && c.get(AX, 1)) || (!guestStub && ps2.state().keyboardFull)) {
            auto scan = guestStub ? c.get(AX, 1) : in(0x60, 1);
            if (biosPauseBytes) {
                --biosPauseBytes;
                if (!guestStub)
                    out(0x20, 0x20, 1);
                break;
            }
            if (scan == 0xe1) {
                biosPauseBytes = 5;
                if (!guestStub)
                    out(0x20, 0x20, 1);
                break;
            }
            if (scan == 0xe0) {
                biosKeyPrefix = scan;
                if (!guestStub)
                    out(0x20, 0x20, 1);
                break;
            }
            bool extended = biosKeyPrefix != 0;
            biosKeyPrefix = 0;
            if (extended && ((scan & 127) == 42 || (scan & 127) == 54)) {
                if (!guestStub)
                    out(0x20, 0x20, 1);
                break;
            }
            auto shifts = c.read(0x417, 1);
            unsigned code = scan & 127;
            uint8_t bit = code == 42 ? 2 : code == 54 ? 1 : code == 29 ? 4 : code == 56 ? 8 : 0;
            if (bit) {
                shifts = (scan & 128) ? shifts & ~bit : shifts | bit;
                c.write(0x417, shifts, 1);
            } else if (code == 58 && !(scan & 128))
                c.write(0x417, shifts ^ 64, 1);
            else if (!(scan & 128)) {
                static const char ascii[] = {
                    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8,
                    9,   'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13,  0,
                    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', 39,  '`', 0,   92,  'z',
                    'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' '};
                unsigned ch = code < sizeof(ascii) ? uint8_t(ascii[code]) : 0;
                if (ch >= 'a' && ch <= 'z' && bool(shifts & 3) != bool(shifts & 64))
                    ch -= 32;
                else if (shifts & 3) {
                    const char *plain = "1234567890-=[];'`\\,./";
                    const char *shifted = "!@#$%^&*()_+{}:\"~|<>?";
                    if (ch)
                        if (auto found = std::strchr(plain, ch))
                            ch = uint8_t(shifted[found - plain]);
                }
                if ((shifts & 4) && ch >= 'a' && ch <= 'z')
                    ch -= 96;
                keys.push_back((code << 8) | ch);
            }
        }
        if (!guestStub)
            out(0x20, 0x20, 1);
        break;
    case 0x74: {
        uint8_t byte = guestStub ? c.get(AX, 1) : in(0x60, 1);
        c.set(AX, 0, 1);
        if (biosMouseCount || (byte & 8)) {
            biosMousePacket[biosMouseCount++] = byte;
            if (biosMouseCount == 3) {
                biosMouseCount = 0;
                if (ps2.mouse().enabled && c.read(0xf0320, 4)) {
                    for (unsigned i = 0; i < 3; ++i)
                        c.write(0xf0324 + i * 2, biosMousePacket[i], 2);
                    c.set(AX, 1, 1);
                }
            }
        }
        if (!guestStub) {
            out(0xa0, 0x20, 1);
            out(0x20, 0x20, 1);
        }
        break;
    }
    case 0x1c:
    case 0x28:
    case 0x2a:
        break;
    case 0x10:
        switch (ah) {
        case 0:
            vga.mode(al & 127, !(al & 128));
            videoMode = al & 127;
            columns = videoMode == 0 || videoMode == 1 ? 40 : 80;
            rows = 25;
            cursor = 0;
            activePage = 0;
            c.write(0x449, videoMode, 1);
            c.write(0x44a, columns, 2);
            if (!(al & 128))
                for (unsigned i = 0; i < columns * rows; i++)
                    c.write(0xb8000 + i * 2, 0x0720, 2);
            break;
        case 1:
            c.write(0x460, c.get(CX, 2), 2);
            break;
        case 2: {
            unsigned page = c.get(7, 1) & 7;
            auto pos = c.get(DX, 2);
            c.write(0x450 + page * 2, pos, 2);
            if (page == activePage)
                cursor = (pos >> 8) * columns + (pos & 255);
            break;
        }
        case 3:
            c.set(DX, c.read(0x450 + (c.get(7, 1) & 7) * 2, 2), 2);
            c.set(CX, c.read(0x460, 2), 2);
            break;
        case 5:
            activePage = al & 7;
            c.write(0x462, activePage, 1);
            c.write(0x44e, activePage * 4096, 2);
            {
                auto pos = c.read(0x450 + activePage * 2, 2);
                cursor = (pos >> 8) * columns + (pos & 255);
            }
            break;
        case 6:
        case 7:
            scroll(c.get(5, 1), c.get(CX, 1), c.get(6, 1), c.get(DX, 1),
                   ah == 7 ? -int(al) : int(al), c.get(7, 1));
            break;
        case 8:
            c.set(AX, c.read(0xb8000 + (c.get(7, 1) & 7) * 4096 + cursor * 2, 2), 2);
            break;
        case 9:
        case 0xa: {
            auto addr = 0xb8000 + (c.get(7, 1) & 7) * 4096 + cursor * 2;
            for (unsigned i = 0; i < c.get(CX, 2) && i < columns * rows; i++) {
                c.write(addr + i * 2, al, 1);
                if (ah == 9)
                    c.write(addr + i * 2 + 1, c.get(BX, 1), 1);
            }
            break;
        }
        case 0xe:
            character(al, 7);
            break;
        case 0xf:
            c.set(AX, (columns << 8) | videoMode, 2);
            c.set(7, activePage, 1);
            break;
        case 0x10:
            if (al == 0) {
                vga.attr[c.get(BX, 1) & 15] = c.get(7, 1);
            } else if (al == 1) {
                vga.attr[0x11] = c.get(7, 1);
            } else if (al == 2 || al == 9) {
                auto ptr = c.address(ES, c.get(DX, 2));
                for (unsigned i = 0; i < 16; i++) {
                    if (al == 2)
                        vga.attr[i] = c.read(ptr + i, 1);
                    else
                        c.write(ptr + i, vga.attr[i], 1);
                }
                if (al == 2)
                    vga.attr[0x11] = c.read(ptr + 16, 1);
                else
                    c.write(ptr + 16, vga.attr[0x11], 1);
            } else if (al == 3) {
                vga.attr[0x10] = (vga.attr[0x10] & ~8) | (c.get(BX, 1) ? 8 : 0);
            } else if (al == 7) {
                c.set(7, vga.attr[c.get(BX, 1) & 15], 1);
            } else if (al == 8) {
                c.set(7, vga.attr[0x11], 1);
            } else if (al == 0x10) {
                unsigned idx = c.get(BX, 2) & 255;
                vga.dac[idx][0] = c.get(6, 1) & 63;
                vga.dac[idx][1] = c.get(5, 1) & 63;
                vga.dac[idx][2] = c.get(CX, 1) & 63;
            } else if (al == 0x12) {
                unsigned idx = c.get(BX, 2) & 255;
                auto ptr = c.address(ES, c.get(DX, 2));
                for (unsigned i = 0; i < c.get(CX, 2) && idx + i < 256; i++)
                    for (unsigned j = 0; j < 3; j++)
                        vga.dac[idx + i][j] = c.read(ptr + i * 3 + j, 1) & 63;
            }
            break;
        case 0x11:
            if (al == 0x30) {
                c.segment(ES, 0xc000);
                c.set(BP, 0x8ee8, 2);
                c.set(CX, 8, 2);
                c.set(DX, 24, 2);
            }
            break;
        case 0x12:
            if (c.get(BX, 1) == 0x10) {
                c.set(BX, 3, 2);
                c.set(CX, 9, 2);
            }
            break;
        case 0x1a:

            c.set(AX, 0x1a, 2);
            c.set(BX, 8, 2);
            break;
        case 0x1b:
            c.set(AX, 0, 2);
            break;
        default:
            break;
        }
        break;
    case 0x11:
        c.set(AX, c.read(0x410, 2), 2);
        break;
    case 0x12:
        c.set(AX, 640, 2);
        break;
    case 0x13: {
        auto drive = c.get(DX, 1);
        Disk *d = drive == 0x80 ? disk.get() : drive == 0 ? floppy.get() : nullptr;
        unsigned statusAddress = drive & 0x80 ? 0x474 : 0x441;
        auto diskOK = [&]() {
            ok();
            c.write(statusAddress, 0, 1);
        };
        auto diskError = [&](uint8_t code) {
            error(code);
            c.write(statusAddress, code, 1);
        };
        if (ah == 0) {
            diskOK();
            break;
        }
        if (!d) {
            diskError(0x80);
            break;
        }
        if (ah == 1) {
            c.set(4, c.read(statusAddress, 1), 1);
            c.flags = (c.flags & ~CF) | (c.get(4, 1) ? CF : 0);
            break;
        }
        if (ah == 2 || ah == 3 || ah == 4) {
            unsigned sector = c.get(CX, 1) & 63, cyl = c.get(5, 1) | ((c.get(CX, 1) & 0xc0) << 2),
                     head = c.get(6, 1), count = al;
            uint64_t lba = (uint64_t(cyl) * d->heads + head) * d->spt + sector - 1;
            if (sector < 1 || sector > d->spt || head >= d->heads || !count) {
                diskError(4);
                break;
            }
            if (ah == 4 || d->transfer(c, ah == 3, lba, count, c.address(ES, c.get(BX, 2)))) {
                diskOK();
                c.set(AX, count, 1);
            } else
                diskError(4);
            break;
        }
        if (ah == 8) {
            unsigned cyl = std::min<uint64_t>(1024, d->sectors / (d->heads * d->spt));
            if (cyl)
                cyl--;
            c.set(CX, ((cyl & 255) << 8) | ((cyl >> 2) & 0xc0) | d->spt, 2);
            c.set(DX, ((d->heads - 1) << 8) | 1, 2);
            c.set(BX, d->floppy ? 4 : 0, 1);
            if (d->floppy) {
                c.segment(ES, 0xf000);
                c.set(DI, 0x400, 2);
            }
            diskOK();
            break;
        }
        if (ah == 0x15) {
            c.flags &= ~CF;
            c.set(4, d->floppy ? 1 : 3, 1);
            if (!d->floppy) {
                c.set(CX, d->sectors >> 16, 2);
                c.set(DX, d->sectors, 2);
            }
            break;
        }
        if (ah == 0x41 && c.get(BX, 2) == 0x55aa) {
            c.set(BX, 0xaa55, 2);
            c.set(CX, 1, 2);
            c.set(4, 0x30, 1);
            c.flags &= ~CF;
            break;
        }
        if (ah == 0x42 || ah == 0x43) {
            auto ptr = c.address(DS, c.get(SI, 2));
            unsigned count = c.read(ptr + 2, 2);
            uint64_t lba = uint64_t(c.read(ptr + 12, 4)) << 32 | c.read(ptr + 8, 4);
            auto dest = (c.read(ptr + 6, 2) << 4) + c.read(ptr + 4, 2);
            if (c.read(ptr, 1) >= 16 && d->transfer(c, ah == 0x43, lba, count, dest))
                diskOK();
            else
                diskError(4);
            break;
        }
        if (ah == 0x48) {
            auto ptr = c.address(DS, c.get(SI, 2));
            if (c.read(ptr, 2) < 26) {
                diskError(1);
                break;
            }
            c.write(ptr, 26, 2);
            c.write(ptr + 2, 2, 2);
            c.write(ptr + 4, d->sectors / (d->heads * d->spt), 4);
            c.write(ptr + 8, d->heads, 4);
            c.write(ptr + 12, d->spt, 4);
            c.write(ptr + 16, d->sectors, 4);
            c.write(ptr + 20, d->sectors >> 32, 4);
            c.write(ptr + 24, 512, 2);
            diskOK();
            break;
        }
        if (ah == 0x18 || ah == 0x17) {
            diskOK();
            break;
        }
        diskError(1);
        break;
    }
    case 0x14:
        c.set(AX, 0x8000, 2);
        break;
    case 0x15:
        c.write(0xf032a, 0, 1);
        switch (ah) {
        case 0x24:
            c.a20 = al == 1 ? true : al == 0 ? false : c.a20;
            ok();
            if (al == 2)
                c.set(AX, c.a20 ? 1 : 0, 1);
            if (al == 3)
                c.set(BX, 3, 2);
            break;
        case 0x4f:
            c.flags |= CF;
            break;
        case 0x83:
        case 0x86:
        case 0x90:
        case 0x91:
            ok();
            break;
        case 0x87: {
            auto ptr = c.address(ES, c.get(SI, 2));
            auto descbase = [&](unsigned off) {
                auto lo = c.read(ptr + off, 4), hi = c.read(ptr + off + 4, 4);
                return (lo >> 16) | ((hi & 255) << 16) | (hi & 0xff000000);
            };
            auto src = descbase(0x10), dst = descbase(0x18);
            for (unsigned i = 0; i < c.get(CX, 2) * 2; i++)
                c.write(dst + i, c.read(src + i, 1), 1);
            ok();
            break;
        }
        case 0x88:
            c.set(AX, (c.mem.size() - 1024 * 1024) / 1024, 2);
            c.flags &= ~CF;
            break;
        case 0xc0:
            c.segment(ES, 0xf000);
            c.set(BX, 0x300, 2);
            ok();
            break;
        case 0xc1:
            error(0x86);
            break;
        case 0xc2: {
            auto bh = c.get(BX, 2) >> 8;
            switch (al) {
            case 0:
                if (bh > 1) {
                    error(2);
                    break;
                }
                if (bh && !c.read(0xf0320, 4)) {
                    error(5);
                    break;
                }
                c.write(0xf032a, bh ? 1 : 2, 1);
                ps2.mouse().enabled = bh;
                ps2.enableAuxiliaryInterface();
                if (bh) {
                    pic[0].mask &= ~4;
                    pic[1].mask &= ~0x10;
                } else {
                    ps2.mouse().packets.clear();
                    biosMouseCount = 0;
                }
                ok();
                break;
            case 1:
                ps2.mouse().defaults();
                biosMouseCount = 0;
                c.set(BX, 0xaa, 2);
                ok();
                break;
            case 2: {
                const uint8_t rates[] = {10, 20, 40, 60, 80, 100, 200};
                if (bh >= 7) {
                    error(2);
                    break;
                }
                ps2.mouse().rate = rates[bh];
                ok();
                break;
            }
            case 3:
                if (bh > 3) {
                    error(2);
                    break;
                }
                ps2.mouse().resolution = bh;
                ok();
                break;
            case 4:
                c.set(BX, (c.get(BX, 2) & 255) | (ps2.mouse().id << 8), 2);
                ok();
                break;
            case 5:
                if (bh != 3) {
                    error(2);
                    break;
                }
                ps2.mouse().defaults();
                biosMouseCount = 0;
                ok();
                break;
            case 6:
                if (bh == 0) {
                    c.set(BX, (c.get(BX, 2) & 0xff00) | ps2.mouse().status(), 2);
                    c.set(CX, (c.get(CX, 2) & 0xff00) | ps2.mouse().resolution, 2);
                    c.set(DX, (c.get(DX, 2) & 0xff00) | ps2.mouse().rate, 2);
                } else if (bh <= 2)
                    ps2.mouse().scaling2 = bh == 2;
                else {
                    error(2);
                    break;
                }
                ok();
                break;
            case 7:
                c.write(0xf0320, (uint32_t(c.s[ES].selector) << 16) | c.get(BX, 2), 4);
                biosMouseCount = 0;
                if (!c.read(0xf0320, 4)) {
                    ps2.mouse().enabled = false;
                    ps2.mouse().packets.clear();
                }
                ok();
                break;
            default:
                error(1);
                break;
            }
            break;
        }
        default:
            error(0x86);
            break;
        }
        break;
    case 0x16:
        if (ah == 0 || ah == 0x10) {
            if (keys.empty()) {
                waitingKey = true;
                c.halted = true;
                // The firmware waits with interrupts enabled so IRQ1 can fill
                // its input buffer. Caller flags remain in the INT frame.
                c.flags |= IF;
                c.ip = (c.ip - 2) & 65535;
            } else {
                c.set(AX, keys.front(), 2);
                keys.pop_front();
                waitingKey = false;
            }
        } else if (ah == 1 || ah == 0x11) {
            if (keys.empty())
                c.flags |= ZF;
            else {
                c.flags &= ~ZF;
                c.set(AX, keys.front(), 2);
            }
        } else if (ah == 2)
            c.set(AX, c.read(0x417, 1), 1);
        else if (ah == 5) {
            keys.push_back(c.get(CX, 2));
            c.set(AX, 0, 1);
        } else if (ah == 0x12)
            c.set(AX, 0, 2);
        break;
    case 0x17:
        c.set(4, 0x90, 1);
        break;
    case 0x18:
        throw std::runtime_error("BIOS boot failed (INT 18h)");
    case 0x19:
        boot(bootFromFloppy);
        break;
    case 0x1a:
        if (ah == 0xb1) {
            auto bdf = c.get(BX, 2), offset = c.get(DI, 2);
            auto failure = [&](uint8_t status) {
                c.set(4, status, 1);
                c.flags |= CF;
            };
            switch (al) {
            case 1:
                c.set(AX, 1, 2);
                c.set(BX, 0x0210, 2);
                c.set(CX, 0, 2);
                c.r[DX] = 0x20494350;
                c.r[DI] = 0;
                c.flags &= ~CF;
                break;
            case 2:
            case 3: {
                bool found = al == 2 ? c.get(DX, 2) == 0x1234 && c.get(CX, 2) == 0x1111
                                     : (c.r[CX] & 0xffffff) == 0x030000;
                if (!found || c.get(SI, 2))
                    failure(0x86);
                else {
                    c.set(BX, 0x10, 2);
                    ok();
                }
                break;
            }
            case 8:
            case 9:
            case 10:
            case 11:
            case 12:
            case 13: {
                unsigned width = 1u << ((al - 8) % 3);
                if (offset + width > 256 || (offset & (width - 1))) {
                    failure(0x87);
                    break;
                }
                if (al <= 10)
                    c.set(CX, pci.read(bdf, offset, width), width);
                else
                    pci.write(bdf, offset, c.get(CX, width), width);
                ok();
                break;
            }
            default:
                failure(0x81);
                break;
            }
            break;
        }
        if (ah == 0) {
            c.set(CX, ticks >> 16, 2);
            c.set(DX, ticks, 2);
            c.set(AX, 0, 1);
            c.flags &= ~CF;
        } else if (ah == 2 || ah == 4) {
            std::time_t now = rtcTime();
            auto t = *std::localtime(&now);
            auto bcd = [](int v) { return (v / 10) * 16 + v % 10; };
            c.set(CX,
                  ah == 2 ? (bcd(t.tm_hour) << 8) | bcd(t.tm_min) : 0x2000 | bcd(t.tm_year % 100),
                  2);
            c.set(DX, ah == 2 ? bcd(t.tm_sec) << 8 : (bcd(t.tm_mon + 1) << 8) | bcd(t.tm_mday), 2);
            c.flags &= ~CF;
        } else
            error(0x86);
        break;
    case 0x2f:
        c.set(AX, 0, 1);
        break;
    case 0x33:
        c.set(AX, 0, 2);
        c.set(BX, 0, 2);
        break;
    default:
        if (n < 8)
            throw std::runtime_error("unhandled CPU exception " + std::to_string(n) + " at " +
                                     c.location());
        break;
    }
    return true;
}
} // namespace pc
