# Standalone VGA BIOS

PCEmu now has an original, optional 32-KiB x86 VGA option ROM in
`firmware/vga/vga.asm`. Its entry point installs INT 10h and initializes VGA
through guest port I/O. It includes color/monochrome text, standard VGA graphics, font services,
strings, palettes, state save/restore and VBE 2.0 for our DISPI board. BIOS calls execute guest instructions; no private BIOS
trap, host video service or SDL call implements these services. The existing
host-backed system BIOS remains in use. This is the text implementation,
not a complete VGA BIOS replacement and not yet the default for installed guests.

## Build and run

Only NASM and Python 3 are required to build the firmware. No Watcom compiler,
external VGA ROM, network access, QEMU installation or DOS image is required.

```sh
cmake -S . -B build -G Ninja
cmake --build build --target vga-bios-tests
./build/vga-bios-tests build/firmware/pcemu-vga.bin
python3 -m unittest discover -s tests -p test_vga_bios_build.py
cmake --build build --target vga-bios-smoke
```

The ROM can also be built directly:

```sh
python3 tools/build_vga_bios.py
```

Select it explicitly for an existing guest with
`--vgabios build/firmware/pcemu-vga.bin --execute-vgabios`. Keep automated runs
headless. Do not use this milestone for a Windows installation: installed-guest compatibility is still being validated.
The existing external ROM stays available for those workloads.

The output and generated font include live under ignored `build/firmware/`.
The builder validates pinned font-source hashes, assembles a fixed-origin ROM,
and fixes the expansion-ROM checksum. NASM 3's absolute-word relocation warning
is disabled specifically because absolute 16-bit offsets into this flat ROM are
intentional; other warnings remain errors.

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
| AH=00h | Modes 00h..03h and 07h; 40/80-column color/monochrome; scan-line/font geometry; bit 7 preserves text memory |
| AH=01h/02h/03h | Cursor shape, page-specific position, query; update hardware only for active page |
| AH=05h | Page selection, BDA byte start, CRTC word start and cursor |
| AH=06h/07h | Overlap-safe window scrolling, zero/oversized counts clear, blank attribute, active-page isolation |
| AH=08h/09h/0Ah | Read at requested page's cursor; repeated character/attribute writes; cursor unchanged |
| AH=0Eh | Active-page teletype, CR/LF/BS/BEL, wrapping and scrolling; bell is silent |
| AH=0Fh | Current mode, no-clear bit, columns and active page |
| AX=1010h/1015h | Individual RGB DAC colors; AH=10h also implements EGA palette blocks, blinking, color paging, PEL masks, DAC blocks and grayscale |
| AX=1130h | Font information for BH=00h..07h; ROM 8x8, 8x14 and 8x16 tables |
| AH=11h | User/ROM font loading, eight font blocks, character-map selection, font resizing and graphics font pointers |
| AH=12h | EGA information, text scan-line selection, palette/grayscale/cursor controls, screen/address enable |
| AH=13h | Counted strings, optional attributes/cursor movement, control characters and page isolation |
| AH=1Bh | Standard 64-byte functionality/state block |
| AX=1A00h | Analog color VGA identification |
| AH=1Ch | Selective hardware/BIOS/DAC save and restore |
| AH=4Fh | VBE 2.0 mode discovery/set/query, save/restore, banking, stride, panning, 6/8-bit DAC, BGRA palettes and relocatable 32-bit interface |

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

## Validation and its limits

`tests/vga_bios_test.cpp` executes the assembled ROM in the interpreter with
host BIOS callbacks removed. It verifies literal expected BDA values, CRTC
addresses, every text page, character repeats, attributes, cursor behavior,
no-clear mode setting, teletype controls, DAC read/write, ROM font pointers and
all 4096 font scan-line bytes loaded into plane 2. A cell-array oracle checks
scrolling in both directions with zero, small and oversized counts. Boundary
cases include clipped/empty rectangles, invalid pages, unsupported functions and
maximal repeats at the last page. Interrupt calls enter with DF and CF set;
stack balance, segment state and documented register outputs are checked. ROM
bytes must remain unchanged. These checks do not depend on the external ROM.

A differential run applies the same supported text/cursor/scroll contracts to
our ROM and the existing external ROM:

```sh
./build/vga-bios-tests build/firmware/pcemu-vga.bin roms/vgabios.bin
```

The reference ROM is optional and remains an ignored local input. Differences
outside the contract, such as font design and CGA cursor-shape emulation, are
not treated as universal correctness rules.

`tests/vga-bios-boot.asm` is a small RAM-only boot probe that checks the installed BIOS
vector, INT 10h calls and B8000 memory. `tools/smoke_vga_bios.py` boots it under
PCEmu's interpreter and JIT with the new ROM, no network and no GUI. Temporary
small media are removed on completion. For independent execution under
QEMU's VGA device and SeaBIOS option-ROM discovery:

