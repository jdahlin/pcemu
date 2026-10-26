// The Video Graphics Array (VGA) converts display memory into pixels through
// several cooperating blocks. The sequencer selects writable memory planes and
// chain-4 addressing; the graphics controller implements read latches, color
// comparisons and masked/rotated writes. The cathode-ray tube controller (CRTC)
// supplies the display start, line stride and visible geometry. The attribute
// controller maps 16-color values into palette indexes. A digital-to-analog
// converter (DAC) palette supplies red/green/blue intensity for indexed pixels.
// Guests access groups through byte index/data I/O ports; Attribute writes
// alternate index/data until a Status1 read returns the flip-flop to index state.
//
// Legacy graphics use four 64 KiB planes, either one bit per plane for each of
// eight pixels per byte, or interleaved bytes for 256-color/chain-4 modes. Bochs'
// Display Interface (DISPI) is a separate 16-bit indexed extension used by the
// video BIOS to offer VESA BIOS Extension (VBE) modes. It exposes a 16 MiB packed
// pixel buffer through a 64 KiB bank window or fixed linear framebuffer, with
// virtual pitch and x/y offsets for panning. Plane storage and packed storage
// are distinct here. The PCI wrapper describes the framebuffer BAR; the machine
// maps memory/ports and provides the synthetic I/O clock. PcDisplay separately
// renders text using BIOS state/font data and presents/captures graphics frames.
//
// IMPLEMENTATION NOTE: Reset installs simplified firmware presets, not power-on
// electrical state. Retrace is simulated by I/O-clock parity; there are no scan
// clocks, blanking interrupts or analog signals. Rendering produces opaque host
// AARRGGBB pixels. Geometry bounds and accepted DISPI modes protect host storage.
// COMPATIBILITY: Both CRTC port aliases are accepted regardless of IOS; decode is
// not gated by RAM-enable/map bits or DISPI.LFB_ENABLED. Unknown ports return zero.
// Most timing and attribute controls are stored without rendering effects. Odd/even
// addressing, full text/font scanout, split-screen/pel panning and CRTC protection
// are not modeled here. Legacy geometry uses a simplified stride/address model.
// FIXME: Implement those documented address/control effects before claiming full
// VGA conformance; write mode 3 must also apply the requested logic operation.
// IBM Hardware Interface Technical Reference, Video Subsystems, 42G2193,
// September 1992, section 2 VGA Function (not the XGA extension in section 3).
// https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=65
// https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=78
// https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=109
// https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=113
// Bochs VBE Display API, version 0.6 history plus B0C5 memory-size extension.
// https://github.com/bochs-emu/VGABIOS/blob/master/vgabios/vbe_display_api.txt
// COMPATIBILITY: Accept B0C0..B0C5 without changing register availability; reject
// zero BPP instead of treating it as eight, and ignore invalid programming instead
// of the API's historical host-panic policy. Banked and linear paths remain decoded.

