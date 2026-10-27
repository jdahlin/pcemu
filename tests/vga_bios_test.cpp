// Execute the actual option ROM and software interrupts. Expected BIOS data,
// port values and video memory are literal, independent of firmware constants.
#include "machine.hpp"
#include "pc_display.hpp"
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
        check(c.get(SI,2) == 0x4567 && (ax == 0x4f0a || c.get(DI,2) == di), "SI/DI preserved");
        check((c.flags & (DF | CF)) == (DF | CF), "FLAGS/DF restored");
        if ((ax >> 8) != 0 && (ax >> 8) != 8 && (ax >> 8) != 13 && (ax >> 8) != 15 && (ax >> 8) != 0x1a && (ax >> 8) != 0x12 && (ax >> 8) != 0x1b && (ax >> 8) != 0x1c && (ax >> 8) != 0x4f)
            check(c.get(AX, 2) == ax, "AX preserved for call " + std::to_string(ax) + " got " + std::to_string(c.get(AX, 2)));
        if ((ax >> 8) != 15 && (ax >> 8) != 0x1a && ax != 0x1007 && ax != 0x1008 && ax != 0x1019 && ax != 0x101a && (ax >> 8) != 0x12 && ax != 0x4f03 && ax != 0x4f04 && ax != 0x4f06 && ax != 0x4f07 && ax != 0x4f08 && (ax >> 8) != 0x1c) check(c.get(BX, 2) == bx, "BX preserved");
        if ((ax >> 8) != 3 && ax != 0x1130 && ax != 0x1015 && (ax >> 8) != 0x12 && ax != 0x4f06 && ax != 0x4f07 && ax != 0x4f0a)
            check(c.get(CX, 2) == cx, "CX preserved");
        if ((ax >> 8) != 3 && ax != 0x1130 && ax != 0x1015 && (ax >> 8) != 0x12 && ax != 0x4f05 && ax != 0x4f06 && ax != 0x4f07)
            check(c.get(DX, 2) == dx, "DX preserved");
        for (unsigned r=0;r<8;++r) if (r != SP)
            check((c.r[r] >> 16) == 0xa5a5, "upper register halves preserved");
        bool font = ax == 0x1130 && (bx >> 8) < 8;
        if (!font && ax != 0x4f0a) {
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
    check(g.cell(7, 24, 79) == 0x3e58 && g.word(0xb8000 - 2) == 0xffff && g.word(0xc0000) == 0xaa55,
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
    g.call(0x0008); g.call(0x0f00);
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
    for (unsigned pair : {0x0108u,0x0801u,0x0008u}) {
        g.call(0x1a01,pair);g.call(0x1a00);
        check(c.get(BX,2)==pair,"DCC primary/alternate ordering roundtrip");
    }
    unsigned save=c.read(0x4a8,2)+(c.read(0x4aa,2)<<4);
    unsigned secondary=c.read(save+16,2)+(c.read(save+18,2)<<4);
    unsigned dcc=c.read(secondary+2,2)+(c.read(secondary+4,2)<<4);
    check(save>=0xc0000 && secondary>=0xc0000 && g.word(secondary)==26 && c.read(dcc,1)==16,
          "required primary/secondary/DCC save pointers are valid");
    unsigned legacy=c.read(0x74,2)+(c.read(0x76,2)<<4);
    check(c.read(legacy+1,1)==40 && c.read(legacy+17,1)==80,"INT 1Dh legacy parameter sets");
    g.call(0x1210,0x0010);
    check(c.get(BX,2)==3 && c.get(CX,2)==9,"EGA memory/monitor query");
    g.call(3);
}


void graphics_contract(Guest &g) {
    auto &c=g.m->cpu; auto &v=g.m->vga;
    for(unsigned mode : {0x0du,0x0eu,0x0fu,0x10u,0x11u,0x12u,0x13u}) {
        g.call(mode);
        unsigned width=(mode==0x0d || mode==0x13)?320:640;
        unsigned height=mode<=0x0e || mode==0x13?200:mode<=0x10?350:480;
        check(v.graphics && v.width==width && v.height==height,"VGA mode geometry from programmed registers");
        unsigned pages=mode==0x0d?8:mode==0x0e?4:mode<=0x10?2:1;
        unsigned mask=mode==0x0f || mode==0x11?1:mode==0x13?255:15;
        for(unsigned page=0;page<pages;++page)for(unsigned n=0;n<19;++n) {
            unsigned x=n==0?0:n==1?width-1:(n*37)%width;
            unsigned y=n==0?0:n==1?height-1:(n*29)%height;
            unsigned color=(n+page+3)&mask;
            g.call(0x0c00|color,page<<8,x,y);
            g.call(0x0d00,page<<8,x,y);
            check(c.get(AX,1)==color,"BIOS pixel write/read at page/coordinate");
            if(mode==0x13)check(v.planes[x&3][(y*width+x)/4]==color,"chain-4 plane address");
            else {
                unsigned a=page*g.word(0x44c)+y*(width/8)+x/8;
                for(unsigned p=0;p<4;++p)if(mask==15 || p==0)
                    check(bool(v.planes[p][a]&(128>>(x&7)))==bool(color&(1<<p)),"planar bus address and color bit");
                g.call(0x0c80|color,page<<8,x,y);
                g.call(0x0d00,page<<8,x,y);
                check(c.get(AX,1)==0,"planar XOR preserves neighbors and toggles pixel");
            }
        }
        g.call(0x0c01,0,width,0);g.call(0x0d00,0,width,0);
        check(c.get(AX,1)==0,"out-of-bounds pixel rejected");
        g.call(0x0c00|mask,0,3,4);
        g.call(0x0080|mode);
        g.call(0x0d00,0,3,4);
        check(c.get(AX,1)==mask,"graphics no-clear mode set");
    }
    g.call(3);
}


void graphics_text_and_scroll(Guest &g) {
    auto &c=g.m->cpu; auto &v=g.m->vga;
    for(unsigned mode : {0x12u,0x13u}) {
        g.call(mode);g.call(0x0200,0,0,0x0203);g.call(0x0941,0x000e,1);
        g.call(0x0800);
        check(c.get(AX,1)=='A',"graphics glyph readback matches active font");
        g.call(0x1130,0x0100);
        unsigned font=c.s[ES].base+c.get(BP,2), ch=g.word(0x485);
        for(unsigned y=0;y<ch;++y)for(unsigned x=0;x<8;++x) {
            g.call(0x0d00,0,24+x,2*ch+y);
            unsigned expected=c.read(font+'A'*ch+y,1)&(128>>x)?14:0;
            check(c.get(AX,1)==expected,"graphics glyph foreground/background pixels");
        }
        g.call(0x0e42,0x000a);check(g.word(0x450)==0x0204,"graphics teletype advances cursor");
        for(unsigned down : {0u,1u})for(unsigned lines : {0u,1u,50u}) {
            g.call(mode);
            unsigned width=mode==0x13?320:640,height=mode==0x13?200:480;
            unsigned font_height=mode==0x13?8:16;
            for(unsigned p=0;p<4;++p)for(unsigned a=0;a<65536;++a)
                v.planes[p][a]=(a*37+p*53+17)&255;
            auto before=v.planes;
            auto seq=v.seq;auto gfx=v.gfx;
            g.call((down?0x0700:0x0600)|lines,0x0500,0x0203,0x0406);
            if(mode==0x13) {
                for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
                    unsigned at=y*width+x;uint8_t expected=before[at&3][at>>2];
                    if(y>=2*font_height && y<5*font_height && x>=24 && x<56) {
                        int source=down?int(y)-int(lines*font_height):int(y)+int(lines*font_height);
                        if(lines && source>=int(2*font_height) && source<int(5*font_height)) {
                            unsigned from=source*width+x;expected=before[from&3][from>>2];
                        }else expected=5;
                    }
                    check(v.planes[at&3][at>>2]==expected,"packed graphics scroll pixel oracle");
                }
            }else {
                for(unsigned p=0;p<4;++p)for(unsigned a=0;a<65536;++a) {
                    unsigned y=a/(width/8),x=a%(width/8);uint8_t expected=before[p][a];
                    if(y>=2*font_height && y<5*font_height && x>=3 && x<=6) {
                        int source=down?int(y)-int(lines*font_height):int(y)+int(lines*font_height);
                        expected=lines && source>=int(2*font_height) && source<int(5*font_height)
                            ?before[p][source*(width/8)+x]:(5&(1<<p)?255:0);
                    }
                    check(v.planes[p][a]==expected,"planar graphics scroll byte/plane oracle");
                }
            }
            check(v.seq==seq && v.gfx==gfx,"graphics scroll preserves register programming");
        }
    }
    g.call(3);
}


