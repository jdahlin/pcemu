#include "arm_ramfb.hpp"
#include <algorithm>
#include <fstream>
#include <stdexcept>
namespace pc {
std::string ArmRamFb::mode() const {
    auto bytes = fwCfg.file(0x20);
    if (bytes.size() != 28) return "invalid";
    auto field = [&](size_t offset, size_t count) {
        uint64_t value = 0;
        for (size_t i = 0; i < count; ++i) value = (value << 8) | bytes[offset + i];
        return value;
    };
    auto address = field(0, 8), format = field(8, 4), flags = field(12, 4);
    auto width = field(16, 4), height = field(20, 4), stride = field(24, 4);
    if (!address) return "inactive";
    if (format != 0x34325258 || flags || !width || !height || width > 4096 || height > 4096 ||
        stride < width * 4 || stride > 16384 || address < ramBase ||
        address - ramBase > ram.size() ||
        height * stride > ram.size() - (address - ramBase))
        return "invalid";
    return std::to_string(width) + "x" + std::to_string(height) + "x32";
}
void ArmRamFb::save(const std::string &path) const {
    auto currentMode = mode();
    if (currentMode == "inactive" || currentMode == "invalid") throw std::runtime_error("No valid GOP framebuffer");
    auto bytes = fwCfg.file(0x20);
    auto field = [&](size_t offset, size_t count) {
        uint64_t value = 0;
        for (size_t i = 0; i < count; ++i) value = (value << 8) | bytes[offset + i];
        return value;
    };
    auto address = field(0, 8), width = field(16, 4), height = field(20, 4), stride = field(24, 4);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "P6\n" << width << ' ' << height << "\n255\n";
    std::vector<uint8_t> row(width * 4), rgb(width * 3);
    for (uint64_t y = 0; y < height; ++y) {
        std::copy_n(ram.begin() + (address - ramBase) + y * stride, row.size(), row.begin());
        for (uint64_t x = 0; x < width; ++x) {
            rgb[x * 3] = row[x * 4 + 2]; rgb[x * 3 + 1] = row[x * 4 + 1]; rgb[x * 3 + 2] = row[x * 4];
        }
        output.write(reinterpret_cast<const char *>(rgb.data()), rgb.size());
    }
    output.close();
    if (!output) throw std::runtime_error("Cannot save GOP framebuffer: " + path);
}
} // namespace pc
