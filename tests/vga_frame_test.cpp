#include "vga.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string_view>
using namespace pc;
int main(int argc, char **argv) {
    auto v = std::make_unique<VGA>();
    std::mt19937 random(0x461);
    for (auto &p : v->planes) for (auto &b : p) b = random();
    for (auto &b : v->vram) b = random();
    for (auto &a : v->attr) a = random();
    uint64_t checks = 0;
    for (unsigned n = 0; n < 128; ++n) {
        v->width = 257 + random() % 70; v->height = 17 + random() % 23;
        for (auto &c : v->dac) for (auto &b : c) b = random() & ((n & 1) ? 255 : 63);
        v->dacMask = random();
        v->crtc[12] = random(); v->crtc[13] = random();
        v->crtc[0x13] = n % 3 ? random() : 0;
        v->gfx[5] = n & 2 ? 0x40 : 0;
        v->dispi[4] = n & 4 ? (1 | ((n & 1) ? 0x20 : 0)) : 0;
        const unsigned formats[] = {8, 8, 15, 16, 24, 32, 0, 7};
        v->dispi[3] = formats[(n >> 3) & 7];
        v->dispi[6] = n & 8 ? 65535 : 317;
        v->dispi[8] = random() % 1024; v->dispi[9] = n & 16 ? 255 : 0;
        unsigned w = v->width + (n % 3) - 1, h = v->height + (n % 3) - 1;
        std::vector<uint32_t> pixels(size_t(w) * h + 1, 0x12345678);
        v->render(pixels, w, h);
        for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
            if (pixels[size_t(y) * w + x] != v->pixel(x, y))
                throw std::runtime_error("VGA frame differs from pixel reference at case " + std::to_string(n));
            ++checks;
        }
        if (pixels.back() != 0x12345678) throw std::runtime_error("VGA frame overrun");
    }
    std::vector<uint32_t> empty;
    v->render(empty, 0, 10); v->render(empty, 10, 0);
    bool rejected = false;
    try { v->render(empty, 1, 1); } catch (const std::invalid_argument &) { rejected = true; }
    if (!rejected) throw std::runtime_error("undersized frame accepted");
    std::cout << "Passed " << checks << " pixel differential checks and frame bounds\n";
    if (argc > 1 && std::string_view(argv[1]) == "--bench") {
        v->width = 640; v->height = 480; v->dispi[4] = 0; v->gfx[5] = 0;
        std::vector<uint32_t> pixels(640 * 480);
        uint64_t checksum = 0;
        auto time = [&](bool batch) {
            auto begin = std::chrono::steady_clock::now();
            for (unsigned n = 0; n < 120; ++n) {
                if (batch) v->render(pixels, 640, 480);
                else for (unsigned y = 0; y < 480; ++y) for (unsigned x = 0; x < 640; ++x)
                    pixels[y * 640 + x] = v->pixel(x, y);
                checksum += pixels[n];
            }
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        };
        for (unsigned mode : {0u, 1u, 8u, 16u, 32u}) {
            v->dispi[4] = mode >= 8 ? 1 : 0;
            v->dispi[3] = mode;
            v->dispi[6] = 640; v->dispi[8] = v->dispi[9] = 0;
            v->gfx[5] = mode == 1 ? 0x40 : 0;
            auto oldMs = time(false), newMs = time(true);
            std::cout << "120 frames, mode " << mode << ": pixel " << oldMs << " ms; frame " << newMs
                      << " ms; ratio " << oldMs / newMs << "; checksum " << checksum << '\n';
        }
    }
}