void vbe_contract(Guest &g) {
    auto &c=g.m->cpu;auto &v=g.m->vga;
    for(unsigned a=0;a<514;++a)c.write(0x20100+a,0xa5,1);
    c.write(0x20100,0x32454256,4); // caller signature VBE2.
    g.call(0x4f00,0,0,0,0x2000,0x6789,0x0100);
    check(c.get(AX,2)==0x004f && c.read(0x20100,4)==0x41534556 &&
          g.word(0x20104)==0x0200 && g.word(0x20112)==256,"VBE2 controller info and 16-MiB memory");
    check(c.read(0x20300,1)==0xa5 && c.read(0x20301,1)==0xa5,"512-byte info boundary");
    unsigned list=c.read(0x2010e,2)+(c.read(0x20110,2)<<4);
    std::vector<unsigned> modes;
    for(unsigned n=0;n<128;++n) {
        unsigned mode=g.word(list+n*2);if(mode==0xffff)break;
        check(std::find(modes.begin(),modes.end(),mode)==modes.end(),"unique VBE mode list");
        modes.push_back(mode);
    }
    check(modes.size()==28 && std::find(modes.begin(),modes.end(),0x142)!=modes.end(),"bounded terminated VBE mode list");
    for(unsigned a=0;a<512;++a)c.write(0x20500+a,0xcc,1);
    g.call(0x4f00,0,0,0,0x2000,0x6789,0x0500);
    check(c.read(0x20600,1)==0xcc && c.read(0x206ff,1)==0xcc,"legacy 256-byte info boundary");
    for(unsigned mode:modes) {
        c.write(0x21100,0xa55a,2);
        g.call(0x4f01,0,mode,0,0x2000,0x6789,0x1000);
        check(c.get(AX,2)==0x004f && g.word(0x21100)==0xa55a,"mode info success and 256-byte bounds");
        unsigned w=g.word(0x21012),h=g.word(0x21014),bpp=c.read(0x21019,1),stride=g.word(0x21010);
        check(stride==w*((bpp+7)/8) && g.word(0x21000)==0x009b && c.read(0x21002,1)==7,
              "mode memory layout and available interfaces");
        check(c.read(0x21028,4)==0xe0000000,"mode LFB address comes from PCI BAR");
        if(mode==0x101)check(w==640 && h==480 && bpp==8,"standard 101h geometry");
        if(mode==0x111)check(w==640 && h==480 && bpp==16 && g.word(0x21021)==0x0506,"RGB565 mask layout");
        if(mode==0x142)check(w==640 && h==480 && bpp==32,"Bochs OEM 32-bit geometry");
        v.vram[42]=0xa5;
        g.call(0x4f02,mode|0x4000);
        check(c.get(AX,2)==0x004f && v.dispi[1]==w && v.dispi[2]==h && v.dispi[3]==bpp &&
              v.dispi[4]==0x41 && v.vram[42]==0,"VBE mode set matches advertised mode and clears VRAM");
        g.call(0x4f03);check(c.get(BX,2)==(mode|0x4000),"current VBE mode and LFB flag");
        v.vram[42]=0x7b;g.call(0x4f02,mode|0xc000);
        check(v.vram[42]==0x7b,"VBE no-clear framebuffer preservation");
    }
    g.call(0x4f02,0x4142);
    g.call(0x4f05,0,0,255);check(c.get(AX,2)==0x004f && v.dispi[5]==255,"last bank accepted");
    c.write(0xa0000,0x5a,1);check(v.vram[255*65536]==0x5a,"banked aperture reaches selected VRAM");
    g.call(0x4f05,0x0100);check(c.get(DX,2)==255,"bank readback");
    auto before=v.dispi;
    for(auto request:{std::array<uint16_t,4>{0x4f02,0x4102,0,0}, {0x4f02,0x4942,0,0},
                      {0x4f05,1,0,0},{0x4f05,0,0,256},{0x4f06,0,100,0}}) {
        g.call(request[0],request[1],request[2],request[3]);
        check(c.get(AX,2)==0x014f && v.dispi==before,"invalid VBE request is atomic");
    }
    g.call(0x4f06,0,1024);
    check(c.get(BX,2)==4096 && c.get(CX,2)==1024 && c.get(DX,2)==4096 && v.dispi[6]==1024,"VBE pixel stride and max rows");
    g.call(0x4f07,0,30,100);check(v.dispi[8]==30 && v.dispi[9]==100,"VBE display start");
    g.call(0x4f07,1);check(c.get(CX,2)==30 && c.get(DX,2)==100,"VBE display-start query");
    before=v.dispi;
    g.call(0x4f07,0,400,4000);check(c.get(AX,2)==0x014f && v.dispi==before,"panning visible-rectangle bounds");
    g.call(0x4f06,2,4097);check(c.get(BX,2)==4100 && c.get(CX,2)==1025 && v.dispi[8]==0 && v.dispi[9]==0,"byte stride rounds up to complete pixels");
    g.call(0x4f08,0x0800);check(c.get(BX,2)==0x0800 && (v.dispi[4]&0x20),"8-bit DAC precision");
    c.write(0x22000,0x00fa7b40,4);
    g.call(0x4f09,0,1,10,0x2000,0x6789,0x2000);
    check(v.dac[10][0]==250 && v.dac[10][1]==123 && v.dac[10][2]==64,"VBE BGRA palette format");
    g.call(0x4f09,1,1,10,0x2000,0x6789,0x2100);
    check(c.read(0x22100,4)==0x00fa7b40,"VBE palette readback and reserved byte");
    g.call(0x4f08,0x0700);check(c.get(AX,2)==0x014f && (v.dispi[4]&0x20),"invalid DAC width rejected");
    g.call(0x4f09,0,257,0,0x2000,0x6789,0x2000);check(c.get(AX,2)==0x014f,"invalid palette range rejected");
    g.call(0x4f0a);
    check(c.get(AX,2)==0x004f && c.s[ES].selector==0xc000 && c.get(CX,2)>32,"protected-mode interface points into ROM");
    // Save/restore all four state groups without changing framebuffer bytes.
    v.vram[42]=0x6c;
    g.call(0x4f04,0,15,0);
    check(c.get(BX,2)==16,"state size in 64-byte blocks");
    auto seq=v.seq;auto gfx=v.gfx;auto crtc=v.crtc;auto attr=v.attr;auto dac=v.dac;before=v.dispi;
    c.write(0x23400,0xa55a,2);
    g.call(0x4f04,0x3000,15,1,0x2000);
    check(c.get(AX,2)==0x004f && g.word(0x23400)==0xa55a,"state buffer size and guard");
    g.call(3);g.call(0x1010,10,0,0);
    g.call(0x4f04,0x3000,15,2,0x2000);
    check(c.get(AX,2)==0x004f && v.seq==seq && v.gfx==gfx && v.crtc==crtc && v.attr==attr &&
          v.dac==dac && v.dispi==before,"hardware/BIOS/DAC/DISPI state roundtrip");
    check(v.vram[42]==0x6c && c.read(0x449,1)==0xff,"state restore preserves display bytes and restores BIOS mode");
    c.write(0x23000,0,4);before=v.dispi;
    g.call(0x4f04,0x3000,15,2,0x2000);
    check(c.get(AX,2)==0x014f && v.dispi==before,"invalid state image rejected before writes");
    g.call(0x1c00,0,7);check(c.get(AX,1)==0x1c && c.get(BX,2)==16,"legacy VGA state size query");
    g.call(3);
}


