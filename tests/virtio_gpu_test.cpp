#include "virtio_gpu.hpp"
#include "virtio_tool_fixture.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace pc;
using namespace tooltest;
static void put(std::vector<uint8_t> &v, size_t at, uint64_t n, unsigned width = 4) {
    for (unsigned i = 0; i < width; ++i)
        v.at(at + i) = uint8_t(n >> (i * 8));
}
int main() {
    Memory m;
    bool irq = false;
    VirtioGpu gpu(m.ram, m.base, [&](bool v, unsigned) { irq = v; });
    auto setup = [&] { m.configure(gpu, 2, 2); };
    auto request = [](unsigned type, size_t size) {
        std::vector<uint8_t> data(size);
        put(data, 0, type);
        return data;
    };
    auto send = [&](const std::vector<uint8_t> &data, unsigned capacity = 1056) {
        std::copy(data.begin(), data.end(), m.ram.begin() + 0x20000);
        std::fill_n(m.ram.begin() + 0x22000, 1056, 0xa5);
        m.descriptor(0, 0, 0x20000, data.size(), 1, 1);
        m.descriptor(0, 1, 0x22000, capacity, 2);
        m.submit(gpu, 0);
        check(gpu.error().empty());
        return unsigned(m.get(0x22000, 4));
    };
    auto create = [&](unsigned id, unsigned w, unsigned h, unsigned fmt = 2) {
        auto d = request(0x101, 40);
        put(d, 24, id);
        put(d, 28, fmt);
        put(d, 32, w);
        put(d, 36, h);
        return send(d);
    };
    auto attach = [&](unsigned id, size_t first, size_t count) {
        auto d = request(0x106, 48);
        put(d, 24, id);
        put(d, 28, 1);
        put(d, 32, m.base + first, 8);
        put(d, 40, count);
        return send(d);
    };
    auto box = [&](unsigned type, size_t size, unsigned x, unsigned y, unsigned w, unsigned h) {
        auto d = request(type, size);
        put(d, 24, x);
        put(d, 28, y);
        put(d, 32, w);
        put(d, 36, h);
        return d;
    };
    auto transfer = [&](unsigned id, unsigned w, unsigned h, uint64_t offset = 0) {
        auto d = box(0x105, 56, 0, 0, w, h);
        put(d, 40, offset, 8);
        put(d, 48, id);
        return send(d);
    };
    auto scanout = [&](unsigned id, unsigned x, unsigned y, unsigned w, unsigned h) {
        auto d = box(0x103, 48, x, y, w, h);
        put(d, 44, id);
        return send(d);
    };
    setup();
    check(gpu.read(8) == 16 && gpu.read(0x108) == 1);
    check(send(request(0x100, 24)) == 0x1101 && m.get(0x22020, 4) == 1280 &&
          m.get(0x22024, 4) == 720);
    check(send(request(0x10a, 32)) == 0x1104 && m.get(0x22018, 4) == 128);
    unsigned sum = 0;
    for (unsigned i = 0; i < 128; ++i)
        sum += m.ram[0x22020 + i];
    check(!(sum & 255));
    auto fenced = request(0x999, 24);
    put(fenced, 4, 1);
    put(fenced, 8, 0x123456789abcdef0, 8);
    check(send(fenced) == 0x1200 && m.get(0x22004, 4) == 1 &&
          m.get(0x22008, 8) == 0x123456789abcdef0);
    check(send(request(0x100, 24), 24) == 0x1205);
    check(create(1, 4, 3) == 0x1100);
    check(create(1, 4, 3) == 0x1205);
    check(create(2, UINT32_MAX, 3) == 0x1205);
    check(create(2, 1, 0) == 0x1205);
    check(create(2, 1, 1, 999) == 0x1205);
    auto backing = request(0x106, 64);
    put(backing, 24, 1);
    put(backing, 28, 2);
    put(backing, 32, m.base + 0x30000, 8);
    put(backing, 40, 10);
    put(backing, 48, m.base + m.ram.size() + 1, 8);
    put(backing, 56, 38);
    check(send(backing) == 0x1205);
    put(backing, 48, m.base + 0x31000, 8);
    check(send(backing) == 0x1100);
    std::vector<uint8_t> pixels(48);
    for (unsigned i = 0; i < 12; ++i)
        put(pixels, i * 4, 0xff000000 | i * 0x010203);
    std::copy_n(pixels.begin(), 10, m.ram.begin() + 0x30000);
    std::copy(pixels.begin() + 10, pixels.end(), m.ram.begin() + 0x31000);
    check(transfer(1, 4, 3) == 0x1100);
    check(transfer(1, 4, 3, UINT64_MAX) == 0x1205);
    check(scanout(1, 1, 1, 2, 2) == 0x1100 && gpu.mode_description() == "2x2x32");
    std::vector<uint32_t> output;
    unsigned width = 0, height = 0;
    check(gpu.changed_pixels(output, width, height));
    check(width == 2 && height == 2 && output[0] == (0xff000000 | 5 * 0x010203) &&
          output[3] == (0xff000000 | 10 * 0x010203));
    check(!gpu.changed_pixels(output, width, height));
    auto flush = box(0x104, 48, 0, 0, 4, 3);
    put(flush, 40, 1);
    check(send(flush) == 0x1100 && gpu.changed_pixels(output, width, height));
    check(scanout(1, 3, 2, 2, 2) == 0x1205);
    check(scanout(9, 0, 0, 1, 1) == 0x1203);
    // Alpha cursor is composited over the selected resource and clipped to scanout.
    check(create(2, 64, 64, 1) == 0x1100 && attach(2, 0x40000, 16384) == 0x1100);
    m.put(0x40000, 0xffff0000, 4);
    check(transfer(2, 64, 64) == 0x1100);
    auto cursor = request(0x300, 56);
    put(cursor, 28, 1);
    put(cursor, 32, 1);
    put(cursor, 40, 2);
    auto sendCursor = [&] {
        std::copy(cursor.begin(), cursor.end(), m.ram.begin() + 0x24000);
        m.descriptor(1, 0, 0x24000, cursor.size(), 0);
        m.submit(gpu, 1);
        check(gpu.error().empty());
    };
    sendCursor();
    check(gpu.changed_pixels(output, width, height) && output[3] == 0xffff0000);
    auto shape = gpu.pointer_shape();
    check(shape.active && shape.visible && shape.width == 64 && shape.height == 64);
    check(shape.pixels[0] == 0xffff0000 && shape.hotX == 0 && shape.hotY == 0);
    check(gpu.changed_pixels(output, width, height, false));
    check(output[3] != 0xffff0000); // Native host cursor must not appear in streamed pixels.
    check(!gpu.changed_pixels(output, width, height, false));
    check(gpu.changed_pixels(output, width, height, true) && output[3] == 0xffff0000);
    put(cursor, 0, 0x301);
    put(cursor, 28, 0);
    put(cursor, 32, 0);
    sendCursor();
    check(gpu.changed_pixels(output, width, height) && output[0] == 0xffff0000);
    auto unref = request(0x102, 32);
    put(unref, 24, 2);
    check(send(unref) == 0x1100);
    check(!gpu.pointer_shape().visible);
    check(gpu.changed_pixels(output, width, height) && output[0] == (0xff000000 | 5 * 0x010203));
    auto detach = request(0x107, 32);
    put(detach, 24, 1);
    check(send(detach) == 0x1100 && transfer(1, 4, 3) == 0x1205);
    // All standard packed formats produce the same RGB pixel.
    const std::pair<unsigned, std::array<uint8_t, 4>> formats[] = {
        {1, {0x33, 0x22, 0x11, 0xff}},   {2, {0x33, 0x22, 0x11, 0}},
        {3, {0xff, 0x11, 0x22, 0x33}},   {4, {0, 0x11, 0x22, 0x33}},
        {67, {0x11, 0x22, 0x33, 0xff}},  {134, {0x11, 0x22, 0x33, 0}},
        {121, {0xff, 0x33, 0x22, 0x11}}, {68, {0, 0x33, 0x22, 0x11}}};
    for (auto [format, data] : formats) {
        check(create(100 + format, 1, 1, format) == 0x1100);
        std::copy(data.begin(), data.end(), m.ram.begin() + 0x32000);
        check(attach(100 + format, 0x32000, 4) == 0x1100);
        check(transfer(100 + format, 1, 1) == 0x1100);
        check(scanout(100 + format, 0, 0, 1, 1) == 0x1100);
        gpu.read_pixels(output, width, height);
        check(output[0] == 0xff112233);
    }
    check(scanout(0, 0, 0, 0, 0) == 0x1100 && !gpu.active());
    // Windows releases scanout resources before powering off; capture the last
    // frame without turning an orderly shutdown into a host exception.
    auto path = (std::filesystem::temp_directory_path() /
                 ("pcemu-gpu-" + std::to_string(getpid()) + ".ppm"))
                    .string();
    gpu.save_ppm(path);
    std::ifstream capture(path, std::ios::binary);
    std::string saved((std::istreambuf_iterator<char>(capture)), std::istreambuf_iterator<char>());
    check(saved == std::string("P6\n1 1\n255\n") + char(0x11) + char(0x22) + char(0x33));
    capture.close();
    std::filesystem::remove(path);

    // Windows DOD reads EDID without negotiating the optional bit.
    m.configure(gpu, 2, 0);
    check(send(request(0x10a, 32)) == 0x1104);
    setup();
    for (unsigned i = 1; i <= 128; ++i)
        check(create(i, 1, 1) == 0x1100);
    check(create(129, 1, 1) == 0x1201);
    check(gpu.resource_bytes() == 512);
    gpu.write(0x70, 0);
    check(!gpu.active() && gpu.resource_bytes() == 0);
    setup();
    m.descriptor(0, 0, 0x20000, 24, 1, 1);
    m.descriptor(0, 1, 0x22000, 4, 2);
    m.submit(gpu, 0);
    check(!gpu.error().empty() && (gpu.read(0x70) & 64) && irq);
    std::cout << checks << " virtio GPU checks passed\n";
}
