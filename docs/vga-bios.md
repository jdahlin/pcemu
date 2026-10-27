# Standalone VGA BIOS

PCEmu builds and embeds an original 32-KiB x86 VGA/VBE 2.0 option ROM.
Normal x86 sessions execute it by default, without an external VGA firmware file.
Its INT 10h services execute guest instructions using VGA ports and display
memory. The host-backed system BIOS remains responsible for other services;
ARM UEFI is a separate firmware dependency.

`firmware/vga/vga.asm` contains initialization, interrupt dispatch and core text
services. Separate assembly modules implement fonts, palettes, graphics, CGA,
queries, state and VBE. Together these contain approximately 4,500 source lines,
plus generated font/mode tables, the builder and tests.

## Build and run

`make test` includes firmware packaging tests, ROM oracles and interpreter/JIT
RAM-only boot probes. QEMU and installed-DOS checks remain explicit optional
validation commands.

NASM and Python 3 are build dependencies. All font sources are vendored and
hash-checked: firmware builds require no network, Watcom, DOS image or QEMU.

```sh
cmake -S . -B build -G Ninja
cmake --build build --target pcemu vga-bios-tests
ctest --test-dir build -R '^vga-bios' --output-on-failure
python3 tools/smoke_vga_bios.py --qemu
```

`build/firmware/pcemu-vga.bin` is also usable as an ordinary option ROM.
`--vgabios FILE` selects an explicit comparison ROM. `--no-execute-vgabios`
selects the host video BIOS path for diagnostics; `--execute-vgabios` explicitly
selects guest execution. Snapshots retain their own ROM and reset policy.
The native macOS frontend also uses the embedded ROM for new sessions.
Generated binaries and includes remain under ignored `build/`.

The builder checks the option-ROM and PCI headers and fixes the checksum.
NASM 3's absolute-word relocation warning is disabled for intentional 16-bit
flat-ROM offsets; other warnings remain errors. The executable embeds the same
checksummed bytes as the standalone file.

## Specification contract

The baseline is the IBM PS/2 VGA interface, excluding Model 30's special cursor
scaling. References distinguish BIOS contracts from register programming:

- **IBM Personal System/2 and Personal Computer BIOS Interface Technical
  Reference, first edition, April 1987, part 84X1514**:
  [INT 10h summary, printed 2-10 / PDF 25](https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=25),
  [cursor services, printed 2-14..15 / PDF 29..30](https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=29),
  [pages/scrolling, printed 2-16 / PDF 31](https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=31),
  [font information, printed 2-32 / PDF 47](https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=47),
  [video data area 1, printed 3-7 / PDF 144](https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=144),
  [video data area 2, printed 3-9 / PDF 146](https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=146),
  [adapter ROM header/entry/checksum, printed 4-12 / PDF 173](https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=173).
- **IBM Hardware Interface Technical Reference, Video Subsystems, 42G2193,
  September 1992, section 2**:
  [register definitions, printed 2-39 onward / PDF 65 onward](https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=65).
  Source comments link the relevant register groups beside their operations.
