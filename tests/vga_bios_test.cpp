// Execute the actual option ROM and software interrupts. Expected BIOS data,
// port values and video memory are literal, independent of firmware constants.
#include "machine.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
using namespace pc;
namespace {
unsigned checks = 0;
void check(bool condition, const std::string &message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct Guest {
    std::unique_ptr<Machine> m = std::make_unique<Machine>("", "", 2 * 1024 * 1024);
    std::vector<uint8_t> rom;
    explicit Guest(const char *path, bool standalone = true) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error(std::string("Cannot open ROM: ") + path);
        rom.assign(std::istreambuf_iterator<char>(stream), {});
        m->vgaRom = rom;
        if (standalone) {
            m->cpu.bios = {}; // Every INT executes its guest vector, including INT 10h.
            m->cpu.observeFault = [](const Fault &) {
                throw std::runtime_error("Standalone VGA ROM raised a CPU fault");
            };
        }
        m->initializeVga();
    }
    // Start each call with distinct sentinel registers and DF set. The handler
    // must restore DF and segment/stack state despite using REP instructions.
    void call(uint16_t ax, uint16_t bx = 0, uint16_t cx = 0, uint16_t dx = 0,
              uint16_t es = 0x2345, uint16_t bp = 0x6789, uint16_t di = 0x5678) {
        auto &c = m->cpu;
        c.segment(CS, 0); c.segment(DS, 0x1234); c.segment(ES, es);
        c.segment(SS, 0); c.r[SP] = 0x8000;
        c.r[AX] = ax; c.r[BX] = bx; c.r[CX] = cx; c.r[DX] = dx;
        c.r[SI] = 0x4567; c.r[DI] = di; c.r[BP] = bp;
        for (unsigned r=0;r<8;++r) if (r != SP) c.r[r] |= 0xa5a50000;
        c.flags = 2 | DF | CF; c.halted = false; c.ip = 0x700;
        c.write(0x700, 0x10cd, 2); c.write(0x702, 0xf4, 1);
        uint64_t limit = c.instructions + 2000000;
        while (!c.halted && c.instructions < limit) c.step();
        check(c.halted && c.ip == 0x703, "INT 10h returned within instruction budget");
        check(c.r[SP] == 0x8000 && c.s[SS].selector == 0, "balanced interrupt stack");
        check(c.s[DS].selector == 0x1234, "DS preserved");
        check(c.get(SI,2) == 0x4567 && c.get(DI,2) == di, "SI/DI preserved");
        check((c.flags & (DF | CF)) == (DF | CF), "FLAGS/DF restored");
        if ((ax >> 8) != 0 && (ax >> 8) != 8 && (ax >> 8) != 15 && (ax >> 8) != 0x1a && (ax >> 8) != 0x12 && (ax >> 8) != 0x1b && (ax >> 8) != 0x4f)
            check(c.get(AX, 2) == ax, "AX preserved for call " + std::to_string(ax) + " got " + std::to_string(c.get(AX, 2)));
        if ((ax >> 8) != 15 && (ax >> 8) != 0x1a && ax != 0x1007 && ax != 0x1008 && ax != 0x1019 && ax != 0x101a && (ax >> 8) != 0x12) check(c.get(BX, 2) == bx, "BX preserved");
        if ((ax >> 8) != 3 && ax != 0x1130 && ax != 0x1015 && (ax >> 8) != 0x12)
            check(c.get(CX, 2) == cx, "CX preserved");
        if ((ax >> 8) != 3 && ax != 0x1130 && ax != 0x1015 && (ax >> 8) != 0x12)
            check(c.get(DX, 2) == dx, "DX preserved");
        for (unsigned r=0;r<8;++r) if (r != SP)
            check((c.r[r] >> 16) == 0xa5a5, "upper register halves preserved");
        bool font = ax == 0x1130 && (bx >> 8) < 8;
        if (!font) {
            check(c.s[ES].selector == es, "ES preserved");
            check(c.get(BP,2) == bp, "BP preserved");
        }
    }
    uint16_t word(unsigned a) { return m->cpu.read(a, 2); }
    uint16_t cell(unsigned page, unsigned row, unsigned col) {
        return word(0xb8000 + page * 4096 + row * 160 + col * 2);
    }
};
// The expansion-ROM entry is a far-call ABI, not a boot-sector ABI. Verify it
// can run again with arbitrary segments and DF/IF set without corrupting callers.
void initialization_contract(Guest &g) {
    auto &c = g.m->cpu;
    for (unsigned r = 0; r < 8; ++r) c.r[r] = 0x1111 * (r + 1);
    c.r[SP] = 0x9000;
    std::array<uint32_t, 8> registers;
    std::copy(std::begin(c.r), std::end(c.r), registers.begin());
    c.segment(CS, 0); c.segment(SS, 0);
    c.segment(DS, 0x1234); c.segment(ES, 0x2345);
    c.flags = 2 | DF | IF | CF;
    c.ip = 0x600; c.halted = false;
    const uint8_t entry[] = {0x9a,3,0,0,0xc0,0xf4};
    for (unsigned i = 0; i < sizeof(entry); ++i) c.write(0x600+i, entry[i], 1);
    uint64_t limit = c.instructions + 2000000;
    while (!c.halted && c.instructions < limit) c.step();
    check(c.halted && c.ip == 0x606, "option ROM far return");
    check(std::equal(registers.begin(), registers.end(), std::begin(c.r)), "ROM entry preserves general registers");
    check(c.s[DS].selector == 0x1234 && c.s[ES].selector == 0x2345, "ROM entry preserves segments");
    check(c.flags == (2 | DF | IF | CF), "ROM entry preserves FLAGS/DF/IF");
    check(c.read(0x7e,2) == 0xc000 && c.read(0x10e,2) == 0xc000, "ROM entry installs font vectors");
}
void text_contract(Guest &g) {
    auto &m = *g.m; auto &c = m.cpu;
    g.call(3);
    check(g.word(0x44a) == 80 && g.word(0x44c) == 4096 &&
          c.read(0x484, 1) == 24 && g.word(0x485) == 16, "mode 3 BIOS geometry: " + std::to_string(g.word(0x44a)) + "," + std::to_string(g.word(0x44c)) + "," + std::to_string(c.read(0x484,1)) + "," + std::to_string(g.word(0x485)));
    check((m.vga.seq[2] & 15) == 3 && (m.vga.seq[4] & 6) == 2 && (m.vga.gfx[6] & 15) == 0x0e,
          "mode set programs text memory addressing");
    for (unsigned p = 0; p < 8; ++p) {
        check(g.cell(p, 0, 0) == 0x0720 && g.cell(p, 24, 79) == 0x0720,
              "mode set clears every page");
        g.call(0x0200, p << 8, 0, 0x0307 + p);
        g.call(0x0900 | ('A' + p), (p << 8) | (0x10 + p), 3);
        g.call(0x0300, p << 8);
        check(c.get(DX, 2) == 0x0307 + p, "page-local cursor remains after character repeat");
        g.call(0x0800, p << 8);
        check(c.get(AX, 2) == ((0x10 + p) << 8 | ('A' + p)), "read requested page cursor");
        check(g.cell(p, 3, 9 + p) == ((0x10 + p) << 8 | ('A' + p)), "repeat count");
        g.call(0x0a5a, p << 8, 1);
        check(g.cell(p, 3, 7 + p) == ((0x10 + p) << 8 | 'Z'), "character-only preserves attribute");
        g.call(0x0500 | p);
        check(g.word(0x44e) == p * 4096 && c.read(0x462, 1) == p, "active page BDA");
        check(((unsigned(m.vga.crtc[12]) << 8) | m.vga.crtc[13]) == p * 2048, "CRTC word start");
        check(((unsigned(m.vga.crtc[14]) << 8) | m.vga.crtc[15]) == p * 2048 + 3 * 80 + 7 + p,
              "CRTC cursor includes page base");
    }
    g.call(0x0100, 0, 0x0d0e);
    g.call(0x0300, 0x0700);
    check(c.get(CX, 2) == 0x0d0e && m.vga.crtc[10] == 13 && m.vga.crtc[11] == 14,
          "cursor shape in BIOS and hardware");
    g.call(0x0083);
    check(g.cell(7, 3, 14) == 0x175a, "mode no-clear preserves text");
    g.call(0x0f00, 0x00a5);
    check(c.get(AX, 2) == 0x5083 && c.get(BX, 2) == 0x00a5, "get mode and preserve BL");
    g.call(3);
    g.call(0x0200, 0, 0, 0x0102);
    g.call(0x0e41);
    check(g.cell(0, 1, 2) == 0x0741 && g.word(0x450) == 0x0103, "teletype output");
    g.call(0x0e08); check(g.word(0x450) == 0x0102, "backspace");
    g.call(0x0e0d); check(g.word(0x450) == 0x0100, "carriage return");
    g.call(0x0e0a); check(g.word(0x450) == 0x0200, "line feed retains column");
    g.call(0x0e07); check(g.word(0x450) == 0x0200, "bell does not print");
}
// Check clipped subwindows, overlap direction, clearing and page isolation with
// an independent cell-array oracle, rather than comparing two scroll routines.
void scrolling(Guest &g) {
    auto &c = g.m->cpu;
    for (unsigned down : {0u, 1u}) for (unsigned lines : {0u, 1u, 2u, 8u, 255u}) {
        g.call(3); g.call(0x0502);
        std::array<uint16_t, 2000> before{};
        for (unsigned i = 0; i < before.size(); ++i) {
            before[i] = 0x1000 + i;
            c.write(0xb8000 + 8192 + i * 2, before[i], 2);
        }
        g.call((down ? 0x0700 : 0x0600) | lines, 0x2e00, 0x0203, 0x0709);
        for (int row = 0; row < 25; ++row) for (int col = 0; col < 80; ++col) {
            uint16_t expected = before[row * 80 + col];
            if (row >= 2 && row <= 7 && col >= 3 && col <= 9) {
                int source = down ? row - int(lines) : row + int(lines);
                expected = lines && source >= 2 && source <= 7 ? before[source * 80 + col] : 0x2e20;
            }
            check(g.cell(2, row, col) == expected, "scroll cell oracle down=" + std::to_string(down) +
                  " lines=" + std::to_string(lines) + " row=" + std::to_string(row) +
                  " col=" + std::to_string(col) + " actual=" + std::to_string(g.cell(2, row, col)) +
                  " expected=" + std::to_string(expected));
        }
        check(g.cell(1, 2, 3) == 0x0720, "scroll page isolation");
    }
    g.call(3);
    g.call(0x0200, 0, 0, 0x184f);
    g.call(0x0958, 0x1e, 1);
    g.call(0x0e59);
    check(g.word(0x450) == 0x1800 && g.cell(0, 23, 79) == 0x1e59 &&
          g.cell(0, 24, 0) == 0x0720, "teletype bottom scroll and fill attribute: cursor=" + std::to_string(g.word(0x450)) +
          " shifted=" + std::to_string(g.cell(0,23,79)) + " blank=" + std::to_string(g.cell(0,24,0)));
}
// Unsupported interfaces must not claim success or corrupt the active mode.
// Exercise clipping/empty windows and maximal repeat counts at aperture edges.
void boundaries(Guest &g) {
    auto &c = g.m->cpu;
    g.call(3);
    g.call(0x0200, 0x0700, 0, 0x184f);
    c.write(0xb8000 - 2, 0xa55a, 2);
    g.call(0x0958, 0x073e, 0xffff);
    check(g.cell(7, 24, 79) == 0x3e58 && g.word(0xb8000 - 2) == 0xa55a && g.word(0xc0000) == 0xaa55,
          "maximal repeat stays within text aperture");
    g.call(0x0507);
    g.call(0x0600, 0x4f00, 0x184e, 0xffff);
    check(g.cell(7, 24, 78) == 0x4f20 && g.cell(7, 24, 79) == 0x4f20 &&
          g.cell(7, 23, 79) == 0x0720, "scroll clips bottom/right inclusively");
    auto before = std::vector<uint8_t>(c.mem.begin() + 0xb8000, c.mem.begin() + 0xc0000);
    for (auto rectangle : {std::array<uint16_t,2>{0x0505,0x0406},
                           std::array<uint16_t,2>{0x0505,0x0604},
                           std::array<uint16_t,2>{0x1950,0xffff}})
        g.call(0x0601, 0x5e00, rectangle[0], rectangle[1]);
    g.call(0x0508); g.call(0x0200, 0x0800, 0, 0x1234);
    g.call(0x0959, 0x0800, 0xffff);
    check(std::equal(before.begin(), before.end(), c.mem.begin() + 0xb8000),
          "empty rectangles and invalid pages do not write video memory");
    check(c.read(0x462, 1) == 7 && g.word(0x45e) == 0x184f, "invalid page does not alter BDA");
    g.call(0x0013); g.call(0x0f00);
    check(c.get(AX, 2) == 0x5003, "unsupported graphics mode does not change geometry");
    g.call(0x1130, 0x0800);
    check(c.get(CX, 2) == 0 && c.get(DX, 2) == 0, "unsupported font query leaves registers alone");
}

void extended_text_contract(Guest &g) {
    auto &c = g.m->cpu; auto &v = g.m->vga;
    for (unsigned mode : {0u,1u,2u,3u,7u}) {
        g.call(mode);
        unsigned columns = mode < 2 ? 40 : 80;
        unsigned segment = mode == 7 ? 0xb0000 : 0xb8000;
        check(g.word(0x44a) == columns, "text mode column count");
        g.call(0x0200,0x0100,0,0x0203);
        g.call(0x0942,0x012e,2);
        unsigned bytes = g.word(0x44c);
        check(g.word(segment+bytes+2*columns*2+6) == 0x2e42, "text mode page/aperture");
        g.call(0x0501);
        check(((unsigned(v.crtc[12])<<8)|v.crtc[13]) == bytes/2, "text mode CRTC page start");
    }
    g.call(3);
    g.call(0x1112); // ROM 8x8, resize to 80x50.
    check(g.word(0x485) == 8 && c.read(0x484,1) == 49 && g.word(0x44c) == 8192,
          "font resize 80x50 geometry");
    g.call(0x0503);
    check(c.read(0x462,1) == 3, "fourth 80x50 page accepted");
    g.call(0x0504);
    check(c.read(0x462,1) == 3, "fifth 80x50 page rejected before ROM aperture");
    g.call(3);
    g.call(0x0100,0,0x0607);
    check(v.crtc[10] == 13 && v.crtc[11] == 15 && g.word(0x460) == 0x0607,
          "CGA cursor emulation scales hardware but preserves BIOS shape");
    g.call(0x1201,0x0034);
    g.call(0x0100,0,0x0607);
    check(v.crtc[10] == 6 && v.crtc[11] == 7, "cursor emulation can be disabled");
    g.call(0x1200,0x0034);
    // User font: two 12-line characters in font block 5, starting at code 65.
    for (unsigned i=0;i<24;++i) c.write(0x20100+i,0x80+i,1);
    auto old_seq=v.seq; auto old_gfx=v.gfx;
    g.call(0x1100,0x0c05,2,65,0x2000,0x0100);
    for (unsigned ch=0;ch<2;++ch) for (unsigned row=0;row<32;++row)
        check(v.planes[2][0x6000+(65+ch)*32+row] == (row<12 ? 0x80+ch*12+row : 0),
              "user font block/character offset, source stride and padding");
    check(v.seq==old_seq && v.gfx==old_gfx,"font loading restores register programming");
    g.call(0x1103,0x0028);
    check(v.seq[3]==0x28,"character-generator block selector");
    // Both counted-string formats, non-active page, cursor control and CR/LF.
    const uint8_t message[]={'A',0x2e,'B',0x3f,13,0x4a,10,0x4a,'C',0x5b};
    for (unsigned i=0;i<sizeof(message);++i)c.write(0x20200+i,message[i],1);
    g.call(0x0200,0x0200,0,0x0405);
    g.call(0x1302,0x0207,5,0x0102,0x2000,0x0200);
    check(g.cell(2,1,2)==0x2e41 && g.cell(2,1,3)==0x3f42 && g.cell(2,2,0)==0x5b43,
          "counted string per-character attributes and controls");
    check(g.word(0x454)==0x0405 && c.read(0x462,1)==0,"string keeps caller cursor and active page");
    c.write(0x20200,'X',1);c.write(0x20201,'Y',1);
    g.call(0x1301,0x026c,2,0x0304,0x2000,0x0200);
    check(g.cell(2,3,4)==0x6c58 && g.cell(2,3,5)==0x6c59 && g.word(0x454)==0x0306,
          "counted string uniform attribute and final cursor");
    // EGA palette block includes overscan at index 11h, skipping mode control.
    for (unsigned i=0;i<17;++i)c.write(0x20300+i,i+1,1);
    g.call(0x1002,0,0,0x0300,0x2000);
    g.call(0x1009,0,0,0x0400,0x2000);
    for(unsigned i=0;i<17;++i)check(c.read(0x20400+i,1)==i+1,"attribute palette block roundtrip");
    g.call(0x1003,0);check(!(v.attr[0x10]&8),"background intensity");
    g.call(0x1003,1);check(v.attr[0x10]&8,"attribute blinking");
    g.call(0x1013,0x0100);g.call(0x1013,0x0b01);g.call(0x101a);
    check(c.get(BX,2)==0x0b01,"16-color paging mode and page query: " + std::to_string(c.get(BX,2)) + ", ctrl=" + std::to_string(v.attr[0x10]) + ", page=" + std::to_string(v.attr[0x14]));
    g.call(0x1018,0x55);g.call(0x1019);
    check(c.get(BX,1)==0x55,"PEL mask roundtrip");
    g.call(0x1018,0xff);
    for(unsigned i=0;i<12;++i)c.write(0x20500+i,i*3,1);
    g.call(0x1012,252,4,0x0500,0x2000);
    g.call(0x1017,252,4,0x0600,0x2000);
    for(unsigned i=0;i<12;++i)check(c.read(0x20600+i,1)==i*3,"DAC block roundtrip at last entry");
    g.call(0x1010,50,0,0x3f00);g.call(0x101b,50,1);
    check(v.dac[50][0]==19 && v.dac[50][1]==19 && v.dac[50][2]==19,"weighted grayscale");
    g.call(0x1201,0x0031);g.call(0x1010,1,0x0203,0x0100);g.call(3);
    check(v.dac[1][0]==1 && v.dac[1][1]==2 && v.dac[1][2]==3,"mode set palette-loading disable");
    g.call(0x1200,0x0031);g.call(3);
    g.call(0x1b00,0,0,0,0x2000,0x6789,0x0700);
    check(c.get(AX,1)==0x1b && g.word(0x20705)==80 && c.read(0x20722,1)==25 &&
          g.word(0x20723)==16 && c.read(0x20725,1)==8,"functionality/state buffer fields");
    g.call(0x1210,0x0010);
    check(c.get(BX,2)==3 && c.get(CX,2)==9,"EGA memory/monitor query");
    g.call(3);
}

void fonts_and_palette(Guest &g) {
    auto &m = *g.m; auto &c = m.cpu;
    g.call(3);
    g.call(0x1130, 0x0600);
    unsigned font = c.s[ES].base + c.get(BP, 2);
    check(font >= 0xc0000 && font + 4096 <= 0xc8000, "ROM 8x16 font bounds");
    check(c.get(CX, 2) == 16 && c.get(DX, 1) == 24, "font geometry");
    for (unsigned ch = 0; ch < 256; ++ch) for (unsigned row = 0; row < 16; ++row)
        check(m.vga.planes[2][ch * 32 + row] == c.read(font + ch * 16 + row, 1),
              "font loaded into plane 2");
    g.call(0x1130, 0x0300);
    unsigned first = c.s[ES].base + c.get(BP, 2);
    g.call(0x1130, 0x0400);
    check(c.s[ES].base + c.get(BP, 2) == first + 1024, "8x8 upper half pointer");
    const uint8_t glyph[] = {0x30, 0x78, 0xcc, 0xcc, 0xfc, 0xcc, 0xcc, 0};
    for (unsigned r = 0; r < 8; ++r) check(c.read(first + 65 * 8 + r, 1) == glyph[r], "8x8 A font");
    for (unsigned index : {0u, 1u, 63u, 255u}) {
        g.call(0x1010, index, 0x2312, 0x3100);
        g.call(0x1015, index, 0, 0);
        check(c.get(CX, 2) == 0x2312 && c.get(DX, 2) == 0x3100, "DAC color roundtrip");
    }
    g.call(0x1a00);
    check(c.get(AX, 1) == 0x1a && c.get(BX, 2) == 8, "analog color VGA identification");
    g.call(0x4f00);
    check(c.get(AX, 2) == 0x014f, "VBE honestly unsupported");
    check(std::equal(g.rom.begin(), g.rom.end(), c.mem.begin() + 0xc0000), "ROM never modified");
}
}
int main(int argc, char **argv) {
    try {
        const char *romPath = argc > 1 ? argv[1] : PCEMU_VGA_TEST_ROM;
        Guest guest(romPath);
        check(guest.rom.size() == 32768 && guest.rom[0] == 0x55 && guest.rom[1] == 0xaa &&
              guest.rom[2] == 64, "option ROM signature/size");
        check((std::accumulate(guest.rom.begin(), guest.rom.end(), 0u) & 255) == 0,
              "option ROM checksum");
        auto &rom = guest.rom;
        unsigned pcir = rom[0x18] | (rom[0x19] << 8);
        check(pcir + 24 <= rom.size() && std::equal(rom.begin()+pcir, rom.begin()+pcir+4, "PCIR"),
              "PCI option ROM data structure");
        check(rom[pcir+4] == 0x34 && rom[pcir+5] == 0x12 && rom[pcir+6] == 0x11 &&
              rom[pcir+7] == 0x11 && rom[pcir+16] == 64 && rom[pcir+20] == 0 && rom[pcir+21] == 0x80,
              "PCI vendor/device, image length, x86 code and last-image indicator");
        check((guest.word(0x42) << 4) == 0xc0000, "INT 10h points into standalone ROM");
        initialization_contract(guest); text_contract(guest); scrolling(guest); boundaries(guest); extended_text_contract(guest); fonts_and_palette(guest);
        if (argc > 2) {
            Guest reference(argv[2], false);
            text_contract(reference); scrolling(reference);
            std::cout << "Reference ROM passed the same text/cursor/scroll contract\n";
        }
        std::cout << "Passed " << checks << " standalone VGA BIOS checks\n";
    } catch (const std::exception &e) {
        std::cerr << "VGA BIOS test failed: " << e.what() << '\n'; return 1;
    }
}