// VBE 2.0 Function 07h supplies pixel origins, including unaligned 8-bit
// starts and whole-page changes. Derive every packed pixel independently.
// VBE Function 07h, printed 50..52, PDF 58..60:
// https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=58
void vbe_indexed_scanout_edges(Guest &g) {
    auto &m=*g.m;auto &v=m.vga;auto &c=m.cpu;
    g.call(0x4f02,0x4100);g.call(0x4f06,0,648);
    for(unsigned n=0;n<256;++n){v.dac[n][0]=n&63;v.dac[n][1]=(n*3)&63;v.dac[n][2]=(n*7)&63;}
    for(unsigned y=0;y<816;++y)for(unsigned x=0;x<648;++x)
        v.vram[y*648+x]=uint8_t(x*17+y*29+(y/400)*71);
    PcDisplay display;std::vector<uint32_t> frame(640*400);
    const unsigned origins[][2]={{0,0},{1,1},{3,3},{4,4},{8,16},{0,400},{0,0}};
    for(const auto &origin:origins) {
        g.call(0x4f07,0,origin[0],origin[1]);
        check(c.get(AX,2)==0x004f,"indexed pixel/page origin accepted");
        g.call(0x4f07,1);
        check(c.get(CX,2)==origin[0]&&c.get(DX,2)==origin[1],"indexed pixel/page origin readback");
        display.refresh(m,true);display.render(m,frame,640,400,500);
        for(unsigned y=0;y<400;++y)for(unsigned x=0;x<640;++x) {
            unsigned sx=x+origin[0],sy=y+origin[1];
            unsigned index=(sx*17+sy*29+(sy/400)*71)&255;
            auto expand=[](unsigned n){return (n<<2)|(n>>4);};
            uint32_t expected=0xff000000u|expand(index&63)<<16|expand((index*3)&63)<<8|expand((index*7)&63);
            check(frame[y*640+x]==expected,"independent packed-pixel origin/page oracle");
        }
    }
    g.call(3);
}

