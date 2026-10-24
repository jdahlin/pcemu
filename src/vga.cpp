#include "vga.hpp"
#include <algorithm>
namespace pc {
VGA::VGA() {
    mode(3, true);
}
uint16_t VGA::dispiRead() const {
    if (dispiIndex >= dispi.size())
        return 0;
    if (dispi[4] & 2) {
        if (dispiIndex == 1)
            return maxWidth;
        if (dispiIndex == 2)
            return maxHeight;
        if (dispiIndex == 3)
            return 32;
    }
    if (dispiIndex == 7) {
        unsigned stride = unsigned(dispi[6]) * ((dispi[3] + 7) / 8);
        return stride ? std::min(unsigned(65535), vramSize / stride) : 0;
    }
    return dispi[dispiIndex];
}
void VGA::dispiWrite(uint16_t value) {
    if (dispiIndex >= dispi.size())
        return;
    auto bytes = [](unsigned bpp) { return (bpp + 7) / 8; };
    switch (dispiIndex) {
    case 0:
        if (value >= 0xb0c0 && value <= 0xb0c5)
            dispi[0] = value;
        break;
    case 1:
    case 2:
    case 3:
        if (!(dispi[4] & 1))
            dispi[dispiIndex] = value;
        break;
    case 4: {
        bool wasEnabled = dispi[4] & 1;
        if (value & 1) {
            unsigned bpp = dispi[3];
            if (!dispi[1] || dispi[1] > maxWidth || !dispi[2] || dispi[2] > maxHeight ||
                (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32))
                return;
            if (!wasEnabled) {
                dispi[6] = dispi[1];
                dispi[8] = dispi[9] = 0;
                if (!(value & 0x80))
                    std::fill(vram.begin(), vram.end(), 0);
            }
            graphics = true;
            width = dispi[1];
            height = dispi[2];
        }
        dispi[4] = value & 0xe3;
        if (!(value & 1))
            updateGeometry();
        break;
    }
    case 5:
        if (value < vramSize / 65536)
            dispi[5] = value;
        break;
    case 6:
        if (value >= dispi[1] && value && uint64_t(value) * bytes(dispi[3]) * dispi[2] <= vramSize)
            dispi[6] = value;
        break;
    case 8:
    case 9: {
        unsigned x = dispiIndex == 8 ? value : dispi[8];
        unsigned y = dispiIndex == 9 ? value : dispi[9];
        if (uint64_t(x) + dispi[1] <= dispi[6] &&
            (uint64_t(y) + dispi[2]) * dispi[6] * bytes(dispi[3]) <= vramSize)
            dispi[dispiIndex] = value;
        break;
    }
    default:
        break;
    }
}
void VGA::mode(unsigned mode, bool clear) {
    dispi[4] = 0;
    graphics = mode >= 0x0d && mode <= 0x13;
    width = mode == 0x13 || mode == 0xd ? 320 : 640;
    height = mode == 0x12 || mode == 0x11 ? 480 : mode == 0x10 ? 350 : 200;
    std::fill(std::begin(seq), std::end(seq), 0);
    std::fill(std::begin(gfx), std::end(gfx), 0);
    std::fill(std::begin(crtc), std::end(crtc), 0);
    seq[2] = 15;
    seq[4] = mode == 0x13 ? 14 : 6;
    gfx[5] = mode == 0x13 ? 0x40 : 0;
    gfx[6] = graphics ? 5 : 0x0e;
    gfx[7] = 15;
    gfx[8] = 255;
    crtc[1] = width / (mode == 0x13 ? 4 : 8) - 1;
    unsigned displayEnd = height * (mode == 0x13 ? 2 : 1) - 1;
    crtc[0x12] = displayEnd;
    crtc[7] = ((displayEnd >> 7) & 2) | ((displayEnd >> 3) & 0x40);
    crtc[9] = mode == 0x13 ? 1 : 0;
    crtc[0x13] = width / (mode == 0x13 ? 8 : 16);
    const uint8_t egaPalette[16] = {0, 1, 2, 3, 4, 5, 20, 7, 56, 57, 58, 59, 60, 61, 62, 63};
    for (unsigned i = 0; i < 16; i++)
        attr[i] = egaPalette[i];
    for (unsigned i = 0; i < 64; i++) {
        dac[i][0] = ((i & 4) ? 42 : 0) + ((i & 32) ? 21 : 0);
        dac[i][1] = ((i & 2) ? 42 : 0) + ((i & 16) ? 21 : 0);
        dac[i][2] = ((i & 1) ? 42 : 0) + ((i & 8) ? 21 : 0);
    }
    if (clear)
        for (auto &plane : planes)
            plane.fill(0);
}
void VGA::updateGeometry() {
    if (dispi[4] & 1)
        return;
    graphics = gfx[6] & 1;
    if (!graphics)
        return;
    unsigned horizontal = (unsigned(crtc[1]) + 1) * ((gfx[5] & 0x40) ? 4 : 8);
    unsigned vertical = unsigned(crtc[0x12]) | ((crtc[7] & 2) << 7) | ((crtc[7] & 0x40) << 3);
    unsigned repeat = (unsigned(crtc[9] & 31) + 1) * ((crtc[9] & 0x80) ? 2 : 1);
    unsigned rows = (vertical + 1) / repeat;
    // Ignore transient/incomplete register sets while a mode is programmed.
    if (horizontal >= 160 && horizontal <= 1024)
        width = horizontal;
    if (rows >= 100 && rows <= 1024)
        height = rows;
}
uint8_t VGA::read(uint32_t a) {
    if (a >= lfbBase && a - lfbBase < vramSize)
        return vram[a - lfbBase];
    if (dispi[4] & 1)
        return vram[unsigned(dispi[5]) * 65536 + (a & 65535)];
    unsigned plane = gfx[4] & 3, off = a & 65535;
    if (seq[4] & 8) {
        plane = off & 3;
        off >>= 2;
    }
    for (int p = 0; p < 4; p++)
        latch[p] = planes[p][off];
    if (gfx[5] & 8) {
        uint8_t result = 255;
        for (unsigned p = 0; p < 4; p++)
            if (gfx[7] & (1 << p))
                result &= latch[p] ^ ((gfx[2] & (1 << p)) ? 0 : 255);
        return result;
    }
    return latch[plane];
}
void VGA::write(uint32_t a, uint8_t v) {
    if (a >= lfbBase && a - lfbBase < vramSize) {
        vram[a - lfbBase] = v;
        return;
    }
    if (dispi[4] & 1) {
        vram[unsigned(dispi[5]) * 65536 + (a & 65535)] = v;
        return;
    }
    unsigned off = a & 65535, mask = seq[2] & 15;
    if (seq[4] & 8) {
        mask &= 1 << (off & 3);
        off >>= 2;
    }
    unsigned rotate = gfx[3] & 7, logic = (gfx[3] >> 3) & 3, mode = gfx[5] & 3;
    uint8_t rotated = (v >> rotate) | (v << ((8 - rotate) & 7));
    for (unsigned p = 0; p < 4; p++)
        if (mask & (1 << p)) {
            uint8_t data = rotated, bitmask = gfx[8];
            if (mode == 1) {
                planes[p][off] = latch[p];
                continue;
            }
            if (mode == 0 && (gfx[1] & (1 << p)))
                data = (gfx[0] & (1 << p)) ? 255 : 0;
            if (mode == 2)
                data = (v & (1 << p)) ? 255 : 0;
            if (mode == 3) {
                data = (gfx[0] & (1 << p)) ? 255 : 0;
                bitmask &= rotated;
            }
            if (mode != 3) {
                if (logic == 1)
                    data &= latch[p];
                else if (logic == 2)
                    data |= latch[p];
                else if (logic == 3)
                    data ^= latch[p];
            }
            planes[p][off] = (data & bitmask) | (latch[p] & ~bitmask);
        }
}
uint8_t VGA::in(uint16_t p) {
    switch (p) {
    case 0x3cc:
        return misc;
    case 0x3c2:
        return 0x10;
    case 0x3c1:
        return attr[attrIndex & 31];
    case 0x3c4:
        return seqIndex;
    case 0x3c5:
        return seq[seqIndex & 7];
    case 0x3ce:
        return gfxIndex;
    case 0x3cf:
        return gfx[gfxIndex & 15];
    case 0x3d4:
    case 0x3b4:
        return crtcIndex;
    case 0x3d5:
    case 0x3b5:
        return crtc[crtcIndex & 31];
    case 0x3c6:
        return dacMask;
    case 0x3c8:
        return dacWrite;
    case 0x3c9: {
        auto v = dac[dacRead][dacComponent++];
        if (dacComponent == 3) {
            dacComponent = 0;
            dacRead++;
        }
        return v;
    }
    case 0x3da:
        attrData = false;
        return 9;
    default:
        return 0;
    }
}
void VGA::out(uint16_t p, uint8_t v) {
    switch (p) {
    case 0x3c2:
        misc = v;
        break;
    case 0x3c0:
        if (!attrData)
            attrIndex = v & 31;
        else
            attr[attrIndex] = v;
        attrData = !attrData;
        break;
    case 0x3c4:
        seqIndex = v;
        break;
    case 0x3c5:
        seq[seqIndex & 7] = v;
        break;
    case 0x3ce:
        gfxIndex = v;
        break;
    case 0x3cf:
        gfx[gfxIndex & 15] = v;
        break;
    case 0x3d4:
    case 0x3b4:
        crtcIndex = v;
        break;
    case 0x3d5:
    case 0x3b5:
        crtc[crtcIndex & 31] = v;
        break;
    case 0x3c6:
        dacMask = v;
        break;
    case 0x3c7:
        dacRead = v;
        dacComponent = 0;
        break;
    case 0x3c8:
        dacWrite = v;
        dacComponent = 0;
        break;
    case 0x3c9:
        dac[dacWrite][dacComponent++] = v & ((dispi[4] & 0x20) ? 255 : 63);
        if (dacComponent == 3) {
            dacComponent = 0;
            dacWrite++;
        }
        break;
    default:
        break;
    }
    if (p == 0x3c5 || p == 0x3cf || p == 0x3d5 || p == 0x3b5)
        updateGeometry();
}
uint32_t VGA::pixel(unsigned x, unsigned y) const {
    if (x >= width || y >= height)
        return 0xff000000;
    uint8_t color = 0;
    if (dispi[4] & 1) {
        unsigned bpp = dispi[3], count = (bpp + 7) / 8;
        uint64_t at = ((uint64_t(y) + dispi[9]) * dispi[6] + x + dispi[8]) * count;
        if (at + count > vram.size())
            return 0xff000000;
        uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i)
            value |= uint32_t(vram[at + i]) << (i * 8);
        if (bpp == 24 || bpp == 32)
            return 0xff000000 | (value & 0xffffff);
        if (bpp == 15 || bpp == 16) {
            unsigned greenBits = bpp == 16 ? 6 : 5;
            unsigned blue = value & 31, green = (value >> 5) & ((1 << greenBits) - 1);
            unsigned red = (value >> (5 + greenBits)) & 31;
            return 0xff000000 | ((red * 255 / 31) << 16) |
                   ((green * 255 / ((1 << greenBits) - 1)) << 8) | (blue * 255 / 31);
        }
        color = value & dacMask;
        auto expand = [&](uint8_t v) {
            return (dispi[4] & 0x20) ? unsigned(v) : unsigned((v << 2) | (v >> 4));
        };
        return 0xff000000 | (expand(dac[color][0]) << 16) | (expand(dac[color][1]) << 8) |
               expand(dac[color][2]);
    }
    unsigned start = ((crtc[12] << 8) | crtc[13]);
    if (gfx[5] & 0x40) {
        unsigned stride = crtc[0x13] ? crtc[0x13] * 2 : width / 4;
        color = planes[x & 3][(start + y * stride + x / 4) & 65535];
    } else {
        unsigned stride = crtc[0x13] ? crtc[0x13] * 2 : width / 8;
        unsigned a = (start + y * stride + x / 8) & 65535;
        for (unsigned p = 0; p < 4; p++)
            if (planes[p][a] & (128 >> (x & 7)))
                color |= 1 << p;
        color = attr[color] & 63;
    }
    color &= dacMask;
    auto expand = [](uint8_t v) { return uint32_t((v << 2) | (v >> 4)); };
    return 0xff000000 | (expand(dac[color][0]) << 16) | (expand(dac[color][1]) << 8) |
           expand(dac[color][2]);
}
} // namespace pc