```sh
python3 tools/smoke_vga_bios.py --qemu
```

This uses QEMU TCG, a read-only temporary RAM payload, no installed OS and no
large guest image. QEMU is a validation dependency only. The probe exits using
the debug-exit device and reports through port E9h. Passing QEMU establishes a
real option-ROM discovery/boot path; it does not establish full VGA compliance.

Validation on 2026-10-27: the standalone ROM tests and optional reference-ROM
contract passed; Python font/packaging/reproducibility tests passed; the boot
probe passed in PCEmu interpreter, PCEmu JIT and QEMU 11.1.2 TCG. Existing hardware
device and input/video integration tests passed. No Windows installation or GUI
run was performed.

## Existing external diagnostics

These are useful independent diagnostics, not an official comprehensive VGA
BIOS certification suite. They have been inspected, but **have not yet been run
against this text-only milestone**. Their missing prerequisites must not be
reported as passes.

| Diagnostic | Coverage and requirements | How we should use it |
|---|---|---|
| [VGATEST, Marco Bortolin](https://github.com/barotto/VGATEST/tree/905460440c23cf033eaf078b779236fcb70182aa) | GPL-3.0-or-later; text/font maps, graphics modes, palettes, panning/split screen and memory access. Upstream documents `VGATEST0.EXE` (16-bit DOS) and `VGATEST.EXE` (DOS/4GW); source builds with Open Watcom. | Start with the 16-bit binary on an existing DOS guest once font-loading/mode services are implemented; run selected demos with scripted keyboard input and captured frames. Distinguish BIOS failures from unsupported VGA raster features. |
| [Bochs VGABIOS testbios.c](https://github.com/bochs-emu/VGABIOS/blob/12d978bd86fe88ee6b8620b0f1e91c7fb81bc2e6/vgabios/tests/testbios.c) | Existing Turbo C program issuing INT 10h calls and displaying BIOS data; cursor, pages, scrolling and character operations. It uses DOS stdio/conio and human inspection, not machine-readable assertions. | Build with a compatible DOS compiler and convert its observed BIOS-data results into scripted comparisons on a small overlay of an existing DOS image. Our native oracle already covers these operation families independently. |
| [X-VESA, Marco Pistella](https://github.com/Marco-Pistella/X-VESA/tree/96ff78f92f7b6832046f42486641bf9dafbc29b6) | GPL-3.0; mode enumeration, banked/linear framebuffer reliability patterns, display start/virtual geometry, DAC switching and EDID. Includes a release COM binary; source build requires ASMC/TLINK and additional tools. | Use after VBE implementation. Commands 1, 3, 5, 6, 7 and 8 exercise discovery, VRAM patterns, display, page flips, virtual geometry and DAC respectively. Exclude physical timing and throughput claims: upstream explicitly discourages interpreting emulator timing measurements. |

Keep downloaded diagnostic binaries under ignored `build/` and run them with
small temporary compressed overlays of existing DOS media. Do not create a new
large fixture. Initial headless automation should capture frames and memory,
not depend on a person approving an interactive screen.

## Next milestones

1. Validate the expanded text services with the external legacy diagnostics.
   Add state save/restore and complete parameter tables alongside graphics modes.
2. Add standard mono/40-column/CGA/EGA/VGA graphics modes with mode-specific
   register and palette tables. Verify pixels, planar memory effects, transitions,
   font drawing, no-clear behavior and page limits, then VGATEST captures.
3. Add a deliberately bounded VBE implementation from the VESA Core Functions
   specification. Enumerate only implemented modes and capabilities. Test info
   buffer lengths, ABI preservation, invalid requests, banked/linear access,
   stride/panning, palette operations and protected-mode entry points before
   advertising the corresponding VBE version. Run X-VESA functional tests.
4. Validate DOS applications and Windows 3.11/95 startup, display transitions,
   application use, snapshot restore and firmware restart on existing guest
   images before changing the default ROM. Migrate the system BIOS separately.

Text expansion validation: the same mode-3 differential contracts still pass;
new tests exercise 40-column/monochrome addressing, 80x50 font resizing/page
bounds, user font block 5, cursor emulation, counted strings on an inactive
page, palette blocks, color paging, masks, grayscale, palette-loading policy and
functionality buffers. Interrupt wrappers preserve full 32-bit registers.

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

Text presentation uses the loaded plane-2 font maps, selected glyph height,
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

VGATEST CGA test cards exposed an ARM64 inline-memory guard that covered only
A0000..AFFFF. The guard now also rejects B0000..BFFFF, preserving video-device
callbacks for native loads, stores and read-modify-write operations. Regression
checks cover both aperture halves and boundary-crossing accesses. The same
unmodified VGATEST cards now agree with QEMU in interpreter and JIT runs.

X-VESA directly selects 80x50 text without updating BIOS data. Native text
scanout now derives character height and the visible grid from CRTC registers;
a regression check deliberately leaves stale BIOS geometry in place.