void protected_mode_contract(Guest &g) {
    auto &c=g.m->cpu;auto &v=g.m->vga;
    g.call(0x4f02,0x4142);g.call(0x4f06,0,800);
    g.call(0x4f0a);
    unsigned source=c.s[ES].base+c.get(DI,2),length=c.get(CX,2);
    std::vector<uint8_t> code(c.mem.begin()+source,c.mem.begin()+source+length);
    auto pm_call=[&](unsigned entry,uint32_t bx,uint32_t cx,uint32_t dx,uint32_t di) {
        // Copy the table and all code away from the ROM and execute near calls
        // in a flat 32-bit protected-mode code segment, without BIOS callbacks.
        unsigned base=0x10000;
        std::copy(code.begin(),code.end(),c.mem.begin()+base);
        std::array<Segment,6> segments;
        std::copy(std::begin(c.s),std::end(c.s),segments.begin());
        uint32_t cr0=c.cr[0];
        c.cr[0]=0x11;
        for(unsigned n=0;n<6;++n)c.s[n]={0,0xffffffff,16,true,false};
        c.s[CS]={base,0xffffffff,8,true,false};
        c.r[AX]=0x4f00;c.r[BX]=bx;c.r[CX]=cx;c.r[DX]=dx;c.r[DI]=di;
        c.r[SI]=0x12345678;c.r[BP]=0x87654321;c.r[SP]=0x70000;
        unsigned offset=code[entry*2]|(code[entry*2+1]<<8);
        c.write(base+0x2000,0xe8,1);c.write(base+0x2001,offset-0x2005,4);c.write(base+0x2005,0xf4,1);
        c.ip=0x2000;c.halted=false;c.flags=2|DF;
        auto limit=c.instructions+2000000;
        while(!c.halted && c.instructions<limit)c.step();
        check(c.halted && c.ip==0x2006 && c.r[SP]==0x70000,"copied PM code near-return/stack ABI");
        check(c.r[SI]==0x12345678 && c.r[BP]==0x87654321 && c.s[DS].selector==16 &&
              c.s[SS].selector==16,"PM ESI/EBP/DS/SS preservation");
        unsigned result=c.get(AX,2);
        c.cr[0]=cr0;std::copy(segments.begin(),segments.end(),std::begin(c.s));c.flushTLB();
        return result;
    };
    check(pm_call(0,0,0,3,0)==0x004f && v.dispi[5]==3,"PM bank entry");
    check(pm_call(0,0,0,256,0)==0x014f && v.dispi[5]==3,"PM invalid bank");
    check(pm_call(1,0,(16*3200+8*4)/4,0,0)==0x004f && v.dispi[8]==8 && v.dispi[9]==16,
          "PM display start uses four-byte address units");
    auto before=v.dispi;
    check(pm_call(1,0,0xffff,0xffff,0)==0x014f && v.dispi==before,"PM pan overflow rejected");
    c.write(0x30000,0x00321e0a,4);c.write(0x30004,0x00281405,4);
    check(pm_call(2,0,2,250,0x30000)==0x004f && v.dac[250][0]==50 &&
          v.dac[250][1]==30 && v.dac[250][2]==10 && v.dac[251][0]==40,"PM BGRA palette transfer");
    g.call(3);
}


