// RAM framebuffer (ramfb) is a boot display stored directly in guest RAM.
// Firmware writes the etc/ramfb fw_cfg file with an address, pixel format,
// dimensions and stride. Stride is bytes between row starts, so padding after
// the visible pixels is permitted. The display adapter validates that descriptor
// against guest RAM and converts visible pixels for the host; it has no interrupt
// or command queue. The machine supplies the fw_cfg item and chooses its selector.
//
// The descriptor fields are big-endian, but XRGB8888 pixels are little-endian
// B,G,R,X bytes. X is ignored; host captures use opaque alpha. A previous-frame
// cache avoids reporting a display change when only invisible row padding changes.
// IMPLEMENTATION NOTE: Only XR24/XRGB8888 with flags zero is accepted. Dimension
// and stride caps are host resource policy. File selector 0x20 is fixed by current
// board registration order, not prescribed by the ramfb protocol.
// COMPATIBILITY: Stride zero is rejected instead of deriving a packed row size.
// Reference: QEMU RAMFBCfg layout and ramfb_fw_cfg_write (wire-format definition),
// and Linux DRM format definitions. This is an original protocol implementation.
// https://github.com/qemu/qemu/blob/master/hw/display/ramfb.c#L23
// https://github.com/qemu/qemu/blob/master/hw/display/ramfb.c#L77

#include "arm_ramfb.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
namespace pc {
namespace {
// RAMFBCfg field offsets; descriptor fields are packed, with no native padding.
// https://github.com/qemu/qemu/blob/master/hw/display/ramfb.c#L23
enum ConfigurationField : unsigned {
    // Guest physical address of the framebuffer's first pixel (64 bits).
    Address = 0,
    // DRM FourCC code selecting the pixel layout (32 bits).
    PixelFormat = 8,
    // Reserved/configuration flags; this model requires zero (32 bits).
    Flags = 12,
    // Number of visible pixels in each row (32 bits).
    Width = 16,
    // Number of visible rows (32 bits).
    Height = 20,
    // Bytes between adjacent framebuffer rows, including any padding (32 bits).
    Stride = 24,
};
// IMPLEMENTATION NOTE: The board currently installs etc/ramfb as its first fw_cfg file.
constexpr uint16_t FramebufferFileSelector = 0x20;
// Packed RAMFBCfg contains one 64-bit and five 32-bit big-endian fields.
constexpr size_t ConfigurationBytes = 28;
// DRM_FORMAT_XRGB8888 = fourcc_code('X','R','2','4'), encoded as 0x34325258.
// X is unused; each pixel stores blue, green, red, unused bytes in that order.
// https://github.com/torvalds/linux/blob/master/include/uapi/drm/drm_fourcc.h
constexpr uint32_t Xrgb8888PixelFormat = 0x34325258;
// Each XRGB8888 pixel occupies four bytes, including the unused X byte.
constexpr unsigned PixelBytes = 4;
// PPM captures store three bytes per pixel in red, green, blue order.
constexpr unsigned PpmPixelBytes = 3;
// Host image output uses ARGB with every pixel fully opaque.
constexpr uint32_t OpaqueAlpha = 0xff000000;
// IMPLEMENTATION NOTE: Bound host framebuffer allocations to 4096 pixels per dimension.
constexpr unsigned MaximumDimensionPixels = 4096;
// IMPLEMENTATION NOTE: Limit row stepping to 16 KiB, independently of visible width.
constexpr unsigned MaximumStrideBytes = 16384;
// Decode one descriptor field in big-endian order; callers validate descriptor size first.
// RAMFBCfg / ramfb_fw_cfg_write.
// https://github.com/qemu/qemu/blob/master/hw/display/ramfb.c#L77
uint64_t read_configuration_field(std::span<const uint8_t> bytes, ConfigurationField field) {
    const size_t count = field == ConfigurationField::Address ? sizeof(uint64_t) : sizeof(uint32_t);
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i)
        value = (value << 8) | bytes[size_t(field) + i];
    return value;
}
} // namespace

