// Virtio GPU 2D keeps host-private pixel resources separate from guest backing
// memory. The guest creates a resource, attaches guest RAM entries, transfers
// rendered rectangles into it, selects a scanout rectangle and flushes updates.
// One scanout displays that resource; a second queue updates/moves a 64x64 cursor.
// The common Virtio core validates queue chains and owns transport/interrupt state.
// Control responses carry precise error types and synchronous fence completion.
//
// IMPLEMENTATION NOTE: This is a CPU-backed 2D model: no 3D contexts, capability
// sets, blob resources or VGA compatibility. Resource bytes, dimensions, resource
// count, backing entries and command size have host bounds. EDID (Extended Display
// Identification Data) describes a fixed virtual 1280x720 monitor. Captures convert
// supported eight-bit channel formats to host ARGB; scanout is opaque and cursor
// alpha may be composited or exported independently. The last frame survives
// disable/unreference/reset for host captures; it does not keep scanout active.
// COMPATIBILITY: GET_EDID is served even without EDID negotiation because the
// existing Windows display-only driver queries it that way. Cursor errors count
// diagnostics without a control response; malformed transport buffers still cause
// device reset via the common core. Unsupported commands return ERR_UNSPEC.
// Virtio 1.2, 5.7 GPU Device; VESA E-EDID Release A revision 2 (2006), base EDID 1.4.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3650007
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3810008
// https://glenwing.github.io/docs/VESA-EEDID-A2.pdf#page=19