void cga_contract(Guest &g) {
    auto &c=g.m->cpu;auto &v=g.m->vga;
    for(unsigned mode:{4u,5u,6u}) {
        g.call(mode);
        unsigned width=mode==6?640:320, mask=mode==6?1:3;
        check(v.graphics && v.width==width && v.height==200,"CGA mode register geometry");
        c.write(0xb8000,0xe4,1);c.write(0xba000,0x1b,1);
        for(unsigned x=0;x<(mode==6?8u:4u);++x) {
            unsigned shift=mode==6?7-x:(3-x)*2;
            g.call(0x0d00,0,x,0);check(c.get(AX,1)==((0xe4>>shift)&mask),"direct CGA even-row memory decode");
            g.call(0x0d00,0,x,1);check(c.get(AX,1)==((0x1b>>shift)&mask),"direct CGA odd-row bank decode");
        }
        check(c.read(0xa0000,1)==0xff,"inactive A0000 aperture does not alias CGA memory");
        if(mode==6)check(v.planes[0][0]==0xe4 && v.planes[0][0x2000]==0x1b,"CGA mono sequential plane offsets");
        else check(v.planes[0][0]==0xe4 && v.planes[0][0x1000]==0x1b,"CGA odd/even plane offsets");
        g.call(0x0c00|mask,0,width-1,199);g.call(0x0d00,0,width-1,199);
        check(c.get(AX,1)==mask,"CGA BIOS packed pixel write/read");
        g.call(0x0c80|mask,0,width-1,199);g.call(0x0d00,0,width-1,199);
        check(c.get(AX,1)==0,"CGA XOR pixel");
        g.call(0x0200,0,0,0x0304);g.call(0x0941,mask,1);g.call(0x0800);
        check(c.get(AX,1)=='A',"CGA graphics glyph readback");
        for(unsigned down:{0u,1u})for(unsigned lines:{0u,1u,50u}) {
            g.call(mode);
            for(unsigned a=0;a<16384;++a)c.write(0xb8000+a,(a*37+19)&255,1);
            auto before=v.planes;
            g.call((down?0x0700:0x0600)|lines,uint16_t(mask<<8),0x0203,0x0406);
            unsigned factor=mode==6?1:2;
            for(unsigned a=0;a<16384;++a) {
                unsigned row=(a%8192)/80*2+a/8192,col=(a%8192)%80;
                auto get=[&](unsigned address){return mode==6?before[0][address]:before[address&1][address>>1];};
                uint8_t expected=get(a);
                if(row>=16 && row<40 && col>=3*factor && col<7*factor) {
                    int source=down?int(row)-int(lines*8):int(row)+int(lines*8);
                    expected=lines && source>=16 && source<40
                        ?get((source&1)*8192+(source/2)*80+col):(mode==6?255:255);
                }
                auto actual=mode==6?v.planes[0][a]:v.planes[a&1][a>>1];
                check(actual==expected,"CGA scrolling interleaved-row oracle");
            }
            check(v.planes[2]==before[2] && v.planes[3]==before[3],"CGA scroll does not overwrite font/unused planes");
        }
    }
    g.call(4);g.call(0x0b00,0x0014);
    check(v.attr[0]==4 && v.attr[1]==58 && v.attr[2]==60 && v.attr[3]==62,"CGA background and bright palette");
    g.call(0x0b00,0x0101);
    check(v.attr[1]==59 && v.attr[2]==61 && v.attr[3]==63,"CGA alternate bright palette");
    g.call(6);g.call(0x0b00,2);
    check(v.attr[1]==2,"CGA mono foreground selection");
    g.call(3);
}


