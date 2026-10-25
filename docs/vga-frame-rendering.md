# VGA frame rendering

A headless Excel task profile on the PIC-idle candidate contains 14,109
main-thread samples after excluding the verified profiler attachment barrier.
JIT dispatch accounts for 22.8%, polling 16.6%, and capture/video/control 18.5%.
`VGA::pixel` alone accounts for 11.1% of self samples. These wall-stack shares
include capture and blocked I/O; they are not isolated guest CPU timings.
[Summary and compressed raw trace](measurements/pic-idle-host-profile-2026-10-25.json).

`VGA::render` expands the current DAC palette once per frame. It selects
planar, indexed and VBE 8-bit paths outside the pixel loop. Planar rendering
loads each group's four bytes once for eight pixels. Other VBE pixel formats
retain the existing `pixel` implementation. `PcDisplay` uses the same frame
renderer for GUI presentation and PPM captures; text rendering and cursor
policy remain unchanged. No persistent palette cache or invalidation state is
introduced. Graphics capture temporarily needs four additional bytes per pixel
for the rendered frame (about 1.2 MiB at 640x480); no additional disk is retained.

## Correctness

The standalone test compares 1,014,127 pixels directly to `VGA::pixel` across
seeded random planes, DAC/attribute values, DAC masks, start addresses, stride
wrap, odd widths, cropped/oversized frames, VBE offsets, VRAM bounds, and
8/15/16/24/32-bit and unsupported VBE formats. Empty and undersized spans are
checked separately. Address/undefined-behavior sanitizer validation also passes.

```sh
mkdir -p build/vga-frame
clang++ -std=c++20 -O3 -Wall -Wextra -Wpedantic -Isrc \
  src/vga.cpp tests/vga_frame_test.cpp -o build/vga-frame/tests
build/vga-frame/tests --bench
```

The component tests now compare every rendered and saved PPM pixel against
`VGA::pixel` in planar and indexed modes, in addition to existing text/cursor
and CMOS/RTC checks: 708 checks pass. Headless Win3.11 and both rtts Win95 and
installed OSR2 run Notepad/application/save/restart checks with the new renderer;
successful private compressed disks are removed. CPU execution, interrupt timing
and disk state formats do not change. Fresh Setup was previously validated with
the same x86 core; it is not repeated for a presentation-only change.

## Performance scope

[Raw alternating Excel pairs](measurements/vga-frame-excel-2026-10-25.json)
record the same restored calculation task and expected pixels. The frozen source
trees differ only in `vga.hpp`, `vga.cpp` and `pc_display.cpp`. All three pairs
observe lower process CPU with frame rendering, but host load and CPU-time
variation are substantial. The observed median ratio is not a reliable estimate
of a general application improvement. In particular, this does not establish
faster execution of x86 arithmetic or memory accesses: fewer host cycles are
spent rendering the observations used by the benchmark and by GUI presentation.
The pure RAM rendering benchmark provides the narrower direct performance test.

A local 120-frame, 640x480 run measures planar rendering at 95.51 → 21.55 ms
(4.43×), indexed VGA at 61.20 → 14.31 ms (4.28×), and VBE 8-bit at
93.95 → 11.74 ms (8.00×). The 16/32-bit reference paths show ratios of
1.01/1.06, within the scope of an uncontrolled microbenchmark. These are
render-only wall timings, not a guest benchmark. The three full Excel CPU
ratios are 1.274, 1.456 and 1.841; retain the raw variation instead of treating
the median as a stable expected application gain.

## Monitor shape

The x86 frontend presents classic VGA text and graphics modes in a fixed 4:3
monitor area, independently of framebuffer dimensions. This includes the Windows
95 320x400 startup logo, 320x200 graphics, and 640x350/400 text and graphics.
Mode changes replace the texture without resizing the host window; resizing the
window preserves the monitor aspect with black borders as needed. VBE modes use
square pixels, as does the ARM framebuffer. Raw PPM captures retain guest pixel
dimensions. Presentation policy is passed explicitly rather than inferred from
resolution, so VBE 640x400 can retain its native aspect.

`make build/frontend-tests && ./build/frontend-tests` uses SDL's dummy video
driver to check initial window shape, classic mode transitions, and same-size
VGA/square-pixel policy transitions without a GUI or guest disk.

## BIOS startup text

Cold boots display a PCEmu BIOS banner in guest text video memory, reporting
configured RAM, attached drives and the selected boot drive. The GUI shows it
for 1.5 seconds before executing the disk boot sector, with the window still
responsive to close events. Headless runs have no presentation delay and snapshot
restores resume directly. This is the emulator's own firmware banner, not a
third-party BIOS or a BIOS setup utility. The component tests exercise hard-disk
and floppy boot handoff using a temporary 512-byte image.