#include "vga.hpp"
#include <algorithm>
#include <stdexcept>
namespace pc {
// Initialize the board using a simplified BIOS text-mode preset.
// IMPLEMENTATION NOTE: This is post-firmware state, not power-on register contents.
// https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=40
VGA::VGA() {
    set_bios_mode(BiosMode::Text80Columns, true);
}
// Read the selected port, then synthesize retrace/display-inactive from I/O-clock parity.
// COMPATIBILITY: Status1 is 09h/00h on alternating board ticks, not CRT timing.
// IBM Input Status Register 1, printed 2-42.
// https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=68
uint8_t VGA::read_io_register(uint16_t port, uint32_t ioClock) {
    auto value = read_register(port);
    return port == Register::InputStatus1Register
               ? ((ioClock & 1) ? VerticalRetrace | DisplayInactive : 0)
               : value;
}
// Read DISPI state; GETCAPS reports host limits and virtual height is memory/stride.
// Bochs VBE Display API B0C1 virtual geometry and B0C3 capabilities.
// https://github.com/bochs-emu/VGABIOS/blob/master/vgabios/vbe_display_api.txt#L217
uint16_t VGA::read_display_interface_register() const {
    if (dispiIndex >= dispi.size())
        return 0;
    if (dispi[DisplayInterfaceRegister::EnableRegister] & GetCapabilities) {
        if (dispiIndex == DisplayInterfaceRegister::HorizontalResolutionRegister)
            return maxWidth;
        if (dispiIndex == DisplayInterfaceRegister::VerticalResolutionRegister)
            return maxHeight;
        if (dispiIndex == DisplayInterfaceRegister::BitsPerPixelRegister)
            return Bgrx32BitsPerPixel;
    }
    if (dispiIndex == DisplayInterfaceRegister::VirtualHeightRegister) {
        unsigned stride = unsigned(dispi[DisplayInterfaceRegister::VirtualWidthRegister]) *
                          ((dispi[DisplayInterfaceRegister::BitsPerPixelRegister] + 7) / 8);
        return stride ? std::min(unsigned(UINT16_MAX), vramSize / stride) : 0;
    }
    return dispi[dispiIndex];
}
// Validate/store DISPI mode, bank or pan settings; enabling sets geometry and may clear VRAM.
// IMPLEMENTATION NOTE: Invalid writes are ignored, and mode changes are gated on disabled state.
// Bochs VBE Display API register operations.
// https://github.com/bochs-emu/VGABIOS/blob/master/vgabios/vbe_display_api.txt#L164
void VGA::write_display_interface_register(uint16_t value) {
    if (dispiIndex >= dispi.size())
        return;
    auto bytes = [](unsigned bpp) { return (bpp + 7) / 8; };
    switch (dispiIndex) {
    case DisplayInterfaceRegister::IdentificationRegister:
        if (value >= FirstDisplayInterfaceId && value <= DisplayInterfaceId)
            dispi[DisplayInterfaceRegister::IdentificationRegister] = value;
        break;
    case DisplayInterfaceRegister::HorizontalResolutionRegister:
    case DisplayInterfaceRegister::VerticalResolutionRegister:
    case DisplayInterfaceRegister::BitsPerPixelRegister:
        if (!(dispi[DisplayInterfaceRegister::EnableRegister] & DisplayInterfaceEnabled))
            dispi[dispiIndex] = value;
        break;
    case DisplayInterfaceRegister::EnableRegister: {
        bool wasEnabled = dispi[DisplayInterfaceRegister::EnableRegister] & DisplayInterfaceEnabled;
        if (value & DisplayInterfaceEnabled) {
            unsigned bpp = dispi[DisplayInterfaceRegister::BitsPerPixelRegister];
            if (!dispi[DisplayInterfaceRegister::HorizontalResolutionRegister] ||
                dispi[DisplayInterfaceRegister::HorizontalResolutionRegister] > maxWidth ||
                !dispi[DisplayInterfaceRegister::VerticalResolutionRegister] ||
                dispi[DisplayInterfaceRegister::VerticalResolutionRegister] > maxHeight ||
                (bpp != IndexedBitsPerPixel && bpp != Rgb555BitsPerPixel &&
                 bpp != Rgb565BitsPerPixel && bpp != Bgr24BitsPerPixel &&
                 bpp != Bgrx32BitsPerPixel))
                return;
            if (!wasEnabled) {
                dispi[DisplayInterfaceRegister::VirtualWidthRegister] =
                    dispi[DisplayInterfaceRegister::HorizontalResolutionRegister];
                dispi[DisplayInterfaceRegister::HorizontalOffsetRegister] =
                    dispi[DisplayInterfaceRegister::VerticalOffsetRegister] = 0;
                if (!(value & NoClearMemory))
                    std::fill(vram.begin(), vram.end(), 0);
            }
            graphics = true;
            width = dispi[DisplayInterfaceRegister::HorizontalResolutionRegister];
            height = dispi[DisplayInterfaceRegister::VerticalResolutionRegister];
        }
        dispi[DisplayInterfaceRegister::EnableRegister] = value & DisplayInterfaceEnableMask;
        if (!(value & DisplayInterfaceEnabled))
            update_display_geometry();
        break;
    }
    case DisplayInterfaceRegister::BankRegister:
        if (value < vramSize / PlaneBytes)
            dispi[DisplayInterfaceRegister::BankRegister] = value;
        break;
    case DisplayInterfaceRegister::VirtualWidthRegister:
        if (value >= dispi[DisplayInterfaceRegister::HorizontalResolutionRegister] && value &&
            uint64_t(value) * bytes(dispi[DisplayInterfaceRegister::BitsPerPixelRegister]) *
                    dispi[DisplayInterfaceRegister::VerticalResolutionRegister] <=
                vramSize)
            dispi[DisplayInterfaceRegister::VirtualWidthRegister] = value;
        break;
    case DisplayInterfaceRegister::HorizontalOffsetRegister:
    case DisplayInterfaceRegister::VerticalOffsetRegister: {
        unsigned x = dispiIndex == DisplayInterfaceRegister::HorizontalOffsetRegister
                         ? value
                         : dispi[DisplayInterfaceRegister::HorizontalOffsetRegister];
        unsigned y = dispiIndex == DisplayInterfaceRegister::VerticalOffsetRegister
                         ? value
                         : dispi[DisplayInterfaceRegister::VerticalOffsetRegister];
        if (uint64_t(x) + dispi[DisplayInterfaceRegister::HorizontalResolutionRegister] <=
                dispi[DisplayInterfaceRegister::VirtualWidthRegister] &&
            (uint64_t(y) + dispi[DisplayInterfaceRegister::VerticalResolutionRegister]) *
                    dispi[DisplayInterfaceRegister::VirtualWidthRegister] *
                    bytes(dispi[DisplayInterfaceRegister::BitsPerPixelRegister]) <=
                vramSize)
            dispi[dispiIndex] = value;
        break;
    }
    default:
        break;
    }
}
// Disable DISPI and install simplified BIOS register/palette presets; optionally clear planes.
// IMPLEMENTATION NOTE: Only selected VGA graphics modes have distinct dimensions;
// this is a host BIOS helper, not a replacement for programming full timing registers.
// IBM Type 1 Subsystem Parameters / Graphics Modes.
// https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=40
void VGA::set_bios_mode(unsigned mode, bool clear) {
    dispi[DisplayInterfaceRegister::EnableRegister] = 0;
    graphics =
        mode >= BiosMode::Graphics320x200Color16 && mode <= BiosMode::Graphics320x200Color256;
    width = mode == BiosMode::Graphics320x200Color256 || mode == BiosMode::Graphics320x200Color16
                ? 320
                : 640;
    height = mode == BiosMode::Graphics640x480Color16 || mode == BiosMode::Graphics640x480Monochrome
                 ? 480
             : mode == BiosMode::Graphics640x350Color16 ? 350
                                                        : 200;
    std::fill(std::begin(seq), std::end(seq), 0);
    std::fill(std::begin(gfx), std::end(gfx), 0);
    std::fill(std::begin(crtc), std::end(crtc), 0);
    seq[SequencerRegister::MapMaskRegister] = AllPlanes;
    seq[SequencerRegister::MemoryModeRegister] = mode == BiosMode::Graphics320x200Color256
                                                     ? Chain4 | OddEvenDisable | ExtendedMemory
                                                     : OddEvenDisable | ExtendedMemory;
    gfx[GraphicsRegister::GraphicsModeRegister] =
        mode == BiosMode::Graphics320x200Color256 ? Color256Mode : 0;
    gfx[GraphicsRegister::MiscellaneousRegister] =
        graphics ? GraphicsMode | MemoryMapA0000 : GraphicsOddEven | MemoryMapB8000;
    gfx[GraphicsRegister::ColorDontCareRegister] = AllPlanes;
    gfx[GraphicsRegister::BitMaskRegister] = 255;
    crtc[CrtcRegister::HorizontalDisplayEndRegister] =
        width / (mode == BiosMode::Graphics320x200Color256 ? 4 : 8) - 1;
    unsigned displayEnd = height * (mode == BiosMode::Graphics320x200Color256 ? 2 : 1) - 1;
    crtc[CrtcRegister::VerticalDisplayEndRegister] = displayEnd;
    crtc[CrtcRegister::OverflowRegister] =
        ((displayEnd >> 7) & VerticalDisplayEndBit8) | ((displayEnd >> 3) & VerticalDisplayEndBit9);
    crtc[CrtcRegister::MaximumScanLineRegister] = mode == BiosMode::Graphics320x200Color256 ? 1 : 0;
    crtc[CrtcRegister::OffsetRegister] =
        width / (mode == BiosMode::Graphics320x200Color256 ? 8 : 16);
    // Firmware palette maps color 6 to brown (DAC 14h) and bright colors to
    // DAC 38h..3fh. These are palette indexes, not packed RGB literals.
    const uint8_t DefaultAttributePalette[16] = {0,  1,  2,  3,  4,  5,  20, 7,
                                                 56, 57, 58, 59, 60, 61, 62, 63};
    for (unsigned i = 0; i < 16; i++)
        attr[i] = DefaultAttributePalette[i];
    // EGA-compatible 64-color DAC preset: each RGB component combines 2/3
    // intensity (42/63) and 1/3 intensity (21/63), selected by separate index bits.
    constexpr unsigned PrimaryIntensity = 42, SecondaryIntensity = 21;
    for (unsigned i = 0; i < 64; i++) {
        dac[i][0] = ((i & 4) ? PrimaryIntensity : 0) + ((i & 32) ? SecondaryIntensity : 0);
        dac[i][1] = ((i & 2) ? PrimaryIntensity : 0) + ((i & 16) ? SecondaryIntensity : 0);
        dac[i][2] = ((i & 1) ? PrimaryIntensity : 0) + ((i & 8) ? SecondaryIntensity : 0);
    }
    if (clear)
        for (auto &plane : planes)
            plane.fill(0);
}
// Infer visible logical graphics rows/columns from CRTC end/repeat fields.
// IMPLEMENTATION NOTE: Ignore incomplete/out-of-bounds programming; text geometry belongs to
// PcDisplay. IBM Horizontal/Vertical Display-Enable End, Overflow and Maximum Scan Line.
// https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=88
void VGA::update_display_geometry() {
    if (dispi[DisplayInterfaceRegister::EnableRegister] & DisplayInterfaceEnabled)
        return;
    graphics = gfx[GraphicsRegister::MiscellaneousRegister] & GraphicsMode;
    if (!graphics)
        return;
    unsigned horizontal = (unsigned(crtc[CrtcRegister::HorizontalDisplayEndRegister]) + 1) *
                          ((gfx[GraphicsRegister::GraphicsModeRegister] & Color256Mode) ? 4 : 8);
    // Reassemble vertical-end bits 9:0; register stores final scan-line index,
    // so add one before dividing by character-row repeat/double-scan factor.
    unsigned vertical = unsigned(crtc[CrtcRegister::VerticalDisplayEndRegister]) |
                        ((crtc[CrtcRegister::OverflowRegister] & VerticalDisplayEndBit8) << 7) |
                        ((crtc[CrtcRegister::OverflowRegister] & VerticalDisplayEndBit9) << 3);
    unsigned repeat =
        (unsigned(crtc[CrtcRegister::MaximumScanLineRegister] & MaximumScanLineMask) + 1) *
        ((crtc[CrtcRegister::MaximumScanLineRegister] & DoubleScanning) ? 2 : 1);
    unsigned rows = (vertical + 1) / repeat;
    // Ignore transient/incomplete register sets while a mode is programmed.
    if (horizontal >= MinimumLegacyWidth && horizontal <= MaximumLegacyWidth)
        width = horizontal;
    if (rows >= MinimumLegacyHeight && rows <= MaximumLegacyHeight)
        height = rows;
}
// Read packed linear/banked memory, or latch all legacy planes and apply the selected read mode.
// Board memory decode is supplied by the machine; low address bits select the plane offset.
// IBM Memory Read Operations and Sequencer Memory Mode.
// https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=39
uint8_t VGA::read_memory(uint32_t a) {
    if (a >= lfbBase && a - lfbBase < vramSize)
        return vram[a - lfbBase];
    if (dispi[DisplayInterfaceRegister::EnableRegister] & DisplayInterfaceEnabled)
        return vram[unsigned(dispi[DisplayInterfaceRegister::BankRegister]) * PlaneBytes +
                    (a & PlaneAddressMask)];
    unsigned plane = gfx[GraphicsRegister::ReadMapSelectRegister] & (PlaneCount - 1),
             off = a & PlaneAddressMask;
    if (seq[SequencerRegister::MemoryModeRegister] & Chain4) {
        plane = off & 3;
        off >>= 2;
    }
    for (unsigned p = 0; p < PlaneCount; p++)
        latch[p] = planes[p][off];
    // A memory read latches all maps first. Read mode 1 compares each pixel's
    // four plane bits to Color Compare, including only enabled Don't Care planes.
    // IBM Memory Read Operations / Color Compare, printed 2-13 and 2-79.
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=39
    if (gfx[GraphicsRegister::GraphicsModeRegister] & ColorCompareReadMode) {
        uint8_t result = 255;
        for (unsigned p = 0; p < PlaneCount; p++)
            if (gfx[GraphicsRegister::ColorDontCareRegister] & (1 << p))
                result &=
                    latch[p] ^ ((gfx[GraphicsRegister::ColorCompareRegister] & (1 << p)) ? 0 : 255);
        return result;
    }
    return latch[plane];
}
// Write packed memory or apply plane selection, rotation, set/reset, logic and bit masking.
// Latches retain the preceding read; write mode 1 copies them unchanged.
// IBM Memory Write Operations / Write Mode Definitions.
// https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=109
void VGA::write_memory(uint32_t a, uint8_t v) {
    if (a >= lfbBase && a - lfbBase < vramSize) {
        vram[a - lfbBase] = v;
        return;
    }
    if (dispi[DisplayInterfaceRegister::EnableRegister] & DisplayInterfaceEnabled) {
        vram[unsigned(dispi[DisplayInterfaceRegister::BankRegister]) * PlaneBytes +
             (a & PlaneAddressMask)] = v;
        return;
    }
    unsigned off = a & PlaneAddressMask, mask = seq[SequencerRegister::MapMaskRegister] & AllPlanes;
    if (seq[SequencerRegister::MemoryModeRegister] & Chain4) {
        mask &= 1 << (off & 3);
        off >>= 2;
    }
    unsigned rotate = gfx[GraphicsRegister::DataRotateRegister] & RotateCountMask,
             logic = (gfx[GraphicsRegister::DataRotateRegister] >> LogicalFunctionShift) &
                     LogicalFunctionMask,
             mode = gfx[GraphicsRegister::GraphicsModeRegister] & WriteModeMask;
    uint8_t rotated = (v >> rotate) | (v << ((8 - rotate) & 7));
    for (unsigned p = 0; p < PlaneCount; p++)
        if (mask & (1 << p)) {
            uint8_t data = rotated, bitmask = gfx[GraphicsRegister::BitMaskRegister];
            if (mode == WriteMode::CopyLatch) {
                planes[p][off] = latch[p];
                continue;
            }
            if (mode == WriteMode::RotateSetReset &&
                (gfx[GraphicsRegister::EnableSetResetRegister] & (1 << p)))
                data = (gfx[GraphicsRegister::SetResetRegister] & (1 << p)) ? 255 : 0;
            if (mode == WriteMode::ExpandPlaneBits)
                data = (v & (1 << p)) ? 255 : 0;
            if (mode == WriteMode::MaskedSetReset) {
                data = (gfx[GraphicsRegister::SetResetRegister] & (1 << p)) ? 255 : 0;
                bitmask &= rotated;
            }
            // COMPATIBILITY: Mode 3 currently bypasses the selected logic function.
            // FIXME: Apply Data Rotate.Function Select to mode 3 as required by
            // IBM Write Mode Definitions, printed 2-83, without changing this migration.
            // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=109
            if (mode != WriteMode::MaskedSetReset) {
                if (logic == LogicalFunction::And)
                    data &= latch[p];
                else if (logic == LogicalFunction::Or)
                    data |= latch[p];
                else if (logic == LogicalFunction::Xor)
                    data ^= latch[p];
            }
            planes[p][off] = (data & bitmask) | (latch[p] & ~bitmask);
        }
}
// Read a VGA indexed register or DAC component, with automatic RGB/address progression.
// Input Status1 returns 09h and resets the Attribute flip-flop; unknown reads return zero.
// IBM Registers / Video DAC to System Interface.
// https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=65
uint8_t VGA::read_register(uint16_t p) {
    switch (p) {
    case Register::MiscellaneousOutputReadRegister:
        return misc;
    case Register::InputStatus0Register:
        return MonitorSwitchSense;
    case Register::AttributeDataRegister:
        return attr[attrIndex & AttributeIndexMask];
    case Register::SequencerAddressRegister:
        return seqIndex;
    case Register::SequencerDataRegister:
        return seq[seqIndex & SequencerIndexMask];
    case Register::GraphicsAddressRegister:
        return gfxIndex;
    case Register::GraphicsDataRegister:
        return gfx[gfxIndex & GraphicsIndexMask];
    case Register::ColorCrtcAddressRegister:
    case Register::MonochromeCrtcAddressRegister:
        return crtcIndex;
    case Register::ColorCrtcDataRegister:
    case Register::MonochromeCrtcDataRegister:
        return crtc[crtcIndex & CrtcIndexMask];
    case Register::PelMaskRegister:
        return dacMask;
    case Register::PaletteWriteAddressRegister:
        return dacWrite;
    case Register::PaletteDataRegister: {
        auto v = dac[dacRead][dacComponent++];
        if (dacComponent == ColorComponents) {
            dacComponent = 0;
            dacRead++;
        }
        return v;
    }
    case Register::InputStatus1Register:
        attrData = false;
        return VerticalRetrace | DisplayInactive;
    default:
        return 0;
    }
}
// Select/store VGA registers, toggle the Attribute index/data state, or advance DAC RGB writes.
// Geometry-affecting group writes immediately attempt host geometry recalculation.
// IBM Attribute Controller / Video DAC to System Interface.
// https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=113
void VGA::write_register(uint16_t p, uint8_t v) {
    switch (p) {
    case Register::MiscellaneousOutputWriteRegister:
        misc = v;
        break;
    case Register::AttributeAddressDataRegister:
        if (!attrData)
            attrIndex = v & AttributeIndexMask;
        else
            attr[attrIndex] = v;
        attrData = !attrData;
        break;
    case Register::SequencerAddressRegister:
        seqIndex = v;
        break;
    case Register::SequencerDataRegister:
        seq[seqIndex & SequencerIndexMask] = v;
        break;
    case Register::GraphicsAddressRegister:
        gfxIndex = v;
        break;
    case Register::GraphicsDataRegister:
        gfx[gfxIndex & GraphicsIndexMask] = v;
        break;
    case Register::ColorCrtcAddressRegister:
    case Register::MonochromeCrtcAddressRegister:
        crtcIndex = v;
        break;
    case Register::ColorCrtcDataRegister:
    case Register::MonochromeCrtcDataRegister:
        crtc[crtcIndex & CrtcIndexMask] = v;
        break;
    case Register::PelMaskRegister:
        dacMask = v;
        break;
    case Register::PaletteReadAddressRegister:
        dacRead = v;
        dacComponent = 0;
        break;
    case Register::PaletteWriteAddressRegister:
        dacWrite = v;
        dacComponent = 0;
        break;
    case Register::PaletteDataRegister:
        dac[dacWrite][dacComponent++] =
            v & ((dispi[DisplayInterfaceRegister::EnableRegister] & EightBitDac) ? EightBitColorMask
                                                                                 : SixBitColorMask);
        if (dacComponent == ColorComponents) {
            dacComponent = 0;
            dacWrite++;
        }
        break;
    default:
        break;
    }
    if (p == Register::SequencerDataRegister || p == Register::GraphicsDataRegister ||
        p == Register::ColorCrtcDataRegister || p == Register::MonochromeCrtcDataRegister)
        update_display_geometry();
}
// Decode a graphics pixel through packed formats or legacy plane/palette logic to opaque AARRGGBB.
// IMPLEMENTATION NOTE: Host rendering interprets only the implemented address/format subset;
// out-of-bounds pixels return black. IBM Video Memory Organization / DAC.
// https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=49
uint32_t VGA::pixel_color(unsigned x, unsigned y) const {
    if (x >= width || y >= height)
        return OpaqueBlack;
    uint8_t color = 0;
    if (dispi[DisplayInterfaceRegister::EnableRegister] & DisplayInterfaceEnabled) {
        unsigned bpp = dispi[DisplayInterfaceRegister::BitsPerPixelRegister], count = (bpp + 7) / 8;
        uint64_t at = ((uint64_t(y) + dispi[DisplayInterfaceRegister::VerticalOffsetRegister]) *
                           dispi[DisplayInterfaceRegister::VirtualWidthRegister] +
                       x + dispi[DisplayInterfaceRegister::HorizontalOffsetRegister]) *
                      count;
        if (at + count > vram.size())
            return OpaqueBlack;
        uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i)
            value |= uint32_t(vram[at + i]) << (i * 8);
        // Little-endian packed pixels: low byte blue, next green, then red;
        // unused top byte is discarded and host output supplies opaque alpha.
        if (bpp == Bgr24BitsPerPixel || bpp == Bgrx32BitsPerPixel)
            return OpaqueBlack | (value & RgbMask);
        if (bpp == Rgb555BitsPerPixel || bpp == Rgb565BitsPerPixel) {
            // RGB555 has 5/5/5 bits; RGB565 gives green six bits. Scale each
            // unsigned component from its full range (31 or 63) to 0..255.
            unsigned greenBits = bpp == Rgb565BitsPerPixel ? 6 : 5;
            unsigned blue = value & 31, green = (value >> 5) & ((1 << greenBits) - 1);
            unsigned red = (value >> (5 + greenBits)) & 31;
            return OpaqueBlack | ((red * 255 / 31) << 16) |
                   ((green * 255 / ((1 << greenBits) - 1)) << 8) | (blue * 255 / 31);
        }
        color = value & dacMask;
        // Expand six DAC bits to eight by replication; 8BIT_DAC passes through.
        auto expand = [&](uint8_t v) {
            return (dispi[DisplayInterfaceRegister::EnableRegister] & EightBitDac)
                       ? unsigned(v)
                       : unsigned((v << 2) | (v >> 4));
        };
        return OpaqueBlack | (expand(dac[color][0]) << 16) | (expand(dac[color][1]) << 8) |
               expand(dac[color][2]);
    }
    unsigned start = ((crtc[CrtcRegister::StartAddressHighRegister] << 8) |
                      crtc[CrtcRegister::StartAddressLowRegister]);
    if (gfx[GraphicsRegister::GraphicsModeRegister] & Color256Mode) {
        unsigned stride =
            crtc[CrtcRegister::OffsetRegister] ? crtc[CrtcRegister::OffsetRegister] * 2 : width / 4;
        color = planes[x & 3][(start + y * stride + x / 4) & PlaneAddressMask];
    } else {
        unsigned stride =
            crtc[CrtcRegister::OffsetRegister] ? crtc[CrtcRegister::OffsetRegister] * 2 : width / 8;
        unsigned a = (start + y * stride + x / 8) & PlaneAddressMask;
        for (unsigned p = 0; p < PlaneCount; p++)
            if (planes[p][a] & (128 >> (x & 7)))
                color |= 1 << p;
        color = attr[color] & SixBitColorMask;
    }
    color &= dacMask;
    auto expand = [](uint8_t v) { return uint32_t((v << 2) | (v >> 4)); };
    return OpaqueBlack | (expand(dac[color][0]) << 16) | (expand(dac[color][1]) << 8) |
           expand(dac[color][2]);
}
// Render a stable graphics state into a bounded host frame; throw if destination storage is too
// small. IMPLEMENTATION NOTE: Cache the palette per frame and batch planar reads; other packed
// formats share pixel_color. This changes host work, not the guest memory layout.
// IBM Video Memory Organization / DAC.
// https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=49
void VGA::render_graphics(std::span<uint32_t> pixels, unsigned frameWidth,
                          unsigned frameHeight) const {
    if (frameHeight && frameWidth > pixels.size() / frameHeight)
        throw std::invalid_argument("VGA frame buffer is too small");
    // Snapshot the palette once per frame, rather than expanding DAC components
    // and decoding display mode for every pixel. Other packed formats retain
    // the reference pixel path.
    const bool packed = dispi[DisplayInterfaceRegister::EnableRegister] & DisplayInterfaceEnabled;
    if (packed && dispi[DisplayInterfaceRegister::BitsPerPixelRegister] != IndexedBitsPerPixel) {
        for (unsigned y = 0; y < frameHeight; ++y)
            for (unsigned x = 0; x < frameWidth; ++x)
                pixels[size_t(y) * frameWidth + x] = pixel_color(x, y);
        return;
    }
    std::array<uint32_t, PaletteEntries> colors;
    for (unsigned i = 0; i < colors.size(); ++i) {
        unsigned index = i & dacMask;
        auto expand = [&](uint8_t v) -> uint32_t {
            return packed && (dispi[DisplayInterfaceRegister::EnableRegister] & EightBitDac)
                       ? v
                       : unsigned((v << 2) | (v >> 4));
        };
        colors[i] = OpaqueBlack | (expand(dac[index][0]) << 16) | (expand(dac[index][1]) << 8) |
                    expand(dac[index][2]);
    }
    const unsigned start = (crtc[CrtcRegister::StartAddressHighRegister] << 8) |
                           crtc[CrtcRegister::StartAddressLowRegister];
    const bool indexed = gfx[GraphicsRegister::GraphicsModeRegister] & Color256Mode;
    const unsigned stride = crtc[CrtcRegister::OffsetRegister]
                                ? crtc[CrtcRegister::OffsetRegister] * 2
                                : width / (indexed ? 4 : 8);
    for (unsigned y = 0; y < frameHeight; ++y) {
        auto row = pixels.subspan(size_t(y) * frameWidth, frameWidth);
        std::fill(row.begin(), row.end(), OpaqueBlack);
        if (y >= height)
            continue;
        unsigned end = std::min(frameWidth, width);
        if (packed) {
            uint64_t at = (uint64_t(y) + dispi[DisplayInterfaceRegister::VerticalOffsetRegister]) *
                              dispi[DisplayInterfaceRegister::VirtualWidthRegister] +
                          dispi[DisplayInterfaceRegister::HorizontalOffsetRegister];
            for (unsigned x = 0; x < end && at + x < vram.size(); ++x)
                row[x] = colors[vram[at + x]];
        } else if (indexed) {
            for (unsigned x = 0; x < end; ++x)
                row[x] = colors[planes[x & 3][(start + y * stride + x / 4) & PlaneAddressMask]];
        } else {
            for (unsigned x = 0; x < end; x += 8) {
                unsigned at = (start + y * stride + x / 8) & PlaneAddressMask;
                const uint8_t p0 = planes[0][at], p1 = planes[1][at], p2 = planes[2][at],
                              p3 = planes[3][at];
                for (unsigned bit = 0; bit < 8 && x + bit < end; ++bit) {
                    unsigned shift = 7 - bit;
                    unsigned color = ((p0 >> shift) & 1) | (((p1 >> shift) & 1) << 1) |
                                     (((p2 >> shift) & 1) << 2) | (((p3 >> shift) & 1) << 3);
                    row[x + bit] = colors[attr[color] & SixBitColorMask];
                }
            }
        }
    }
}
} // namespace pc
