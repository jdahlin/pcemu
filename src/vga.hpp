#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>
namespace pc {
// IBM VGA register baseline: Hardware Interface Technical Reference, Video
// Subsystems, 42G2193, September 1992, section 2 VGA Function. No XGA emulation.
// https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=65
// Bochs Display Interface API 0.6 plus B0C5 history (16 MiB memory extension).
// https://github.com/bochs-emu/VGABIOS/blob/master/vgabios/vbe_display_api.txt
struct VGA {
    // Absolute I/O ports (not offsets). Index/data pairs select register groups;
    // CRTC B/D aliases are both accepted here, regardless of Misc.Output.IOS.
    // IBM Registers / General Registers, printed 2-39..2-44.
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=65
    enum Register : uint16_t {
        AttributeAddressDataRegister = 0x3c0,
        AttributeDataRegister = 0x3c1, // Alternating index/data writes; separate read.
        MiscellaneousOutputWriteRegister = 0x3c2,
        InputStatus0Register = 0x3c2,
        SequencerAddressRegister = 0x3c4,
        SequencerDataRegister = 0x3c5,
        PelMaskRegister = 0x3c6, // Mask palette index before DAC lookup (pel means pixel).
        PaletteReadAddressRegister = 0x3c7,
        PaletteWriteAddressRegister = 0x3c8,
        PaletteDataRegister = 0x3c9, // Successive red, green, blue components advance the address.
        MiscellaneousOutputReadRegister = 0x3cc,
        GraphicsAddressRegister = 0x3ce,
        GraphicsDataRegister = 0x3cf,
        MonochromeCrtcAddressRegister = 0x3b4,
        MonochromeCrtcDataRegister = 0x3b5,
        ColorCrtcAddressRegister = 0x3d4,
        ColorCrtcDataRegister = 0x3d5,
        MonochromeInputStatus1Register = 0x3ba, // Active status alias in monochrome modes.
        InputStatus1Register = 0x3da, // Read also returns the Attribute flip-flop to index state.
    };
    // Sequencer indexes: Map Mask enables writable planes; Memory Mode controls
    // chain-4 (low two address bits choose plane), odd/even and 256 KiB access.
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=71
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=78
    enum SequencerRegister : unsigned { MapMaskRegister = 2, MemoryModeRegister = 4 };
    static constexpr uint8_t AllPlanes = 0x0f, Chain4 = 8, OddEvenDisable = 4, ExtendedMemory = 2;
    static constexpr uint8_t SequencerIndexMask = 7, GraphicsIndexMask = 15, CrtcIndexMask = 31,
                             AttributeIndexMask = 31;
    // Graphics indexes: set/reset broadcasts one bit to all eight pixels of each
    // plane; Enable Set/Reset selects it in write mode 0; Compare/Don't Care
    // select matches for read mode 1; Rotate selects CPU rotation/latched logic;
    // Read Map picks a plane; Mode selects read/write/256-color paths; Misc
    // chooses graphics/address mapping; Bit Mask preserves unselected latched bits.
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=102
    enum GraphicsRegister : unsigned {
        SetResetRegister = 0,
        EnableSetResetRegister = 1,
        ColorCompareRegister = 2,
        DataRotateRegister = 3,
        ReadMapSelectRegister = 4,
        GraphicsModeRegister = 5,
        MiscellaneousRegister = 6,
        ColorDontCareRegister = 7,
        BitMaskRegister = 8,
    };
    // Read Mode bit 3, Write Mode bits [1:0], 256-Color Mode bit 6.
    // Graphics Misc bit 0 chooses graphics; map encodings select A0000/B8000 windows.
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=108
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=110
    static constexpr uint8_t ColorCompareReadMode = 8, WriteModeMask = 3, Color256Mode = 0x40,
                             GraphicsMode = 1, MemoryMapA0000 = 4, MemoryMapB8000 = 0x0c,
                             GraphicsOddEven = 2;
    // Graphics Data Rotate: [2:0] rotates right; [4:3] choose replace/AND/OR/XOR.
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=106
    static constexpr unsigned RotateCountMask = 7, LogicalFunctionShift = 3,
                              LogicalFunctionMask = 3;
    // GR05.OE controls read-plane parity; SR04.OE controls write-plane parity.
    // GR05.SR interleaves two packed color bits for CGA modes 4/5.
    // IBM 42G2193 Memory Mode / Graphics Mode, PDF 78/108.
    static constexpr uint8_t OddEvenReadAddressing = 0x10, InterleavedShiftRegister = 0x20;
    static constexpr unsigned AttributeModeControl = 0x10, AttributeColorSelect = 0x14;
    static constexpr uint8_t PaletteAddressSource = 0x20, PaletteAddressMask = 0x3f;
    enum LogicalFunction : unsigned { Replace = 0, And = 1, Or = 2, Xor = 3 };
    // Write modes 0..3: rotated CPU/set-reset, copied latch, expanded CPU plane
    // bits, and set/reset with rotated CPU byte as additional mask.
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=109
    enum WriteMode : unsigned {
        RotateSetReset = 0,
        CopyLatch = 1,
        ExpandPlaneBits = 2,
        MaskedSetReset = 3
    };
    // Cathode-ray tube controller (CRTC) indexes determine visible geometry and
    // display start/row stride. Vertical end is split across low byte and Overflow
    // bits 1 (bit 8) / 6 (bit 9); Maximum Scan Line repeats rows and double scans.
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=86
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=88
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=91
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=95
    enum CrtcRegister : unsigned {
        HorizontalDisplayEndRegister = 1,
        OverflowRegister = 7,
        PresetRowScanRegister = 8,
        MaximumScanLineRegister = 9,
        StartAddressHighRegister = 0x0c,
        StartAddressLowRegister = 0x0d,
        VerticalDisplayEndRegister = 0x12,
        OffsetRegister = 0x13,
        ModeControlRegister = 0x17,
        LineCompareRegister = 0x18,
    };
    static constexpr uint8_t VerticalDisplayEndBit8 = 2, VerticalDisplayEndBit9 = 0x40,
                             MaximumScanLineMask = 31, DoubleScanning = 0x80;
    // Misc.Output 67h: positive vertical/negative horizontal sync, 28.322 MHz,
    // RAM decode enabled and color CRTC addresses. This is a firmware-style preset.
    // Status0 bit 4 is switch sense; Status1 bit 3 vertical retrace, bit 0 display inactive.
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=66
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=68
    static constexpr uint8_t InitialMiscellaneousOutput = 0x67, MonitorSwitchSense = 0x10,
                             VerticalRetrace = 8, DisplayInactive = 1;
    // Bochs DISPI indexes (16-bit data), independent of IBM's indexed byte groups.
    // https://github.com/bochs-emu/VGABIOS/blob/master/vgabios/vbe_display_api.txt#L100
    enum DisplayInterfaceRegister : unsigned {
        IdentificationRegister = 0,
        HorizontalResolutionRegister = 1,
        VerticalResolutionRegister = 2,
        BitsPerPixelRegister = 3,
        EnableRegister = 4,
        BankRegister = 5,
        VirtualWidthRegister = 6,
        VirtualHeightRegister = 7,
        HorizontalOffsetRegister = 8,
        VerticalOffsetRegister = 9,
        VideoMemory64KRegister = 10,
    };
    // B0C0..B0C5 are supported API versions, not part numbers. B0C5 adds the
    // video-memory-size register in 64 KiB units; 256 such units means 16 MiB.
    // https://github.com/bochs-emu/VGABIOS/blob/master/vgabios/vbe_display_api.txt#L14
    static constexpr uint16_t FirstDisplayInterfaceId = 0xb0c0, DisplayInterfaceId = 0xb0c5;
    // Enable bit 0 starts packed pixels, GETCAPS returns limits, 8BIT_DAC selects
    // full-byte palette components, LFB_ENABLED selects linear memory, NOCLEARMEM
    // preserves contents. LFB_ENABLED is retained but decoding is host policy here.
    // https://github.com/bochs-emu/VGABIOS/blob/master/vgabios/vbe_display_api.txt#L246
    static constexpr uint16_t DisplayInterfaceEnabled = 1, GetCapabilities = 2, EightBitDac = 0x20,
                              LinearFramebufferEnabled = 0x40, NoClearMemory = 0x80,
                              DisplayInterfaceEnableMask = DisplayInterfaceEnabled |
                                                           GetCapabilities | EightBitDac |
                                                           LinearFramebufferEnabled | NoClearMemory;
    // Four independent 64 KiB maps. The same 64 KiB unit is used for Bochs banks.
    // IBM Memory Organization; Bochs BANK interface.
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=38
    // https://github.com/bochs-emu/VGABIOS/blob/master/vgabios/vbe_display_api.txt#L212
    static constexpr unsigned PlaneCount = 4, PlaneBytes = 65536, PlaneAddressMask = PlaneBytes - 1;
    // DAC palette has 256 RGB entries; legacy components and Attribute palette
    // indexes use six bits. DISPI 8BIT_DAC uses all eight component bits.
    // IBM Video DAC to System Interface, printed 2-101..2-102.
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=127
    static constexpr unsigned PaletteEntries = 256, ColorComponents = 3;
    static constexpr uint8_t SixBitColorMask = 0x3f, EightBitColorMask = 0xff;
    // Packed formats are palette-index 8 bpp, RGB555, RGB565 and little-endian
    // B,G,R,(unused) bytes at 24/32 bpp. Host output is opaque AARRGGBB.
    // https://github.com/bochs-emu/VGABIOS/blob/master/vgabios/vbe_display_api.txt#L246
    static constexpr unsigned IndexedBitsPerPixel = 8, Rgb555BitsPerPixel = 15,
                              Rgb565BitsPerPixel = 16, Bgr24BitsPerPixel = 24,
                              Bgrx32BitsPerPixel = 32;
    static constexpr uint32_t OpaqueBlack = 0xff000000, RgbMask = 0x00ffffff;
    // IMPLEMENTATION NOTE: Bounds reject transient register combinations and
    // restrict the host display, not IBM hardware timing or Bochs GUI capabilities.
    static constexpr unsigned MinimumLegacyWidth = 160, MaximumLegacyWidth = 1024,
                              MinimumLegacyHeight = 100, MaximumLegacyHeight = 1024;
    // BIOS mode numbers are a host BIOS helper interface, not hardware registers.
    // Only the named subset has distinct geometry in this simplified preset builder.
    // IBM Graphics Modes / Video Memory Organization.
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=43
    enum BiosMode : unsigned {
        Text80Columns = 3,
        Graphics320x200Color16 = 0x0d,
        Graphics640x350Color16 = 0x10,
        Graphics640x480Monochrome = 0x11,
        Graphics640x480Color16 = 0x12,
        Graphics320x200Color256 = 0x13,
    };
    std::array<std::array<uint8_t, PlaneBytes>, PlaneCount> planes{};
    uint8_t seq[8]{}, gfx[16]{}, crtc[32]{}, attr[32]{}, latch[PlaneCount]{};
    uint8_t seqIndex = 0, gfxIndex = 0, crtcIndex = 0, attrIndex = 0x20;
    bool attrData = false, graphics = false;
    bool dacReadMode = false; // DAC State: true reads (03h), false writes (00h).
    uint8_t dac[PaletteEntries][ColorComponents]{}, dacMask = EightBitColorMask, dacRead = 0,
                                                    dacWrite = 0, dacComponent = 0;
    unsigned width = 640, height = 480;
    uint8_t misc = InitialMiscellaneousOutput;
    // IMPLEMENTATION NOTE: Fixed framebuffer placement and 16 MiB allocation.
    // Bochs physical address convention / B0C5 video-memory size.
    // https://github.com/bochs-emu/VGABIOS/blob/master/vgabios/vbe_display_api.txt#L129
    static constexpr uint32_t lfbBase = 0xe0000000, vramSize = 16 * 1024 * 1024;
    // IMPLEMENTATION NOTE: Emulator display limits reported by GETCAPS.
    static constexpr unsigned maxWidth = 1920, maxHeight = 1200;
    std::vector<uint8_t> vram = std::vector<uint8_t>(vramSize);
    uint16_t dispiIndex = 0;
    // Firmware-style initial API ID, X/Y, bpp, disabled, bank 0, virtual width,
    // computed virtual height placeholder, x/y origin, and memory size in 64 KiB banks.
    std::array<uint16_t, 11> dispi{DisplayInterfaceId,
                                   640,
                                   480,
                                   IndexedBitsPerPixel,
                                   0,
                                   0,
                                   640,
                                   0,
                                   0,
                                   0,
                                   uint16_t(vramSize / PlaneBytes)};
    // Decode the VGA-selected memory window; byte offsets precede plane addressing.
    bool decode_legacy_address(uint32_t address, unsigned &offset) const;
    // Identify CGA two-bank row addressing from the CRTC compatibility control.
    bool cga_addressing() const;
    // Translate a 4-bit color through Attribute palette/page selection.
    uint8_t attribute_palette_index(uint8_t color) const;
    // Resolve logical graphics row to plane offset and horizontal pixel pan.
    unsigned graphics_scanline_address(unsigned y, unsigned &panning) const;
    // Read a selected Bochs register, capabilities or computed virtual height.
    uint16_t read_display_interface_register() const;
    // Validate/store a Bochs register and apply enable, geometry or memory effects.
    void write_display_interface_register(uint16_t value);
    // Initialize the board through the text-mode BIOS helper.
    VGA();
    // Recreate firmware-initialized state and clear display memory.
    // IMPLEMENTATION NOTE: Host reset lifecycle, not an electrical VGA reset model.
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=40
    void reset() {
        *this = VGA{};
    }
    // Sample status on the board's shared 1,193,182 Hz PIT clock; no read-driven time.
    uint8_t read_io_register(uint16_t port, uint64_t pitTicks);
    // Read either DISPI index or selected data register.
    // Bochs VBE Display API, index/data protocol.
    // https://github.com/bochs-emu/VGABIOS/blob/master/vgabios/vbe_display_api.txt#L153
    uint16_t read_display_interface_port(bool data) const {
        return data ? read_display_interface_register() : dispiIndex;
    }
    // Select a DISPI index or write its selected register.
    // Bochs VBE Display API, index/data protocol.
    // https://github.com/bochs-emu/VGABIOS/blob/master/vgabios/vbe_display_api.txt#L153
    void write_display_interface_port(bool data, uint16_t value) {
        if (data)
            write_display_interface_register(value);
        else
            dispiIndex = value;
    }
    // Build simplified BIOS presets; optionally clear legacy planes, disable DISPI.
    void set_bios_mode(unsigned mode, bool clear);
    // Derive visible graphics dimensions from CRTC registers within host bounds.
    void update_display_geometry();
    // Read framebuffer/banked memory or latch all planes and return a VGA read-mode result.
    uint8_t read_memory(uint32_t address);
    // Write packed memory or apply VGA plane mask/write mode using the read latches.
    void write_memory(uint32_t address, uint8_t value);
    // Read raw VGA register state; status reads reset the attribute index/data flip-flop.
    uint8_t read_register(uint16_t port);
    // Write a selected VGA group/DAC component, advancing indexes and geometry as needed.
    void write_register(uint16_t port, uint8_t value);
    // Decode one graphics pixel to opaque AARRGGBB; out-of-range pixels are black.
    uint32_t pixel_color(unsigned x, unsigned y) const;
    // Render a capture or frontend frame from a stable device state.
    void render_graphics(std::span<uint32_t> pixels, unsigned frameWidth,
                         unsigned frameHeight) const;
};
} // namespace pc