#include "virtio_gpu.hpp"
#include <algorithm>
#include <fstream>
#include <new>
#include <stdexcept>
namespace pc {
namespace {
// Virtio 1.2, 5.7.1-5.7.3: 2D GPU type, two queues and EDID feature bit 1.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3650007
constexpr uint32_t GpuDeviceId = 16;
enum QueueIndex : unsigned { ControlQueue = 0, CursorQueue = 1 };
constexpr unsigned QueueCount = 2, EdidFeatureBit = 1;
constexpr uint64_t OfferedFeatures = 1ull << EdidFeatureBit;
// Control command/response type values, 5.7.6.7 Request header.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3800007
enum Command : uint32_t {
    GetDisplayInfo = 0x100,        // Return per-scanout rectangles/enabled state.
    ResourceCreate2d = 0x101,      // Allocate host-private pixel storage.
    ResourceUnreference = 0x102,   // Destroy a resource and detach active uses.
    SetScanout = 0x103,            // Select resource rectangle for a display output.
    ResourceFlush = 0x104,         // Publish updated resource content to active scanouts.
    TransferToHost2d = 0x105,      // Copy guest backing pixels into host-private storage.
    ResourceAttachBacking = 0x106, // Bind guest physical memory entries to a resource.
    ResourceDetachBacking = 0x107, // Remove a resource's guest-memory backing.
    GetEdid = 0x10a,               // Return monitor identification for a scanout.
    UpdateCursor = 0x300,          // Set cursor resource, hotspot and position.
    MoveCursor = 0x301,            // Move the existing cursor image.
};
enum Response : uint32_t {
    OkNoData = 0x1100,
    OkDisplayInfo = 0x1101,
    OkEdid = 0x1104,
    ErrorUnspecified = 0x1200,
    ErrorOutOfMemory = 0x1201,
    ErrorInvalidScanoutId = 0x1202,
    ErrorInvalidResourceId = 0x1203,
    ErrorInvalidContextId = 0x1204,
    ErrorInvalidParameter = 0x1205,
};
// All control fields are little-endian. Fence requests complete synchronously;
// their fence_id is copied into the response. No 3D rendering context is supported.
enum HeaderField : unsigned { Type = 0, Flags = 4, FenceId = 8, ContextId = 16 };
constexpr unsigned ControlHeaderBytes = 24;
constexpr uint64_t FenceFlag = 1;
// Rectangles after the header contain four u32 fields (x,y,width,height).
enum RectangleField : unsigned { X = 24, Y = 28, Width = 32, Height = 36 };
// Command-specific field layouts, Virtio 1.2, 5.7.6.8 controlq.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3810008
struct Create2dField {
    enum : unsigned { ResourceId = 24, Format = 28, Width = 32, Height = 36 };
};
struct EdidRequestField {
    enum : unsigned { ScanoutId = 24 };
};
struct ResourceField {
    enum : unsigned { ResourceId = 24, Count = 28 };
};
struct ScanoutField {
    enum : unsigned { ScanoutId = 40, ResourceId = 44 };
};
struct FlushField {
    enum : unsigned { ResourceId = 40 };
};
struct TransferField {
    enum : unsigned { Offset = 40, ResourceId = 48 };
};
struct BackingField {
    enum : unsigned { Entries = 32, Length = 8 };
};
// Display-info response has a 24-byte header and sixteen 24-byte display modes.
// Only mode zero is enabled. EDID response has size/padding then a 1024-byte array.
constexpr unsigned DisplayInfoResponseBytes = 408, DisplayInfoEnabledOffset = 40;
constexpr unsigned EdidResponseBytes = 1056, EdidResponseSizeOffset = 24,
                   EdidResponseDataOffset = 32;
// Request byte lengths include their fixed header and any trailing padding.
constexpr unsigned ResourceRequestBytes = 32, Create2dRequestBytes = 40, ScanoutRequestBytes = 48,
                   TransferRequestBytes = 56, BackingEntryBytes = 16;
// Cursor position follows the header; UPDATE_CURSOR then adds resource and hotspot.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-38300010
struct CursorField {
    enum : unsigned { ScanoutId = 24, X = 28, Y = 32, ResourceId = 40, HotX = 44, HotY = 48 };
};
constexpr unsigned CursorMoveBytes = 40, CursorUpdateBytes = 56, CursorDimensionPixels = 64;
// 8-bit unsigned-normalized channel formats, Virtio 1.2 format enumeration.
// Names describe stored byte order; X is unused, A is alpha.
enum PixelFormat : uint32_t {
    B8G8R8A8Unorm = 1,
    B8G8R8X8Unorm = 2,
    A8R8G8B8Unorm = 3,
    X8R8G8B8Unorm = 4,
    R8G8B8A8Unorm = 67,
    X8B8G8R8Unorm = 68,
    A8B8G8R8Unorm = 121,
    R8G8B8X8Unorm = 134,
};
// Device config: events_read:u32, events_clear:u32, num_scanouts:u32, num_capsets:u32.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3690004
constexpr unsigned EventsClearOffset = 4, ScanoutCountOffset = 8;
// IMPLEMENTATION NOTE: One fixed 1280x720 boot display, with resource/buffer caps.
constexpr unsigned ScanoutCount = 1, DefaultWidth = 1280, DefaultHeight = 720,
                   MaximumDimensionPixels = 4096, MaximumResources = 128,
                   MaximumBackingEntries = 16384;
constexpr size_t MaximumRequestBytes = 1024 * 1024;
// Four-byte pixels map to host ARGB; opaque scanout ignores resource alpha.
constexpr unsigned PixelBytes = 4, BitsPerByte = 8;
constexpr uint32_t OpaqueAlpha = 0xff000000;
constexpr unsigned ChannelMaximum = 255, AlphaRoundingBias = 127;
// EDID base block length/checksum location, VESA E-EDID A2, sections 3.1/3.12.
constexpr unsigned EdidBytes = 128, EdidChecksumOffset = 127;
struct CommandError {
    uint32_t type;
};
// Reject invalid command input with a protocol error, distinct from a transport failure.
// Virtio 1.2 GPU response type definitions.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3800007
void require(bool condition, uint32_t error = Response::ErrorInvalidParameter) {
    if (!condition)
        throw CommandError{error};
}
} // namespace
// Bind the two 2D GPU queues and offer monitor EDID support.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3650007
VirtioGpu::VirtioGpu(std::span<uint8_t> ram, uint64_t base, Interrupt irq)
    : VirtioDevice(ram, base, GpuDeviceId, QueueCount, OfferedFeatures, std::move(irq)) {}
// Cache the currently active composited display before its resource is detached/reset.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3740001
void VirtioGpu::remember_frame() {
    if (active())
        read_pixels(lastFrame, lastWidth, lastHeight);
}
// Preserve the last frame, clear resources/scanout/cursor/events and advance host generation.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3650007
void VirtioGpu::reset_device() {
    remember_frame();
    resources.clear();
    allocated = 0;
    scanoutResource = cursorResource = events = 0;
    scanout = {};
    ++generation;
}
// Decode a bounded little-endian command field or return INVALID_PARAMETER via CommandError.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3800007
uint64_t VirtioGpu::read_command_field(std::span<const uint8_t> bytes, size_t offset,
                                       unsigned width) {
    require(offset <= bytes.size() && width <= bytes.size() - offset);
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= uint64_t(bytes[offset + i]) << (i * BitsPerByte);
    return value;
}
// Encode a little-endian response field into its sized host buffer.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3800007
void VirtioGpu::write_response_field(std::vector<uint8_t> &bytes, size_t offset, uint64_t value,
                                     unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes.at(offset + i) = uint8_t(value >> (i * BitsPerByte));
}
// Decode x/y/width/height following the common command header.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3810008
VirtioGpu::Rect VirtioGpu::read_rectangle(std::span<const uint8_t> bytes) {
    return {uint32_t(read_command_field(bytes, RectangleField::X)),
            uint32_t(read_command_field(bytes, RectangleField::Y)),
            uint32_t(read_command_field(bytes, RectangleField::Width)),
            uint32_t(read_command_field(bytes, RectangleField::Height))};
}
// Check a nonempty rectangle lies entirely within the resource without overflow.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3810008
bool VirtioGpu::contains(const Resource &r, Rect box) {
    return box.width && box.height && box.x <= r.width && box.y <= r.height &&
           box.width <= r.width - box.x && box.height <= r.height - box.y;
}
// Read events and one-scanout/no-capset configuration bytes.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3690004
uint64_t VirtioGpu::read_device_configuration(unsigned offset, unsigned width) const {
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) {
        unsigned at = offset + i;
        if (at < EventsClearOffset)
            value |= uint64_t(uint8_t(events >> (at * 8))) << (i * BitsPerByte);
        if (at == ScanoutCountOffset)
            value |= uint64_t(ScanoutCount) << (i * BitsPerByte); // num_scanouts=1, num_capsets=0.
    }
    return value;
}
// Clear only event bits written through events_clear; ignore other fields.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3690004
void VirtioGpu::write_device_configuration(unsigned offset, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        if (offset + i >= EventsClearOffset && offset + i < ScanoutCountOffset)
            events &= ~(uint32_t(uint8_t(value >> (i * BitsPerByte)))
                        << ((offset + i - EventsClearOffset) * 8));
}
// Build the retained 128-byte virtual-monitor EDID and its checksum.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://glenwing.github.io/docs/VESA-EEDID-A2.pdf#page=19
std::array<uint8_t, 128> VirtioGpu::edid() {
    std::array<uint8_t, EdidBytes> data{};
    // VESA E-EDID A2, 3.2-3.4: signature, manufacturer/product/serial identity.
    // https://glenwing.github.io/docs/VESA-EEDID-A2.pdf#page=21
    constexpr uint8_t Signature[] = {0, 255, 255, 255, 255, 255, 255, 0};
    std::copy(std::begin(Signature), std::end(Signature), data.begin());
    // IMPLEMENTATION NOTE: Retained manufacturer code 406dh decodes to PCM,
    // product 0001h and serial 00000001h; these identify the virtual monitor.
    constexpr unsigned ManufacturerOffset = 8, ProductOffset = 10, SerialOffset = 12;
    data[ManufacturerOffset] = 0x40;
    data[ManufacturerOffset + 1] = 0x6d;
    data[ProductOffset] = 1;
    data[SerialOffset] = 1;
    // Week 1, year 1990+36=2026; EDID structure version 1, revision 4.
    // https://glenwing.github.io/docs/VESA-EEDID-A2.pdf#page=22
    constexpr unsigned ManufactureWeekOffset = 16, ManufactureYearOffset = 17, VersionOffset = 18,
                       RevisionOffset = 19;
    data[ManufactureWeekOffset] = 1;
    data[ManufactureYearOffset] = 36;
    data[VersionOffset] = 1;
    data[RevisionOffset] = 4;
    // Digital input (bit 7), unspecified depth/interface; 34x19 cm image size;
    // gamma encoding 120 gives (120+100)/100=2.20. Feature 0ah advertises
    // RGB/YCrCb 4:4:4 and preferred native timing, with no power-management modes.
    // https://glenwing.github.io/docs/VESA-EEDID-A2.pdf#page=24
    // https://glenwing.github.io/docs/VESA-EEDID-A2.pdf#page=27
    constexpr unsigned VideoInputOffset = 20, WidthCmOffset = 21, HeightCmOffset = 22,
                       GammaOffset = 23, FeaturesOffset = 24;
    data[VideoInputOffset] = 0x80;
    data[WidthCmOffset] = 34;
    data[HeightCmOffset] = 19;
    data[GammaOffset] = 120;
    data[FeaturesOffset] = 0x0a;
    // CIE chromaticity encoding: first two bytes pack low two coordinate bits;
    // remaining bytes give high eight bits of Rx,Ry,Gx,Gy,Bx,By,Wx,Wy.
    // Values approximate sRGB primaries/white point; feature sRGB-default is clear.
    // https://glenwing.github.io/docs/VESA-EEDID-A2.pdf#page=28
    constexpr unsigned ChromaticityOffset = 25;
    constexpr uint8_t SrgbChromaticity[] = {0xee, 0x91, 0xa3, 0x54, 0x4c,
                                            0x99, 0x26, 0x0f, 0x50, 0x54};
    std::copy(std::begin(SrgbChromaticity), std::end(SrgbChromaticity),
              data.begin() + ChromaticityOffset);
    // Standard timing pairs 01h,01h mean unused, not a one-pixel timing.
    // https://glenwing.github.io/docs/VESA-EEDID-A2.pdf#page=31
    constexpr unsigned StandardTimingsBegin = 38, DetailedTimingOffset = 54;
    std::fill(data.begin() + StandardTimingsBegin, data.begin() + DetailedTimingOffset, 1);
    // Detailed timing: 1280x720 at 60 Hz, 74.25 MHz; 1650x750 total pixels.
    // Byte pairs: clock/10kHz=1d01h; active/blanking low + packed high nibbles;
    // H sync front porch=110,width=40; V front porch=5,width=5; high sync bits=0;
    // image=340x190mm; borders=0; flags=1eh digital separate positive H/V sync.
    // https://glenwing.github.io/docs/VESA-EEDID-A2.pdf#page=33
    constexpr uint8_t Preferred720pTiming[] = {0x01, 0x1d, 0x00, 0x72, 0x51, 0xd0,
                                               0x1e, 0x20, 0x6e, 0x28, 0x55, 0x00,
                                               0x54, 0xbe, 0x10, 0x00, 0x00, 0x1e};
    std::copy(std::begin(Preferred720pTiming), std::end(Preferred720pTiming),
              data.begin() + DetailedTimingOffset);
    // Non-timing descriptors are 18 bytes: zero prefix, tag at +3, text/data at +5.
    // FCh is product name; its ASCII text ends with newline and has no NUL terminator.
    // https://glenwing.github.io/docs/VESA-EEDID-A2.pdf#page=43
    constexpr unsigned ProductNameDescriptor = 72, DescriptorTagOffset = 3,
                       DescriptorDataOffset = 5;
    constexpr uint8_t ProductNameTag = 0xfc;
    data[ProductNameDescriptor + DescriptorTagOffset] = ProductNameTag;
    const std::string name = "PCEmu VirtIO\n";
    std::copy(name.begin(), name.end(),
              data.begin() + ProductNameDescriptor + DescriptorDataOffset);
    // FDh range descriptor: 50..75 Hz vertical, 30..90 kHz horizontal,
    // 17*10=170 MHz maximum pixel clock, default GTF (code 0), newline then spaces.
    // https://glenwing.github.io/docs/VESA-EEDID-A2.pdf#page=39
    constexpr unsigned RangeDescriptor = 90;
    constexpr uint8_t RangeTag = 0xfd;
    data[RangeDescriptor + DescriptorTagOffset] = RangeTag;
    enum RangeField : unsigned {
        MinimumVerticalHz = 5,
        MaximumVerticalHz = 6,
        MinimumHorizontalKhz = 7,
        MaximumHorizontalKhz = 8,
        MaximumPixelClock10Mhz = 9,
        TimingSupport = 10,
        Newline = 11,
        PaddingBegin = 12,
        DescriptorEnd = 18,
    };
    data[RangeDescriptor + RangeField::MinimumVerticalHz] = 50;
    data[RangeDescriptor + RangeField::MaximumVerticalHz] = 75;
    data[RangeDescriptor + RangeField::MinimumHorizontalKhz] = 30;
    data[RangeDescriptor + RangeField::MaximumHorizontalKhz] = 90;
    data[RangeDescriptor + RangeField::MaximumPixelClock10Mhz] = 17;
    data[RangeDescriptor + RangeField::TimingSupport] = 0;
    data[RangeDescriptor + RangeField::Newline] = 10;
    std::fill(data.begin() + RangeDescriptor + RangeField::PaddingBegin,
              data.begin() + RangeDescriptor + RangeField::DescriptorEnd, ' ');
    // 10h is a dummy descriptor; extension count stays zero (one base block only).
    // https://glenwing.github.io/docs/VESA-EEDID-A2.pdf#page=37
    constexpr unsigned DummyDescriptor = 108;
    constexpr uint8_t DummyTag = 0x10;
    data[DummyDescriptor + DescriptorTagOffset] = DummyTag;
    // Checksum makes the unsigned sum of all 128 bytes zero modulo 256.
    unsigned sum = 0;
    for (auto byte : data)
        sum += byte;
    data[EdidChecksumOffset] = uint8_t(-sum);
    return data;
}
// Copy a validated slice from the resource’s scatter/gather guest backing into host pixels.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3810008
void VirtioGpu::read_resource_backing(const Resource &r, uint64_t offset,
                                      std::span<uint8_t> output) const {
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
// Execute supported 2D control commands and return a fenced success/error response.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3810008
std::vector<uint8_t> VirtioGpu::execute_control_command(std::span<const uint8_t> request,
                                                        size_t capacity) {
    require(request.size() >= ControlHeaderBytes);
    std::vector<uint8_t> response(ControlHeaderBytes, 0);
    auto type = uint32_t(read_command_field(request, HeaderField::Type));
    // Echo requested fence/context fields in the response header (synchronous 2D completion).
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3800007
    auto header = [&](uint32_t result) {
        write_response_field(response, HeaderField::Type, result);
        auto flags = read_command_field(request, HeaderField::Flags) & FenceFlag;
        write_response_field(response, HeaderField::Flags, flags);
        if (flags)
            write_response_field(response, HeaderField::FenceId,
                                 read_command_field(request, HeaderField::FenceId, 8), 8);
        write_response_field(response, HeaderField::ContextId,
                             read_command_field(request, HeaderField::ContextId));
    };
    try {
        require(!(read_command_field(request, HeaderField::Flags) & ~FenceFlag));
        require(read_command_field(request, HeaderField::ContextId) == 0,
                Response::ErrorInvalidContextId); // 2D mode has no rendering contexts.
        switch (type) {
        case Command::GetDisplayInfo:
            require(capacity >= DisplayInfoResponseBytes);
            response.resize(DisplayInfoResponseBytes, 0);
            write_response_field(response, RectangleField::Width, DefaultWidth);
            write_response_field(response, RectangleField::Height, DefaultHeight);
            write_response_field(response, DisplayInfoEnabledOffset, 1);
            header(Response::OkDisplayInfo);
            return response;
        case Command::GetEdid: {
            // The signed Windows DOD driver queries EDID without acknowledging
            // its feature bit. This read-only command is safe to serve anyway.
            require(request.size() >= ResourceRequestBytes);
            require(read_command_field(request, EdidRequestField::ScanoutId) == 0,
                    Response::ErrorInvalidScanoutId);
            require(capacity >= EdidResponseBytes);
            response.resize(EdidResponseBytes, 0);
            write_response_field(response, EdidResponseSizeOffset, EdidBytes);
            auto bytes = edid();
            std::copy(bytes.begin(), bytes.end(), response.begin() + EdidResponseDataOffset);
            header(Response::OkEdid);
            return response;
        }
        case Command::ResourceCreate2d: {
            require(request.size() >= Create2dRequestBytes);
            auto id = uint32_t(read_command_field(request, Create2dField::ResourceId)),
                 format = uint32_t(read_command_field(request, Create2dField::Format));
            auto width = uint32_t(read_command_field(request, Create2dField::Width)),
                 height = uint32_t(read_command_field(request, Create2dField::Height));
            require(id && !resources.contains(id));
            require(format == PixelFormat::B8G8R8A8Unorm || format == PixelFormat::B8G8R8X8Unorm ||
                    format == PixelFormat::A8R8G8B8Unorm || format == PixelFormat::X8R8G8B8Unorm ||
                    format == PixelFormat::R8G8B8A8Unorm || format == PixelFormat::X8B8G8R8Unorm ||
                    format == PixelFormat::A8B8G8R8Unorm || format == PixelFormat::R8G8B8X8Unorm);
            require(width && height && width <= MaximumDimensionPixels &&
                    height <= MaximumDimensionPixels);
            size_t length = size_t(width) * height * PixelBytes;
            require(resources.size() < MaximumResources &&
                        length <= MaximumResourceBytes - allocated,
                    Response::ErrorOutOfMemory);
            Resource resource{width, height, format, std::vector<uint8_t>(length), {}, 0};
            resources.emplace(id, std::move(resource));
            allocated += length;
            break;
        }
        case Command::ResourceUnreference: {
            require(request.size() >= ResourceRequestBytes);
            auto id = uint32_t(read_command_field(request, ResourceField::ResourceId));
            auto at = resources.find(id);
            require(at != resources.end(), Response::ErrorInvalidResourceId);
            if (scanoutResource == id)
                remember_frame();
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
        case Command::SetScanout: {
            require(request.size() >= ScanoutRequestBytes);
            require(read_command_field(request, ScanoutField::ScanoutId) == 0,
                    Response::ErrorInvalidScanoutId);
            auto id = uint32_t(read_command_field(request, ScanoutField::ResourceId));
            auto box = read_rectangle(request);
            if (id) {
                auto at = resources.find(id);
                require(at != resources.end(), Response::ErrorInvalidResourceId);
                require(contains(at->second, box));
            }
            if (!id)
                remember_frame();
            ++scanoutCommands;
            scanoutResource = id;
            scanout = box;
            ++generation;
            break;
        }
        case Command::ResourceFlush: {
            require(request.size() >= ScanoutRequestBytes);
            auto id = uint32_t(read_command_field(request, FlushField::ResourceId));
            auto at = resources.find(id);
            require(at != resources.end(), Response::ErrorInvalidResourceId);
            require(contains(at->second, read_rectangle(request)));
            ++flushes;
            if (id == scanoutResource)
                ++generation;
            break;
        }
        case Command::TransferToHost2d: {
            require(request.size() >= TransferRequestBytes);
            auto id = uint32_t(read_command_field(request, TransferField::ResourceId));
            auto at = resources.find(id);
            require(at != resources.end(), Response::ErrorInvalidResourceId);
            auto &r = at->second;
            auto box = read_rectangle(request);
            require(contains(r, box));
            auto offset = read_command_field(request, TransferField::Offset, 8);
            uint64_t stride = uint64_t(r.width) * PixelBytes;
            uint64_t end = uint64_t(box.height - 1) * stride + uint64_t(box.width) * PixelBytes;
            require(offset <= r.backingBytes && end <= r.backingBytes - offset);
            for (unsigned y = 0; y < box.height; ++y) {
                size_t destination = (size_t(box.y + y) * r.width + box.x) * PixelBytes;
                read_resource_backing(
                    r, offset + uint64_t(y) * stride,
                    std::span(r.data).subspan(destination, size_t(box.width) * PixelBytes));
            }
            ++transfers;
            break;
        }
        case Command::ResourceAttachBacking: {
            require(request.size() >= ResourceRequestBytes);
            auto id = uint32_t(read_command_field(request, ResourceField::ResourceId));
            auto at = resources.find(id);
            require(at != resources.end(), Response::ErrorInvalidResourceId);
            auto &r = at->second;
            auto count = read_command_field(request, ResourceField::Count);
            require(count && count <= MaximumBackingEntries &&
                    request.size() == BackingField::Entries + count * BackingEntryBytes &&
                    r.backing.empty());
            std::vector<Backing> entries;
            uint64_t length = 0;
            for (size_t i = 0; i < count; ++i) {
                auto address =
                    read_command_field(request, BackingField::Entries + i * BackingEntryBytes, 8);
                auto size = uint32_t(read_command_field(
                    request, BackingField::Entries + BackingField::Length + i * BackingEntryBytes));
                require(size && size <= MaximumResourceBytes - length);
                try {
                    memory(address, size);
                } catch (const std::runtime_error &) {
                    throw CommandError{Response::ErrorInvalidParameter};
                }
                entries.push_back({address, size});
                length += size;
            }
            require(length >= r.data.size());
            r.backing = std::move(entries);
            r.backingBytes = length;
            break;
        }
        case Command::ResourceDetachBacking: {
            require(request.size() >= ResourceRequestBytes);
            auto at =
                resources.find(uint32_t(read_command_field(request, ResourceField::ResourceId)));
            require(at != resources.end(), Response::ErrorInvalidResourceId);
            at->second.backing.clear();
            at->second.backingBytes = 0;
            break;
        }
        default:
            throw CommandError{Response::ErrorUnspecified};
        }
        header(Response::OkNoData);
    } catch (const CommandError &error) {
        lastErrorCommand = type;
        lastErrorResponse = error.type;
        ++commandErrors;
        response.resize(ControlHeaderBytes);
        header(error.type);
    } catch (const std::bad_alloc &) {
        ++commandErrors;
        response.resize(ControlHeaderBytes);
        header(Response::ErrorOutOfMemory);
    }
    return response;
}
// Validate cursor update/move state and advance host display generation; count command errors.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-38300010
void VirtioGpu::execute_cursor_command(std::span<const uint8_t> request) {
    try {
        require(request.size() >= CursorMoveBytes);
        auto type = read_command_field(request, HeaderField::Type);
        require(type == Command::UpdateCursor || type == Command::MoveCursor);
        require(read_command_field(request, CursorField::ScanoutId) == 0,
                Response::ErrorInvalidScanoutId);
        if (type == Command::UpdateCursor) {
            require(request.size() >= CursorUpdateBytes);
            auto id = uint32_t(read_command_field(request, CursorField::ResourceId));
            if (id) {
                auto at = resources.find(id);
                require(at != resources.end(), Response::ErrorInvalidResourceId);
                require(at->second.width == CursorDimensionPixels &&
                        at->second.height == CursorDimensionPixels &&
                        read_command_field(request, CursorField::HotX) < CursorDimensionPixels &&
                        read_command_field(request, CursorField::HotY) < CursorDimensionPixels);
            }
            cursorResource = id;
            hotX = uint32_t(read_command_field(request, CursorField::HotX));
            hotY = uint32_t(read_command_field(request, CursorField::HotY));
        }
        cursorX = uint32_t(read_command_field(request, CursorField::X));
        cursorY = uint32_t(read_command_field(request, CursorField::Y));
        ++generation;
        ++cursorUpdates;
    } catch (const CommandError &) {
        ++commandErrors;
    }
}
// Dispatch a readable command chain to controlq/cursorq and write any control response.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3650007
std::optional<uint32_t> VirtioGpu::handle(unsigned queue, const Chain &chain) {
    ++commands;
    auto request = chain.input(MaximumRequestBytes);
    if (queue == CursorQueue) {
        if (chain.writable)
            throw std::runtime_error("GPU cursor queue must be readable");
        execute_cursor_command(request);
        return 0;
    }
    if (chain.writable < ControlHeaderBytes)
        throw std::runtime_error("GPU response buffer lacks header");
    if (request.size() < ControlHeaderBytes) {
        ++commandErrors;
        std::vector<uint8_t> error(ControlHeaderBytes);
        write_response_field(error, HeaderField::Type, Response::ErrorInvalidParameter);
        chain.output(error);
        return ControlHeaderBytes;
    }
    auto response = execute_control_command(request, chain.writable);
    chain.output(response);
    return uint32_t(response.size());
}
// Decode one supported eight-bit normalized format into host ARGB without forcing opacity.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3810008
uint32_t VirtioGpu::decode_pixel(const Resource &r, size_t index) {
    const auto *p = r.data.data() + index * PixelBytes;
    uint32_t red = 0, green = 0, blue = 0, alpha = ChannelMaximum;
    switch (r.format) {
    case PixelFormat::B8G8R8A8Unorm:
        alpha = p[3];
        [[fallthrough]];
    case PixelFormat::B8G8R8X8Unorm:
        blue = p[0];
        green = p[1];
        red = p[2];
        break;
    case PixelFormat::A8R8G8B8Unorm:
        alpha = p[0];
        [[fallthrough]];
    case PixelFormat::X8R8G8B8Unorm:
        red = p[1];
        green = p[2];
        blue = p[3];
        break;
    case PixelFormat::R8G8B8A8Unorm:
        alpha = p[3];
        [[fallthrough]];
    case PixelFormat::R8G8B8X8Unorm:
        red = p[0];
        green = p[1];
        blue = p[2];
        break;
    case PixelFormat::A8B8G8R8Unorm:
        alpha = p[0];
        [[fallthrough]];
    case PixelFormat::X8B8G8R8Unorm:
        blue = p[1];
        green = p[2];
        red = p[3];
        break;
    }
    return (alpha << 24) | (red << 16) | (green << 8) | blue;
}
// Capture the active scanout as opaque ARGB, optionally alpha-compositing the cursor.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3790006
bool VirtioGpu::read_pixels(std::vector<uint32_t> &output, unsigned &width, unsigned &height,
                            bool compositeCursor) const {
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
                decode_pixel(resource, size_t(scanout.y + y) * resource.width + scanout.x + x) |
                OpaqueAlpha;
    if (compositeCursor && cursorResource) {
        const auto &r = resources.at(cursorResource);
        for (unsigned y = 0; y < CursorDimensionPixels; ++y)
            for (unsigned x = 0; x < CursorDimensionPixels; ++x) {
                int64_t dx = int64_t(cursorX) + x - hotX, dy = int64_t(cursorY) + y - hotY;
                if (dx < 0 || dy < 0 || dx >= width || dy >= height)
                    continue;
                auto foreground = decode_pixel(r, y * CursorDimensionPixels + x);
                unsigned alpha = foreground >> 24;
                auto &background = output[size_t(dy) * width + size_t(dx)];
                uint32_t result = OpaqueAlpha;
                for (unsigned shift : {0u, 8u, 16u})
                    result |=
                        ((((foreground >> shift) & ChannelMaximum) * alpha +
                          ((background >> shift) & ChannelMaximum) * (ChannelMaximum - alpha) +
                          AlphaRoundingBias) /
                         ChannelMaximum)
                        << shift;
                background = result;
            }
    }
    return true;
}
// Capture only when display generation or cursor-compositing mode changed.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3750002
bool VirtioGpu::changed_pixels(std::vector<uint32_t> &output, unsigned &width, unsigned &height,
                               bool compositeCursor) const {
    width = active() ? scanout.width : 0;
    height = active() ? scanout.height : 0;
    if (!active() || (displayedGeneration == generation && displayedComposite == compositeCursor))
        return false;
    read_pixels(output, width, height, compositeCursor);
    displayedComposite = compositeCursor;
    displayedGeneration = generation;
    return true;
}
// Export a separate cursor image/hotspot for the host frontend without compositing it.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3790006
PointerShape VirtioGpu::pointer_shape() const {
    PointerShape shape;
    shape.active = active();
    shape.visible = shape.active && cursorResource;
    shape.revision = generation;
    if (shape.visible) {
        shape.width = shape.height = CursorDimensionPixels;
        shape.hotX = hotX;
        shape.hotY = hotY;
        const auto &resource = resources.at(cursorResource);
        shape.pixels.resize(CursorDimensionPixels * CursorDimensionPixels);
        for (size_t i = 0; i < shape.pixels.size(); ++i)
            shape.pixels[i] = decode_pixel(resource, i);
    }
    return shape;
}
// Describe the active scanout dimensions or report inactive.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3740001
std::string VirtioGpu::mode_description() const {
    return active() ? std::to_string(scanout.width) + "x" + std::to_string(scanout.height) + "x32"
                    : "inactive";
}
// Export current or last captured display as binary PPM; fail if none exists.
// Virtio 1.2 GPU; host capture/resource policy is described in the overview.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3740001
void VirtioGpu::save_ppm(const std::string &path) const {
    std::vector<uint32_t> data;
    unsigned width = 0, height = 0;
    if (!read_pixels(data, width, height)) {
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
