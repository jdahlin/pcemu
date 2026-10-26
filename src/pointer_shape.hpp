#pragma once
#include <cstdint>
#include <vector>
namespace pc {
// Straight-alpha ARGB pixels, independent of framebuffer cursor compositing.
struct PointerShape {
    bool active = false, visible = false;
    unsigned width = 0, height = 0, hotX = 0, hotY = 0;
    uint64_t revision = 0;
    std::vector<uint32_t> pixels;
};
} // namespace pc