- **PCI option-ROM packaging**:
  [UEFI 2.10, PCI expansion ROMs, Table 14.14](https://uefi.org/specs/UEFI/2.10/14_Protocols_PCI_Bus_Support.html#pci-expansion-rom),
  and the primary implementation reference
  [TianoCore PCI 2.2 wire structures](https://github.com/tianocore/edk2/blob/master/MdePkg/Include/IndustryStandard/Pci22.h).
  The ROM includes a PCIR structure for our `1234:1111` display device, x86 code
  type, image length and last-image flag. No EDK II code is linked into it.
- **Bochs DISPI**, for disabling an extended framebuffer before text mode:
  [VBE Display API](https://github.com/bochs-emu/VGABIOS/blob/master/vgabios/vbe_display_api.txt).
  This device register interface is separate from the guest-facing VBE BIOS API.

| Interface | Implemented behavior |
|---|---|
| ROM entry | Install INT 10h, INT 1Fh and INT 43h font pointers; set mode 3; preserve caller registers/FLAGS; far return |
| AH=00h | Modes 00h..07h and 0Dh..13h; color/monochrome text, CGA, planar VGA and chain-4; bit 7 preserves display memory |
| AH=01h/02h/03h | Cursor shape, page-specific position, query; update hardware only for active page |
| AH=05h | Page selection, BDA byte start, CRTC word start and cursor |
| AH=06h/07h | Overlap-safe window scrolling, zero/oversized counts clear, blank attribute, active-page isolation |
| AH=08h/09h/0Ah | Read at requested page's cursor; repeated character/attribute writes; cursor unchanged |
| AH=0Bh/0Ch/0Dh | CGA background/palette selection; graphics pixel write/read, XOR and pages |
| AH=0Eh | Active-page teletype, CR/LF/BS/BEL, wrapping and scrolling; bell is silent |
| AH=0Fh | Current mode, no-clear bit, columns and active page |
| AX=1010h/1015h | Individual RGB DAC colors; AH=10h also implements EGA palette blocks, blinking, color paging, PEL masks, DAC blocks and grayscale |
| AX=1130h | Font information for BH=00h..07h; ROM 8x8, 8x14 and 8x16 tables |
| AH=11h | User/ROM font loading, eight font blocks, character-map selection, font resizing and graphics font pointers |
| AH=12h | EGA information, text scan-line selection, palette/grayscale/cursor controls, screen/address enable |
| AH=13h | Counted strings, optional attributes/cursor movement, control characters and page isolation |
| AH=1Bh | Standard 64-byte functionality/state block |
| AH=1Ah | Display combination read/write, primary/alternate ordering |
| AH=1Ch | Selective hardware/BIOS/DAC save and restore |
| AH=4Fh | VBE 2.0 mode discovery/set/query, save/restore, banking, stride, panning, 6/8-bit DAC, BGRA palettes and relocatable 32-bit interface |

The VBE baseline is [VESA VBE Core Functions 3.0](https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf),
using its retained 2.0 contracts (functions 00h..0Ah). VBE 3.0 extensions are
not advertised.

Unknown legacy operations are no-ops; unsupported mode sets leave the existing
mode intact. Defensive policy ignores invalid page numbers, cursor coordinates
outside the text grid and empty scroll rectangles, clips scroll bottom/right,
and bounds repeated writes to the selected page allocation. These policies are
not claims about undocumented IBM behavior. Teletype scroll blanking uses the
last row's first attribute, matching the existing Bochs ROM; IBM does not define
that attribute in its 0Eh description.

The font data comes from the upstream public-domain `dhepper/font8x8` project,
pinned at `8e279d2d864e79128e96188a6b9526cfa3fbfef9`. Unmodified source headers,
attribution and SHA-256 hashes are in `firmware/vga/vendor/font8x8/`. The builder
maps Unicode glyphs into CP437 and reverses the source's bit order. CP437 control pictures and twelve supplementary mathematical glyphs are original. The 8x14/8x16 fonts resample the 8x8 rows: it is usable but does not claim IBM ROM glyph identity. Control pictures, the 7Fh house, printable Latin, Greek, box-drawing and
mathematical characters have glyphs. Font data is not taken
from the external VGA ROM.

## Standard VGA graphics expansion

Modes 0Dh..13h now program the VGA hardware for 320x200, 640x200,
640x350 and 640x480 planar/chain-4 operation. Pixel read/write supports planar
XOR, per-plane colors, page addressing and no-clear mode sets. Graphics font
output/readback, teletype and character-aligned scrolling execute in the ROM;
planar scrolling uses the hardware latches/write mode 1 and preserves register
programming. The graphics tests compare literal plane offsets and use independent
byte/pixel oracles for overlapping scrolling in both directions. The boot-sector
probe also checks modes 12h and 13h under PCEmu interpreter/JIT and QEMU TCG.
CGA-compatible modes 04h/05h/06h are implemented with B8000 packed/interleaved addressing.

## VBE 2.0 and state management

The ROM publishes 28 implemented 8/15/16/24/32-bit modes through 1600x1200,
including Bochs OEM 32-bit modes. Mode information reports the actual PCI BAR,
64-KiB bank granularity, packed stride, RGB masks and available image pages.
The 4F00h interface respects the caller's 256/512-byte buffer contract. Reserved
flags, unsupported modes/windows, out-of-range banks, impossible strides,
panning rectangles and palette ranges are rejected before writes. Mode sets
reset stride, bank, pan and DAC precision; no-clear preserves display bytes.
VBE 3.0 scheduling/refresh/pixel-clock/stereo interfaces are not advertised.

4F0Ah returns a relocatable 32-bit interface for bank, display start and palette
writes. Protected-mode display start uses DX:CX in four-byte units, while the
real-mode entry uses pixel coordinates. Tests copy this code out of ROM and
execute it in a flat protected-mode segment with host BIOS callbacks disabled.
The RAM boot probe also copies and executes it under PCEmu interpreter/JIT and
QEMU, switches back to real mode, and checks its device effects.

AH=1Ch and VBE 4F04h use a private 1024-byte state image (16 blocks of 64 bytes).
Selectors independently cover VGA hardware, BIOS data/font pointers, DAC state
and DISPI state (VBE only). Framebuffer bytes are not part of the image and
restoration never erases them. Signature/version/selector validation occurs
before restoration. Tests verify an all-group state roundtrip across a legacy
mode change, buffer guards, display-byte preservation and corrupt-image rejection.
The QEMU boot probe independently verifies save/restore and mode reconstruction.

## CGA compatibility and native text scanout

Modes 04h/05h/06h use the documented B8000 aperture, even/odd scan-line banks,
odd/even plane selection for two-bit pixels, packed pixel/XOR services, glyphs,
scrolling, foreground/background/intensity and palette selection. The CPU and
JIT route the full A0000..BFFFF video window through the device callbacks;
graphics reads/writes no longer bypass CGA memory as ordinary RAM. Legacy
VGA memory-map selection is decoded before plane addressing. Headless boot
probes independently check direct B8000 writes and BIOS pixels in QEMU.

Text presentation uses the loaded plane-2 font maps, CRTC glyph height and visible grid,
40/80-column geometry, monochrome B0000 memory, page allocation, DAC colors,
background intensity, blinking and screen blanking. The host-backed system
BIOS retains its existing RAM-shadow/ROM-font fallback. CRTC split-screen,
horizontal pel panning, complete text-address generation, timing and protected
CRTC registers remain device-model limitations; the ROM itself does not supply
host rendering shortcuts for those features.

Attribute Address reads now include Palette Address Source, and the DAC State
register reports the documented read/write direction. Snapshot version 7 stores
the DAC direction latch and Attribute source bit; versions 1..6 remain readable
with their previous always-enabled display behavior. Tests include an encoded
version-6 compatibility image, a version-7 roundtrip and active DAC direction.
VGA write mode 3 now applies the requested logical operation to its latches.

The primary and secondary video save-pointer tables, DCC table and separate
legacy INT 1Dh parameter sets are present. DCC reads/writes preserve primary and
alternate display ordering. Functionality reports use mode-specific color/page
counts and scan-line codes rather than assuming text mode.

Native text follows direct CRTC geometry changes even when BIOS data is stale,
as exercised by X-VESA. The native memory guard covers the full A0000..BFFFF
aperture, including CGA loads, stores and read-modify-write operations.

## Validation and its limits

`tests/vga_bios_test.cpp` executes the assembled ROM with host BIOS callbacks
removed. Independent cell, byte and pixel oracles check text pages, clipping,
scroll overlap, font planes/maps, CGA banks, planar/chain-4 addressing, palettes,
VBE mode information and bounds, stride/panning, invalid-request atomicity and
selective state restoration. ABI checks cover full 32-bit register preservation,
segments, stack balance, DF/CF, guarded buffers and immutable ROM bytes.
Protected-mode tests relocate the VBE entry points into RAM and execute them
in flat 32-bit mode. Display tests include direct CRTC text geometry changes.
The roughly 2.3 million assertions include per-byte/per-pixel checks; that number
is not a count of independently covered features.

The optional reference-ROM run compares only shared supported contracts:

```sh
./build/vga-bios-tests build/firmware/pcemu-vga.bin roms/vgabios.bin
```

`tests/vga-bios-boot.asm` is a small two-stage RAM-only probe. It verifies option
ROM initialization, text, standard/CGA graphics, direct B8000 access, VBE mode
information/stride/panning/DAC/state, and copied protected-mode interfaces.
`tools/smoke_vga_bios.py --qemu` runs it headlessly in PCEmu interpreter and JIT,
using both embedded and explicit ROM selection, then under QEMU TCG/SeaBIOS.
It requires no installed OS and removes its tiny temporary boot image.

Third-party diagnostics are supplementary evidence, not an official VGA
certification suite. The fetch tool pins release/source revisions and SHA-256
hashes, retains licenses and stores binaries only under ignored `build/`.

```sh
python3 tools/smoke_vga_diagnostics.py --disk /absolute/path/existing-dos.qcow2 --qemu
# Add --no-jit for the interpreter comparison.
```

The tool stages unmodified diagnostics on a small compressed overlay, creates
independent PCEmu/QEMU children, retains captures and JSON evidence, and removes
owned disks after success. The existing DOS parent is unchanged. Diagnostic
execution requires Pillow, Tesseract and, for comparison, QEMU.

| External diagnostic | Verified scope |
|---|---|
| [VGATEST 1.5, Marco Bortolin](https://github.com/barotto/VGATEST/tree/905460440c23cf033eaf078b779236fcb70182aa) | Unmodified 16-bit DOS binary: font maps 0/2, 80x50 text, static test cards in modes 04h, 06h, 0Dh, 12h, 13h and Mode X 320x240. Interpreter and JIT captures compared with QEMU. |
| [X-VESA 2.0, Marco Pistella](https://github.com/Marco-Pistella/X-VESA/tree/96ff78f92f7b6832046f42486641bf9dafbc29b6) | Unmodified COM binary: all 28 VBE modes enumerated, detailed mode information decoded, and 6/8-bit DAC gradients toggled. Enumeration and both gradients compared with QEMU; native precision latch also checked. |
| [Bochs testbios.c](https://github.com/bochs-emu/VGABIOS/blob/12d978bd86fe88ee6b8620b0f1e91c7fb81bc2e6/vgabios/tests/testbios.c) | Inspected, not compiled or run. Requires a compatible DOS C toolchain; its interactive reports are not automated assertions. |

Pixel comparisons reduce captured RGB channels to their six-bit DAC values,
remove the ninth text column and exclude only the text cursor footprint.
They retry for a visible text blink phase and completed drawing. For the dual
font-map case alone, QEMU's clone-style foreground intensity is mapped to IBM's
low palette: IBM reassigns that intensity bit to font selection. Independent
native tests verify PCEmu's palette behavior. These adjustments are recorded in
the report; font shapes and remaining pixels must match exactly.
X-VESA's DAC test initially asks to continue because DISPI mode selection leaves
legacy VGA registers unchanged. Its register-based heuristic makes the same
request on both boards. The harness records and accepts that prompt, then
requires both actual gradient screens and an enabled eight-bit DAC latch;
it does not count the warning screen as a successful DAC test.

Recorded results, binary hashes and comparison adjustments are in
[the validation record](measurements/standalone-vga-2026-10-27.json).

Headless installed-guest checks on existing Windows images passed with the ROM:
Windows 3.11 Notepad save/DOS readback, and Windows 95 cold boot, Calculator
arithmetic, clipboard transfer, exact Notepad file persistence and guest-requested
firmware restart. Successful runs remove their private compressed overlays.
`tools/smoke_win311.py` and `tools/smoke_win95.py` accept `--vgabios FILE` for
explicit comparison; omission exercises the embedded default.

The focused suite also covers snapshots (including version-6 compatibility),
hardware devices, input/video integration and native memory access boundaries.
Remaining device limits include split screen, pel panning, complete text-address
generation, protected CRTC registers and accurate raster timing. Not every
interactive VGATEST/X-VESA demo has been validated. Optional LCD, dual-adapter,
DDC/EDID and VBE 3.0 functions are not advertised. Published BIOS save/parameter
tables do not imply support for arbitrary guest replacement mode tables.
Fonts are usable CP437 designs, not pixel-identical IBM ROM fonts. This is a
useful standalone implementation for the verified guests, not full physical VGA
conformance or an independently rewritten system BIOS.

## Edge-case validation expansion

Native text now scans actual VGA character/attribute planes and CRTC display
start/stride, Preset Row Scan, byte/pel panning, and the ten-bit Line Compare.
The lower screen restarts at address zero on the following scan line; the
Attribute Pixel Panning Mode controls lower-screen horizontal reset. Nine-dot
line-graphics replication, CRTC cursor location/shape and underline selection
are rendered. Physical text captures expose the programmed 8/9-dot geometry.
The host BIOS preset retains its RAM-shadow compatibility path.

An independent full-frame oracle covers unaligned splits, overflow bits 8/9,
zero/custom strides, wrapping starts and independent upper/lower panning. QEMU
text captures are not a complete oracle here: its renderer rounds splits to
character rows and omits text pel/preset-row panning. IBM 42G2193 is the contract.

Planar and 256-color graphics now apply the ten-bit Line Compare, display
start/stride, byte and horizontal pel panning, lower-screen pan reset, and
Attribute Color Plane Enable. Zero stride repeats a display row instead of
substituting a default pitch. Independent plane-byte oracles cover wrapped
starts, overflow boundaries, zero/custom stride and both pixel formats.