void text_scanout_contract(Guest &g) {
    auto &c=g.m->cpu;auto &m=*g.m;
    PcDisplay display;std::vector<uint32_t> pixels(640*400);
    auto frame=[&](uint64_t ticks=500){display.refresh(m,true);display.render(m,pixels,640,400,ticks);};
    auto font=[&](unsigned block,unsigned height,uint8_t first){
        for(unsigned i=0;i<height;++i)c.write(0x20100+i,i==0?first:0,1);
        g.call(0x1100,(height<<8)|block,1,65,0x2000,0x0100);
    };
    g.call(3);font(0,16,0x80);font(5,16,0x40);
    g.call(0x1103,0x24);g.call(0x0200,0,0,0);g.call(0x0941,0x07,1);
    g.call(0x0200,0,0,1);g.call(0x0941,0x0f,1);frame();
    check(pixels[0]==0xffaaaaaa && pixels[1]==0xff000000 && pixels[8]==0xff000000 &&
          pixels[9]==0xffaaaaaa,"dual font maps select plane-2 glyphs without foreground intensity");
    g.call(0);font(0,16,0x80);g.call(0x0941,0x1e,1);frame();
    check(pixels[0]==0xffffff55 && pixels[1]==0xffffff55 && pixels[2]==0xff0000aa,"40-column text scales full logical frame");
    g.call(3);g.call(0x1112);g.call(0x0941,0x1e,1);frame();
    check(m.rows==50 && pixels[2]==0xffffff55 && pixels[640+1]==0xffffff55,"80x50 uses actual 8-line glyph scanout");
    // X-VESA switches to 80x50 directly, leaving the BIOS geometry stale.
    c.mem[0x484]=24;c.mem[0x485]=16;
    frame();
    check(m.rows==50 && pixels[2]==0xffffff55,
          "native text scanout derives geometry from CRTC, not stale BIOS data");
    g.call(7);font(0,14,0x80);g.call(0x0941,0x07,1);frame();
    check(pixels[0]==0xffaaaaaa && c.read(0xb0000,1)=='A',"monochrome text aperture renders");
    g.call(3);g.call(0x1003,0);g.call(0x0900,0xf0,1);frame();
    check(pixels[0]==0xffffffff,"background intensity replaces blink bit");
    g.call(0x1201,0x0036);frame();check(pixels[0]==0xff000000,"sequencer screen-off control renders black");
    g.call(0x1200,0x0036);frame();check(pixels[0]==0xffffffff,"sequencer screen-on restores scanout");
    g.call(3);
}

