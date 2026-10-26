#pragma once
#include <array>
#include <cstdint>
namespace pc {
// PCEmu cursor upload: word commands at 5659, CURSORSHAPE bytes at 565a.
// Publishing is atomic; malformed/partial uploads never expose partial shapes.
struct HostCursor {
    static constexpr unsigned capacity = 12 + 64 * 64 * 4 + 64 * 8;
    std::array<uint8_t, capacity> shape{}, pending{};
    std::array<uint32_t, 256> palette{};
    unsigned size = 0, received = 0;
    bool active = false, visible = false, uploading = false;
    uint64_t revision = 0;
    uint32_t signature() const {
        uint32_t hash = 2166136261u;
        for (unsigned i = 0; i < size && i < capacity; ++i) hash = (hash ^ shape[i]) * 16777619u;
        return hash;
    }
    static uint8_t monoData(uint8_t andMask, uint8_t xorMask) {
        return uint8_t(~(andMask ^ xorMask));
    }
    static uint8_t monoMask(uint8_t andMask) { return uint8_t(~andMask); }
    static unsigned word(const auto &b, unsigned p) { return b[p] | unsigned(b[p+1]) << 8; }
    static unsigned length(const auto &b) {
        unsigned w = word(b,4), h = word(b,6), bpp = b[11];
        if (!w || w > 64 || !h || h > 64 || b[10] != 1 ||
            word(b,0) >= w || word(b,2) >= h ||
            (bpp != 1 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)) return 0;
        return 12 + ((w+7)/8)*h + (bpp == 1 ? ((w+7)/8)*h : ((bpp+7)/8)*w*h);
    }
    // Call only after validating length(shape). Windows stores colour XOR
    // pixels as BGR, RGB565 or palette indices; AND bits select transparency.
    uint32_t colorPixel(unsigned x, unsigned y) const {
        const unsigned w = word(shape,4), h = word(shape,6), stride = (w+7)/8;
        const unsigned bytes = shape[11]/8;
        const auto *p = shape.data()+12+stride*h+(y*w+x)*bytes;
        uint32_t rgb;
        if (bytes >= 3) rgb = p[0] | unsigned(p[1])<<8 | unsigned(p[2])<<16;
        else if (bytes == 2) {
            const unsigned value = p[0] | unsigned(p[1])<<8;
            rgb = (value & 31)*255/31 | (((value>>5)&63)*255/63)<<8 |
                  (((value>>11)&31)*255/31)<<16;
        } else rgb = palette[p[0]];
        return rgb | ((shape[12+y*stride+x/8] & (0x80>>(x%8))) ? 0 : 0xff000000);
    }
    void write(uint16_t port, uint32_t value, int width) {
        if (port == 0x5659 && width == 2) {
            if (value == 0xc500) {
                active = true; visible = uploading = false; ++revision;
            } else if (value == 0xc501) { received = 0; uploading = true; }
            else if (value == 0xc502 && uploading) {
                uploading = false;
                if (received >= 12 && length(pending) == received) {
                    shape = pending; size = received; active = visible = true; ++revision;
                }
            }
        } else if (port == 0x565a && width == 1 && uploading) {
            if (received < capacity) pending[received++] = value;
            else uploading = false;
        }
    }
};
} // namespace pc