// Validate the descriptor and describe its mode, or report invalid/inactive.
// IMPLEMENTATION NOTE: Host observation of RAMFBCfg, not a guest device operation.
// https://github.com/qemu/qemu/blob/master/hw/display/ramfb.c#L23
std::string ArmRamFb::mode_description() const {
    auto bytes = fwCfg.file_data(FramebufferFileSelector);
    if (bytes.size() != ConfigurationBytes)
        return "invalid";
    auto address = read_configuration_field(bytes, ConfigurationField::Address),
         format = read_configuration_field(bytes, ConfigurationField::PixelFormat),
         flags = read_configuration_field(bytes, ConfigurationField::Flags);
    auto width = read_configuration_field(bytes, ConfigurationField::Width),
         height = read_configuration_field(bytes, ConfigurationField::Height),
         stride = read_configuration_field(bytes, ConfigurationField::Stride);
    if (!address)
        return "inactive";
    if (format != Xrgb8888PixelFormat || flags || !width || !height ||
        width > MaximumDimensionPixels || height > MaximumDimensionPixels ||
        stride < width * PixelBytes || stride > MaximumStrideBytes || address < ramBase ||
        address - ramBase > ram.size() || height * stride > ram.size() - (address - ramBase))
        return "invalid";
    return std::to_string(width) + "x" + std::to_string(height) + "x32";
}
// Copy visible XRGB pixels to an opaque host ARGB image, ignoring row padding.
// IMPLEMENTATION NOTE: Host observation of RAMFBCfg, not a guest device operation.
// https://github.com/qemu/qemu/blob/master/hw/display/ramfb.c#L23
bool ArmRamFb::read_pixels(std::vector<uint32_t> &output, unsigned &width, unsigned &height) const {
    auto current = mode_description();
    if (current == "inactive" || current == "invalid")
        return false;
    auto bytes = fwCfg.file_data(FramebufferFileSelector);
    auto address = read_configuration_field(bytes, ConfigurationField::Address),
         stride = read_configuration_field(bytes, ConfigurationField::Stride);
    width = read_configuration_field(bytes, ConfigurationField::Width);
    height = read_configuration_field(bytes, ConfigurationField::Height);
    output.resize(size_t(width) * height);
    for (unsigned y = 0; y < height; ++y) {
        auto row = ram.data() + address - ramBase + y * stride;
        for (unsigned x = 0; x < width; ++x)
            output[size_t(y) * width + x] = OpaqueAlpha | uint32_t(row[x * PixelBytes + 2]) << 16 |
                                            uint32_t(row[x * PixelBytes + 1]) << 8 |
                                            row[x * PixelBytes];
    }
    return true;
}
// Report a new frame only when the descriptor or visible pixel bytes change.
// IMPLEMENTATION NOTE: Host observation of RAMFBCfg, not a guest device operation.
// https://github.com/qemu/qemu/blob/master/hw/display/ramfb.c#L23
bool ArmRamFb::changed_pixels(std::vector<uint32_t> &output, unsigned &width,
                              unsigned &height) const {
    auto current = mode_description();
    if (current == "inactive" || current == "invalid") {
        previousDescriptor.clear();
        previousPixels.clear();
        width = height = 0;
        return false;
    }
    auto descriptor = fwCfg.file_data(FramebufferFileSelector);
    width = read_configuration_field(descriptor, ConfigurationField::Width);
    height = read_configuration_field(descriptor, ConfigurationField::Height);
    auto address = read_configuration_field(descriptor, ConfigurationField::Address),
         stride = read_configuration_field(descriptor, ConfigurationField::Stride);
    size_t rowBytes = size_t(width) * PixelBytes;
    bool changed = previousDescriptor.size() != descriptor.size() ||
                   !std::equal(descriptor.begin(), descriptor.end(), previousDescriptor.begin());
    for (unsigned y = 0; !changed && y < height; ++y)
        changed = std::memcmp(ram.data() + address - ramBase + y * stride,
                              previousPixels.data() + y * rowBytes, rowBytes) != 0;
    if (!changed)
        return false;
    read_pixels(output, width, height);
    previousDescriptor.assign(descriptor.begin(), descriptor.end());
    previousPixels.resize(rowBytes * height);
    for (unsigned y = 0; y < height; ++y)
        std::memcpy(previousPixels.data() + y * rowBytes,
                    ram.data() + address - ramBase + y * stride, rowBytes);
    return true;
}
// Export the validated display as binary PPM with RGB pixels.
// IMPLEMENTATION NOTE: Host observation of RAMFBCfg, not a guest device operation.
// https://github.com/qemu/qemu/blob/master/hw/display/ramfb.c#L23
void ArmRamFb::save_ppm(const std::string &path) const {
    auto currentMode = mode_description();
    if (currentMode == "inactive" || currentMode == "invalid")
        throw std::runtime_error("No valid GOP framebuffer");
    auto bytes = fwCfg.file_data(FramebufferFileSelector);
    auto address = read_configuration_field(bytes, ConfigurationField::Address),
         width = read_configuration_field(bytes, ConfigurationField::Width),
         height = read_configuration_field(bytes, ConfigurationField::Height),
         stride = read_configuration_field(bytes, ConfigurationField::Stride);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    // PPM P6 header: binary RGB pixels, eight-bit channel maximum 255.
    // https://netpbm.sourceforge.net/doc/ppm.html
    output << "P6\n" << width << ' ' << height << "\n255\n";
    std::vector<uint8_t> row(width * PixelBytes), rgb(width * PpmPixelBytes);
    for (uint64_t y = 0; y < height; ++y) {
        std::copy_n(ram.begin() + (address - ramBase) + y * stride, row.size(), row.begin());
        for (uint64_t x = 0; x < width; ++x) {
            rgb[x * PpmPixelBytes] = row[x * PixelBytes + 2];
            rgb[x * PpmPixelBytes + 1] = row[x * PixelBytes + 1];
            rgb[x * PpmPixelBytes + 2] = row[x * PixelBytes];
        }
        output.write(reinterpret_cast<const char *>(rgb.data()), rgb.size());
    }
    output.close();
    if (!output)
        throw std::runtime_error("Cannot save GOP framebuffer: " + path);
}
} // namespace pc