// Independent scan-line/cell oracle: visible rows can begin part-way through
// a glyph, the lower screen resets on LC+1, and each half has its own panning.
void text_addressing_edges(Guest &g) {
    g.call(3);auto &v=g.m->vga;
    v.seq[1]|=1;v.attr[0x10]=0;v.crtc[0x0a]=0x20;
    for(unsigned a=0;a<65536;++a){v.planes[0][a]=(a*13+7)&255;v.planes[1][a]=(a*11+3)&127;}
    for(unsigned ch=0;ch<256;++ch)for(unsigned y=0;y<32;++y)
        v.planes[2][ch*32+y]=uint8_t(ch*73+y*19);
    for(unsigned n=0;n<16;++n){v.attr[n]=n;v.dac[n][0]=(n*3)&63;v.dac[n][1]=(n*5)&63;v.dac[n][2]=(n*7)&63;}
    PcDisplay display;std::vector<uint32_t> frame(640*400);
    struct Case { unsigned split,preset,start,stride,pan,reset; };
    for(auto c : {Case{1023,0,0,40,0,0},Case{199,0,0,40,0,0},Case{200,0,7,40,0,0},
                 Case{255,7,0,40,3,0},Case{256,15,0x7fff,47,7,0},Case{511,3,0xfff0,40,3,0},
                 Case{512,0,0x7fff,40,0,0},Case{0,0,7,40,7,0},Case{15,0,0,40,7,1},
                 Case{16,7,7,47,3,1},Case{199,15,0,0,7,0},Case{200,7,0xfff0,0,7,1}}) {
        v.crtc[0x18]=c.split;v.crtc[7]=(v.crtc[7]&~0x10)|((c.split>>4)&0x10);
        v.crtc[9]=(v.crtc[9]&~0x40)|((c.split>>3)&0x40);
        v.crtc[8]=c.preset;v.crtc[0x0c]=c.start>>8;v.crtc[0x0d]=c.start;
        v.crtc[0x13]=c.stride;v.attr[0x13]=c.pan;v.attr[0x10]=c.reset?0x20:0;
        display.refresh(*g.m,true);display.render(*g.m,frame,640,400,500);
        for(unsigned y=0;y<400;++y)for(unsigned x=0;x<640;++x) {
            bool below=y>c.split;unsigned line=below?y-c.split-1:y+c.preset;
            unsigned px=x+((below&&c.reset)?0:c.pan);
            unsigned cell=((below?0:c.start)+(line/16)*c.stride*2+px/8)&65535;
            unsigned ch=(cell*13+7)&255,attr=(cell*11+3)&127;
            unsigned bits=uint8_t(ch*73+(line%16)*19);
            bool on=bits&(128>>(px%8));
            // Attribute 01h also requests underline at CRTC14 (default line 31).
            unsigned color=on?attr&15:(attr>>4)&15;
            auto expand=[](unsigned n){return (n<<2)|(n>>4);};
            unsigned expected=0xff000000|(expand((color*3)&63)<<16)|
                              (expand((color*5)&63)<<8)|expand((color*7)&63);
            check(frame[y*640+x]==expected,"text scan-line addressing matches independent oracle");
        }
    }
    // Nine-dot line graphics repeat the eighth dot only for C0h..DFh.
    g.call(3);v.attr[0x10]=4;v.planes[0][0]=0xc4;v.planes[1][0]=7;
    v.planes[2][0xc4*32]=1;v.planes[2][0x41*32]=1;
    auto extent=display.dimensions(*g.m);check(extent.first==720&&extent.second==400,"native text exposes ninth-dot geometry");
    frame.resize(720*400);display.render(*g.m,frame,720,400,500);
    check(frame[7]==frame[8]&&frame[8]!=0xff000000,"line graphics replicate ninth dot");
    v.planes[0][0]=0x41;display.render(*g.m,frame,720,400,500);
    check(frame[7]!=0xff000000&&frame[8]==0xff000000,"ordinary glyph leaves ninth dot blank");
    v.attr[0x12]=0;display.render(*g.m,frame,720,400,500);
    check(frame[7]==0xff000000,"text Color Plane Enable masks the foreground color index");
    g.call(3);
}

