#include "virtio_gpu.hpp"
#include <algorithm>
#include <fstream>
#include <new>
#include <stdexcept>
namespace pc {
namespace {
struct CommandError {
    uint32_t type;
};
void require(bool condition, uint32_t error = 0x1205) {
    if (!condition)
        throw CommandError{error};
}
} // namespace
VirtioGpu::VirtioGpu(std::span<uint8_t> ram, uint64_t base, Interrupt irq)
    : VirtioDevice(ram, base, 16, 2, 2, std::move(irq)) {}
void VirtioGpu::rememberFrame() {
    if (active())
        pixels(lastFrame, lastWidth, lastHeight);
}
void VirtioGpu::deviceReset() {
    rememberFrame();
    resources.clear();
    allocated = 0;
    scanoutResource = cursorResource = events = 0;
    scanout = {};
    ++generation;
}
uint64_t VirtioGpu::get(std::span<const uint8_t> bytes, size_t offset, unsigned width) {
    require(offset <= bytes.size() && width <= bytes.size() - offset);
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= uint64_t(bytes[offset + i]) << (i * 8);
    return value;
}
void VirtioGpu::put(std::vector<uint8_t> &bytes, size_t offset, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes.at(offset + i) = uint8_t(value >> (i * 8));
}
VirtioGpu::Rect VirtioGpu::rect(std::span<const uint8_t> bytes) {
    return {uint32_t(get(bytes, 24)), uint32_t(get(bytes, 28)), uint32_t(get(bytes, 32)),
            uint32_t(get(bytes, 36))};
}
bool VirtioGpu::contains(const Resource &r, Rect box) {
    return box.width && box.height && box.x <= r.width && box.y <= r.height &&
           box.width <= r.width - box.x && box.height <= r.height - box.y;
}
uint64_t VirtioGpu::configRead(unsigned offset, unsigned width) const {
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) {
        unsigned at = offset + i;
        if (at < 4)
            value |= uint64_t(uint8_t(events >> (at * 8))) << (i * 8);
        if (at == 8)
            value |= 1ull << (i * 8); // num_scanouts=1, num_capsets=0.
    }
    return value;
}
void VirtioGpu::configWrite(unsigned offset, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        if (offset + i >= 4 && offset + i < 8)
            events &= ~(uint32_t(uint8_t(value >> (i * 8))) << ((offset + i - 4) * 8));
}
std::array<uint8_t, 128> VirtioGpu::edid() {
    std::array<uint8_t, 128> data{};
    const uint8_t header[] = {0, 255, 255, 255, 255, 255, 255, 0};
    std::copy(std::begin(header), std::end(header), data.begin());
    data[8] = 0x40;
    data[9] = 0x6d;
    data[10] = 1;
    data[12] = 1;
    data[16] = 1;
    data[17] = 36;
    data[18] = 1;
    data[19] = 4;
    data[20] = 0x80;
    data[21] = 34;
    data[22] = 19;
    data[23] = 120;
    data[24] = 0x0a;
    // sRGB chromaticities and unused standard timing entries.
    const uint8_t chroma[] = {0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54};
    std::copy(std::begin(chroma), std::end(chroma), data.begin() + 25);
    std::fill(data.begin() + 38, data.begin() + 54, 1);
    // 1280x720 @ 60 Hz, 74.25 MHz, 1650x750 totals (CEA timing).
    const uint8_t timing[] = {0x01, 0x1d, 0x00, 0x72, 0x51, 0xd0, 0x1e, 0x20, 0x6e,
                              0x28, 0x55, 0x00, 0x54, 0xbe, 0x10, 0x00, 0x00, 0x1e};
    std::copy(std::begin(timing), std::end(timing), data.begin() + 54);
    data[75] = 0xfc;
    const std::string name = "PCEmu VirtIO\n";
    std::copy(name.begin(), name.end(), data.begin() + 77);
    data[93] = 0xfd;
    data[95] = 50;
    data[96] = 75;
    data[97] = 30;
    data[98] = 90;
    data[99] = 17;
    data[100] = 0;
    data[101] = 10;
    std::fill(data.begin() + 102, data.begin() + 108, 32);
    data[111] = 0x10; // Dummy descriptor.
    unsigned sum = 0;
    for (auto byte : data)
        sum += byte;
    data[127] = uint8_t(-sum);
    return data;
}
void VirtioGpu::readBacking(const Resource &r, uint64_t offset, std::span<uint8_t> output) const {
    require(offset <= r.backingBytes && output.size() <= r.backingBytes - offset);
    for (const auto &entry : r.backing) {
        if (offset >= entry.length) {
            offset -= entry.length;
            continue;
        }
        size_t count = std::min<uint64_t>(output.size(), entry.length - offset);
        auto source = memory(entry.address + offset, count);
        std::copy(source.begin(), source.end(), output.begin());
        output = output.subspan(count);
        offset = 0;
        if (output.empty())
            return;
    }
    require(output.empty());
}
std::vector<uint8_t> VirtioGpu::control(std::span<const uint8_t> request, size_t capacity) {
    require(request.size() >= 24);
    std::vector<uint8_t> response(24, 0);
    auto type = uint32_t(get(request, 0));
    auto header = [&](uint32_t result) {
        put(response, 0, result);
        auto flags = get(request, 4) & 1;
        put(response, 4, flags);
        if (flags)
            put(response, 8, get(request, 8, 8), 8);
        put(response, 16, get(request, 16));
    };
    try {
        require(!(get(request, 4) & ~1ull));
        require(get(request, 16) == 0, 0x1204); // 2D mode has no rendering contexts.
        switch (type) {
        case 0x100:
            require(capacity >= 408);
            response.resize(408, 0);
            put(response, 32, 1280);
            put(response, 36, 720);
            put(response, 40, 1);
            header(0x1101);
            return response;
        case 0x10a: {
            // The signed Windows DOD driver queries EDID without acknowledging
            // its feature bit. This read-only command is safe to serve anyway.
            require(request.size() >= 32);
            require(get(request, 24) == 0, 0x1202);
            require(capacity >= 1056);
            response.resize(1056, 0);
            put(response, 24, 128);
            auto bytes = edid();
            std::copy(bytes.begin(), bytes.end(), response.begin() + 32);
            header(0x1104);
            return response;
        }
        case 0x101: {
            require(request.size() >= 40);
            auto id = uint32_t(get(request, 24)), format = uint32_t(get(request, 28));
            auto width = uint32_t(get(request, 32)), height = uint32_t(get(request, 36));
            require(id && !resources.contains(id));
            require(format == 1 || format == 2 || format == 3 || format == 4 || format == 67 ||
                    format == 68 || format == 121 || format == 134);
            require(width && height && width <= 4096 && height <= 4096);
            size_t length = size_t(width) * height * 4;
            require(resources.size() < 128 && length <= memoryLimit - allocated, 0x1201);
            Resource resource{width, height, format, std::vector<uint8_t>(length), {}, 0};
            resources.emplace(id, std::move(resource));
            allocated += length;
            break;
        }
        case 0x102: {
            require(request.size() >= 32);
            auto id = uint32_t(get(request, 24));
            auto at = resources.find(id);
            require(at != resources.end(), 0x1203);
            if (scanoutResource == id)
                rememberFrame();
            allocated -= at->second.data.size();
            resources.erase(at);
            if (scanoutResource == id) {
                scanoutResource = 0;
                ++generation;
            }
            if (cursorResource == id) {
                cursorResource = 0;
                ++generation;
            }
            break;
        }
        case 0x103: {
            require(request.size() >= 48);
            require(get(request, 40) == 0, 0x1202);
            auto id = uint32_t(get(request, 44));
            auto box = rect(request);
            if (id) {
                auto at = resources.find(id);
                require(at != resources.end(), 0x1203);
                require(contains(at->second, box));
            }
            if (!id)
                rememberFrame();
            ++scanoutCommands;
            scanoutResource = id;
            scanout = box;
            ++generation;
            break;
        }
        case 0x104: {
            require(request.size() >= 48);
            auto id = uint32_t(get(request, 40));
            auto at = resources.find(id);
            require(at != resources.end(), 0x1203);
            require(contains(at->second, rect(request)));
            ++flushes;
            if (id == scanoutResource)
                ++generation;
            break;
        }
        case 0x105: {
            require(request.size() >= 56);
            auto id = uint32_t(get(request, 48));
            auto at = resources.find(id);
            require(at != resources.end(), 0x1203);
            auto &r = at->second;
            auto box = rect(request);
            require(contains(r, box));
            auto offset = get(request, 40, 8);
            uint64_t stride = uint64_t(r.width) * 4;
            uint64_t end = uint64_t(box.height - 1) * stride + uint64_t(box.width) * 4;
            require(offset <= r.backingBytes && end <= r.backingBytes - offset);
            for (unsigned y = 0; y < box.height; ++y) {
                size_t destination = (size_t(box.y + y) * r.width + box.x) * 4;
                readBacking(r, offset + uint64_t(y) * stride,
                            std::span(r.data).subspan(destination, size_t(box.width) * 4));
            }
            ++transfers;
            break;
        }
        case 0x106: {
            require(request.size() >= 32);
            auto id = uint32_t(get(request, 24));
            auto at = resources.find(id);
            require(at != resources.end(), 0x1203);
            auto &r = at->second;
            auto count = get(request, 28);
            require(count && count <= 16384 && request.size() == 32 + count * 16 &&
                    r.backing.empty());
            std::vector<Backing> entries;
            uint64_t length = 0;
            for (size_t i = 0; i < count; ++i) {
                auto address = get(request, 32 + i * 16, 8);
                auto size = uint32_t(get(request, 40 + i * 16));
                require(size && size <= memoryLimit - length);
                try {
                    memory(address, size);
                } catch (const std::runtime_error &) {
                    throw CommandError{0x1205};
                }
                entries.push_back({address, size});
                length += size;
            }
            require(length >= r.data.size());
            r.backing = std::move(entries);
            r.backingBytes = length;
            break;
        }
        case 0x107: {
            require(request.size() >= 32);
            auto at = resources.find(uint32_t(get(request, 24)));
            require(at != resources.end(), 0x1203);
            at->second.backing.clear();
            at->second.backingBytes = 0;
            break;
        }
        default:
            throw CommandError{0x1200};
        }
        header(0x1100);
    } catch (const CommandError &error) {
        lastErrorCommand = type;
        lastErrorResponse = error.type;
        ++commandErrors;
        response.resize(24);
        header(error.type);
    } catch (const std::bad_alloc &) {
        ++commandErrors;
        response.resize(24);
        header(0x1201);
    }
    return response;
}
void VirtioGpu::cursor(std::span<const uint8_t> request) {
    try {
        require(request.size() >= 40);
        auto type = get(request, 0);
        require(type == 0x300 || type == 0x301);
        require(get(request, 24) == 0, 0x1202);
        if (type == 0x300) {
            require(request.size() >= 56);
            auto id = uint32_t(get(request, 40));
            if (id) {
                auto at = resources.find(id);
                require(at != resources.end(), 0x1203);
                require(at->second.width == 64 && at->second.height == 64 &&
                        get(request, 44) < 64 && get(request, 48) < 64);
            }
            cursorResource = id;
            hotX = uint32_t(get(request, 44));
            hotY = uint32_t(get(request, 48));
        }
        cursorX = uint32_t(get(request, 28));
        cursorY = uint32_t(get(request, 32));
        ++generation;
        ++cursorUpdates;
    } catch (const CommandError &) {
        ++commandErrors;
    }
}
std::optional<uint32_t> VirtioGpu::handle(unsigned queue, const Chain &chain) {
    ++commands;
    auto request = chain.input(1024 * 1024);
    if (queue == 1) {
        if (chain.writable)
            throw std::runtime_error("GPU cursor queue must be readable");
        cursor(request);
        return 0;
    }
    if (chain.writable < 24)
        throw std::runtime_error("GPU response buffer lacks header");
    if (request.size() < 24) {
        ++commandErrors;
        std::vector<uint8_t> error(24);
        put(error, 0, 0x1205);
        chain.output(error);
        return 24;
    }
    auto response = control(request, chain.writable);
    chain.output(response);
    return uint32_t(response.size());
}
uint32_t VirtioGpu::pixel(const Resource &r, size_t index) {
    const auto *p = r.data.data() + index * 4;
    uint32_t red = 0, green = 0, blue = 0, alpha = 255;
    switch (r.format) {
    case 1:
        alpha = p[3];
        [[fallthrough]];
    case 2:
        blue = p[0];
        green = p[1];
        red = p[2];
        break;
    case 3:
        alpha = p[0];
        [[fallthrough]];
    case 4:
        red = p[1];
        green = p[2];
        blue = p[3];
        break;
    case 67:
        alpha = p[3];
        [[fallthrough]];
    case 134:
        red = p[0];
        green = p[1];
        blue = p[2];
        break;
    case 121:
        alpha = p[0];
        [[fallthrough]];
    case 68:
        blue = p[1];
        green = p[2];
        red = p[3];
        break;
    }
    return (alpha << 24) | (red << 16) | (green << 8) | blue;
}
bool VirtioGpu::pixels(std::vector<uint32_t> &output, unsigned &width, unsigned &height) const {
    if (!active()) {
        width = height = 0;
        return false;
    }
    const auto &resource = resources.at(scanoutResource);
    width = scanout.width;
    height = scanout.height;
    output.resize(size_t(width) * height);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            output[size_t(y) * width + x] =
                pixel(resource, size_t(scanout.y + y) * resource.width + scanout.x + x) |
                0xff000000;
    if (cursorResource) {
        const auto &r = resources.at(cursorResource);
        for (unsigned y = 0; y < 64; ++y)
            for (unsigned x = 0; x < 64; ++x) {
                int64_t dx = int64_t(cursorX) + x - hotX, dy = int64_t(cursorY) + y - hotY;
                if (dx < 0 || dy < 0 || dx >= width || dy >= height)
                    continue;
                auto foreground = pixel(r, y * 64 + x);
                unsigned alpha = foreground >> 24;
                auto &background = output[size_t(dy) * width + size_t(dx)];
                uint32_t result = 0xff000000;
                for (unsigned shift : {0u, 8u, 16u})
                    result |= ((((foreground >> shift) & 255) * alpha +
                                ((background >> shift) & 255) * (255 - alpha) + 127) /
                               255)
                              << shift;
                background = result;
            }
    }
    return true;
}
bool VirtioGpu::changedPixels(std::vector<uint32_t> &output, unsigned &width,
                              unsigned &height) const {
    width = active() ? scanout.width : 0;
    height = active() ? scanout.height : 0;
    if (!active() || displayedGeneration == generation)
        return false;
    pixels(output, width, height);
    displayedGeneration = generation;
    return true;
}
std::string VirtioGpu::mode() const {
    return active() ? std::to_string(scanout.width) + "x" + std::to_string(scanout.height) + "x32"
                    : "inactive";
}
void VirtioGpu::save(const std::string &path) const {
    std::vector<uint32_t> data;
    unsigned width = 0, height = 0;
    if (!pixels(data, width, height)) {
        if (lastFrame.empty())
            throw std::runtime_error("No captured VirtIO GPU scanout");
        data = lastFrame;
        width = lastWidth;
        height = lastHeight;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (auto pixel : data) {
        char rgb[] = {char(pixel >> 16), char(pixel >> 8), char(pixel)};
        output.write(rgb, 3);
    }
    output.close();
    if (!output)
        throw std::runtime_error("Cannot save VirtIO GPU scanout");
}
} // namespace pc