// Literal plane-address oracle for split/panning in planar and 256-color modes.
void graphics_addressing_edges(Guest &g) {
    auto &v=g.m->vga;
    for(unsigned mode : {0x12u,0x13u}) {
        g.call(mode);
        for(unsigned p=0;p<4;++p)for(unsigned a=0;a<65536;++a)v.planes[p][a]=uint8_t(a*7+p*37);
        for(unsigned n=0;n<256;++n){if(n<16)v.attr[n]=n;v.dac[n][0]=n&63;v.dac[n][1]=(n*3)&63;v.dac[n][2]=(n*5)&63;}
        struct Case { unsigned split,start,stride,pan,reset,planeMask; };
        for(auto c : {Case{1023,0,40,0,0,15},Case{199,0,40,3,0,15},Case{200,7,47,7,1,15},
                     Case{255,0x7fff,40,6,0,7},Case{256,0xfff0,47,2,1,11},Case{511,7,0,7,0,15},
                     Case{512,0x7fff,0,0,0,3},Case{0,7,40,7,1,15},Case{1,7,40,7,1,15},
                     Case{201,0xfff0,40,7,1,15}}) {
            v.crtc[0x18]=c.split;v.crtc[7]=(v.crtc[7]&~0x10)|((c.split>>4)&0x10);
            v.crtc[9]=(v.crtc[9]&~0x40)|((c.split>>3)&0x40);
            v.crtc[0x0c]=c.start>>8;v.crtc[0x0d]=c.start;v.crtc[0x13]=c.stride;
            v.attr[0x13]=c.pan;v.attr[0x10]=(mode==0x13?0x41:1)|(c.reset?0x20:0);v.attr[0x12]=c.planeMask;
            unsigned w=mode==0x13?320:640,h=mode==0x13?200:480,repeat=mode==0x13?2:1;
            std::vector<uint32_t> frame(w*h);v.render_graphics(frame,w,h);
            for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
                bool below=y*repeat>c.split;
                unsigned row=below?(y*repeat-c.split-1)/repeat:y;
                unsigned px=x+((below&&c.reset)?0:(mode==0x13?c.pan/2:c.pan));
                unsigned at=((below?0:c.start)+row*c.stride*2+px/(mode==0x13?4:8))&65535;
                unsigned color=0;
                if(mode==0x13)color=uint8_t(at*7+(px%4)*37);
                else for(unsigned p=0;p<4;++p)if((c.planeMask&(1<<p))&&(uint8_t(at*7+p*37)&(128>>(px%8))))color|=1<<p;
                auto expand=[](unsigned n){return (n<<2)|(n>>4);};
                unsigned expected=0xff000000|(expand(color&63)<<16)|(expand((color*3)&63)<<8)|expand((color*5)&63);
                check(frame[y*w+x]==expected,"graphics address generator matches independent plane oracle");
                if((x%31)==0&&(y%17)==0)check(v.pixel_color(x,y)==expected,"single-pixel and batch scanout agree with oracle");
            }
        }
    }
    g.call(3);
}

// CRTC protection must leave Line Compare's overflow bit writable.
void protected_crtc_edges(Guest &g) {
    g.call(3);auto &v=g.m->vga;
    auto write=[&](unsigned index,unsigned value){v.write_register(0x3d4,index);v.write_register(0x3d5,value);};
    write(0x11,0x0e);write(7,0x6f);
    for(unsigned n=0;n<7;++n)write(n,n*17+3);
    write(0x11,0x8e);
    for(unsigned n=0;n<7;++n){auto before=v.crtc[n];write(n,before^255);check(v.crtc[n]==before,"CRTC protection blocks timing writes 00h..06h");}
    write(7,0x10);check(v.crtc[7]==0x7f,"protected Overflow permits only LC8 set");
    write(7,0);check(v.crtc[7]==0x6f,"protected Overflow permits only LC8 clear");
    write(8,0x63);write(0x18,0x55);check(v.crtc[8]==0x63&&v.crtc[0x18]==0x55,"display panning/line compare remain writable");
    write(0x11,0x0e);write(0,0x55);write(7,0);
    check(v.crtc[0]==0x55&&v.crtc[7]==0,"unlocked timing registers accept writes");
    v.attrData=true;
    check(v.read_io_register(0x3ba,0)==0&&!v.attrData,"monochrome Status1 resets Attribute flip-flop");
    check(v.read_io_register(0x3ba,1)==9,"monochrome Status1 shares retrace transitions");
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
    check(c.get(AX, 2) == 0x004f, "VBE controller service supported");
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
        initialization_contract(guest); text_contract(guest); scrolling(guest); boundaries(guest); extended_text_contract(guest); graphics_contract(guest); graphics_text_and_scroll(guest); vbe_contract(guest); vbe_indexed_scanout_edges(guest); protected_mode_contract(guest); cga_contract(guest); text_scanout_contract(guest); text_addressing_edges(guest); graphics_addressing_edges(guest); protected_crtc_edges(guest); fonts_and_palette(guest);
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
