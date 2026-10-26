# Keyboard, mouse and display integration

PCEmu implements the keyboard controller, PS/2 mouse, VGA/DISPI hardware and PCI
configuration itself. The existing SeaVGABIOS remains the allowed BIOS asset.
The Windows display driver is the external, MIT-licensed VMDisp9x guest driver;
no emulator implementation is linked or copied from the products named in its
compatibility list.

`./run95.sh` prefers `media/win95-mouse.pcd` when present, with seamless mouse integration.
It falls back to `media/win95-monitor.pcd`, configured with
VMDisp9x at 1440×900 in 32-bit colour with a Super VGA 1600x1200 monitor profile. It cold boots normally, including the BIOS
screen. `./run95.sh --vga` selects the original working disk. If that disk is absent, the launcher falls back to `media/win95-svga.pcd`,
then the original VGA disk. Existing disks remain available.

To configure a new SVGA disk, shut down the original guest first, then stage the
pinned driver:

```sh
cp media/win95-working.pcd media/win95-svga-base.pcd
chmod a-w media/win95-svga-base.pcd
python3 tools/stage_display_driver.py media/win95-svga-base.pcd media/win95-monitor.pcd
```

This preserves the existing compressed working overlay as an immutable parent;
the VGA fallback can continue writing its own disk independently. Keep the base
and all its parents. For a pristine QCOW2 installation, stage directly from the
read-only `media/win95.raw` base instead. Boot it with `./run95.sh`, install the compatible adapter using the steps below,
restart, configure the monitor as described below, and select True Color (32 bit)
and 1440×900 in Display Settings. These
settings persist in the guest disk; merely changing the host framebuffer cannot
make the stock Windows VGA driver render more colours.

The configured Windows 95/Office desktop is available through `./run-office97.sh`.
It restores `build/snapshots/office97-integrated` into a private writable delta.
The original VGA benchmark desktop remains available with `--vga`.

## Host controls

SDL physical key events feed the guest in both real and protected mode. Right
Ctrl/Alt, navigation keys, keypad keys, Windows/Menu keys, F11/F12, Print Screen
and Pause have their own scan sequences. F12 and Ctrl+Q reach Windows/Office.
Losing focus releases held host keys and mouse buttons.

The configured QEMouse guest follows the host pointer without capture.
For guests using the stock relative mouse driver, click inside the window to capture relative mouse movement. **Cmd+G** releases
or captures the mouse. For the `win95` profile, window close and **Cmd+Q** request
a normal Windows shutdown. The emulator stays open for save prompts or cancellation
and exits only after recognizing WIN.COM’s completed shutdown loop and flushing
the disks. Other profiles retain immediate close behavior. The capture click is consumed. The window title shows capture state.
The guest keyboard layout controls characters; host IME/text composition is
disabled for this physical-keyboard path.
Stock guests use relative PS/2 input; configured Windows 95 guests use the
absolute protocol below. A shared clipboard is not implemented. Windows controls its own pointer acceleration. A wheel is
available to guest drivers that negotiate IntelliMouse ID 3; the built-in
Windows 95 PS/2 driver normally uses the standard three-byte protocol.

The controller distinguishes keyboard/AUX output, implements AUX commands and
IRQ12, and keeps command replies separate from motion reports. Host scan/mouse
streams wait while their enabled interrupt is masked or being serviced, so
hardware detection cannot wedge the shared output buffer. Polled command replies
remain available. The original INT 15/C2 mouse BIOS and IRQ12 far callback obey
IBM's stack interface. Guest PIC mask writes and EOIs execute as x86 instructions
so a Windows virtual-8086 monitor can observe them.

## Display hardware and driver

The display has 16 MB of video RAM, DISPI ports 01CE/01CF, a banked A0000 aperture
and a linear framebuffer at E0000000. Pixel formats are 8-bit indexed, RGB555,
RGB565, packed 24-bit BGR and 32-bit BGR. Virtual pitch and display offsets are
supported. The SDL texture follows guest geometry and the resizable window keeps
its aspect ratio. PCI identifies the compatible display function at 00:02.0;
real-mode PCI BIOS and original protected-mode BIOS32 firmware expose its
resources. BAR relocation, 3D acceleration and a general PCI chipset are outside
the current implementation.

The guest driver is [VMDisp9x 1.2025.0.119b](https://github.com/JHRobotics/vmdisp9x/releases/tag/v1.2025.0.119),
using its small 2D package. The archive is retained in `media/drivers`; its license
and readme are also staged in the guest. SHA-256:
`e0d698a6089347a6a619ed439c092f247392fbafac6b342f4651c1380911fb23`.
Its **QEMU STD VGA PCI Adapter** selection uses the compatible direct DISPI
interface. That label names the guest driver's target interface.

To stage it into a fresh private disk without a raw image copy:

```sh
python3 tools/stage_display_driver.py \
  build/snapshots/office97-desktop/disk.raw media/my-display-test.pcd
./build/pcemu --disk media/my-display-test.pcd --ram-mb 64 --execute-vgabios
```

Use a cleanly shut down parent or an immutable checkpoint disk, and never stage
into a disk attached to a live guest. The tool verifies all 17 staged files by
reading them back through the original compressed disk implementation. The disk
delta is approximately 443 KB.

In an older Windows installation, first use Add New Hardware's automatic scan to
install **PCI bus** and **Standard PS/2 Port Mouse**, then restart. In Display
Properties → Settings → Advanced Properties → Adapter → Change, choose Have
Disk, `C:\DISPLAY`, Show all devices, and **QEMU STD VGA PCI Adapter**. If copying
asks for a source directory, enter `C:\DISPLAY` again. Restart, then choose the
resolution and colour depth in Settings. The generic VESA driver was investigated
but the direct DISPI driver is the integration target.

## Verified guest behavior

Windows detected PCI bus and Standard PS/2 Port Mouse, loaded the direct display
driver and retained 1024×768/32-bit settings after a full firmware restart.
PS/2 left click opened Start; right click opened Excel's cell menu. Word reopened
the existing saved document and F12 opened Save As. Excel reopened its saved
workbook and displayed the existing formula result 4. The integrated checkpoint
restored successfully in the SDL frontend after disk consolidation.

Screenshots and state metadata are in `build/win95-integration`. The consolidated
2.45 MB disk delta was compared with its source over all 2 GB of logical sectors;
no bytes differed. The approximately 18 MB zstd state contains RAM and VRAM.

## Firmware, checkpoints and tests

`firmware/pci32.asm` and `firmware/int15.asm` are original guest firmware.
`make firmware` regenerates their checked-in C++ byte arrays using NASM. Normal
builds use the checked-in headers.

Snapshot version 2 adds VRAM, DISPI and pending mouse/controller state; version 3
adds PCI configuration state. Versions 1 and 2 remain readable. Host SDL objects
and native JIT code are recreated. Old Windows checkpoints need a guest restart
and driver setup to discover the new devices.

`make test` includes protocol, signed movement, wheel negotiation, BIOS callback,
V86 PIC I/O, PCI/BIOS32, banked/LFB alias, colour rendering, panning and compressed
snapshot checks. The allowed VGA ROM is executed in a test that queries VBE mode
info and sets 1024×768 true colour. These are separate from the real Windows
installation checks.

Specifications: [VBE display API](https://raw.githubusercontent.com/bochs-emu/VGABIOS/master/vgabios/vbe_display_api.txt),
[IBM PS/2 BIOS Technical Reference](https://www.ardent-tool.com/docs/pdf/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf),
[Linux PCI register definitions](https://github.com/torvalds/linux/blob/master/include/uapi/linux/pci_regs.h).
Local reference copies are under `docs/specs`.


### Automated GUI measurements

`--no-host-input` ignores physical host keyboard/mouse events while preserving
control-file `key`, `scan`, `text` and `mouse` commands. Window close and Cmd+Q
remain available. The benchmark harness enables this for compatible engines and
records the policy in its results. It prevents everyday Mac input from changing
the guest during a pixel readiness check. Interactive launches keep their usual
input behavior. A failed automated guest can be made interactive by appending
`host-input on` to its control file; `host-input off` disables it again.

Idle input polling now waits for queued controller bytes or the existing mouse
sample deadline. A due movement packet is still formed while controller output
is paced; queued button transitions are delivered without waiting for the next
movement sample. Full output buffers still update IRQ lines immediately when
IRQ enable bits change. The guard reads live state rather than caching it, so
host injection, command replies and restored pending packets need no explicit
wake-up/invalidation hook. Interrupt delivery itself is not batched by this change.

### Windows 95 close requests

The legacy platform has no ACPI power button. Close requests use paced PS/2
keyboard input to open the English Windows 95 Run dialog and invoke
`rundll32.exe user.exe,ExitWindows` without a force option. Repeated close clicks
are idempotent. A request made during boot waits for protected mode. If Windows
cancels shutdown or the shell cannot accept the shortcut, the emulator remains
open; the user can complete shutdown through Start → Shut Down. There is no
timeout that cuts power. Completion matches the original real-mode WIN.COM
shutdown broadcast and terminal instruction loop, rather than a fixed address,
CPU idle time or a quiet disk. Unrecognized guest versions remain open.

The control-file command `close` exercises the same path headless. The existing
`quit` command remains an explicit immediate stop for disposable validation
guests. Frontend tests cover pacing, repeated requests and rejection of unrelated
guest code as shutdown completion. A private compressed OSR2 overlay was booted
to Explorer and shut down through `close`, reaching the safe-to-turn-off screen
and the logged disk-flush completion.

### Widescreen modes and the guest monitor

The pinned VMDisp9x package omits 1440×900 from its default INF. The staging
tool now adds that mode to `VM.AddReg` at 8-, 16-, 24- and 32-bit colour without
changing driver binaries or the pinned archive. All staged files, including the
derived INF, are read back and verified. The adapter already supports up to
1920×1200 with 16 MiB of VRAM.

Windows 95 also needs a named monitor profile. In Display Properties → Settings
→ Advanced Properties → Monitor → Change, select **Standard monitor types →
Super VGA 1600x1200**. This supplies the virtual monitor's resolution limit and
removes the unspecified-monitor warning. It is a manual monitor profile; DDC
and EDID emulation are not required for this setting.

For an existing installation, stage an upgraded driver directory into a new
compressed child of a stopped, immutable disk:

```sh
python3 tools/stage_display_driver.py PARENT.pcd DESTINATION.pcd --directory WIDE
```

Boot the child, reinstall **QEMU STD VGA PCI Adapter** using Have Disk and
`C:\WIDE`, then restart before changing the monitor. Select the monitor profile
and 1440×900 in Display Settings. Keep the original disk unchanged until a cold
boot verifies the new configuration. `python3 tests/test_display_modes.py` checks
INF section isolation, all four colour depths, idempotence and layout validation.

The configured monitor disk is a child of an immutable compressed copy of the
previous SVGA working disk (`media/win95-before-monitor.pcd`). Keep that parent
and its backing chain. An already-running original SVGA guest is left intact;
close it cleanly and relaunch to use the configured monitor disk. Later changes
made in the original disk are separate from this upgrade snapshot; it remains
available with `./run95.sh --disk media/win95-svga.pcd`.

## Seamless Windows 95 mouse

QEMouse v1.03 is installed in the persistent `media/win95-mouse.pcd` overlay.
Its immutable parent, `media/win95-before-mouse.pcd`, preserves the previous
monitor-configured disk. Keep the complete parent chain. `run95.sh` selects the
native-cursor disk first (when installed), then the mouse, monitor, SVGA and original VGA disks.
`--vga` retains the original relative-input installation.

The guest driver negotiates the VMware-compatible absolute-pointer protocol on
I/O port 5658h. PCEmu implements version detection, enable/disable, relative and
absolute mode selection, status and 1–4-word data reads. Coordinates span
0–65535 across the guest image. PS/2 IRQ12 notifications wake the driver, which
reports absolute motion to Windows. Only a negotiated driver activates seamless
frontend input. Continuous motion is coalesced; button transitions remain ordered.
Invalid reads and queue overflow report a protocol error and relinquish absolute
mode. Reset clears negotiation. Snapshot version 5 preserves negotiation and
pending data; versions 1–4 remain readable.

SDL maps input through the scaled guest viewport, delivers the first click and
never enables relative mouse mode while integrated. The host pointer can leave
the image freely. The original mouse overlay uses Windows software cursor drawing.
The native-cursor overlay described below uploads Windows shapes to the host.
Leaving the image or losing focus releases held buttons; focus loss
also releases keys. The fallback relative driver retains capture and Cmd+G.

QEMouse is an external GPL-2.0 guest asset, not emulator code. Source and license:
[QEMouse v1.03](https://github.com/qemus/qemouse/tree/v1.03). The pinned release
archive SHA-256 is
`1970cb19bf99558acdc88c110cfc9a5bfe54a58c927118b1970048c229a6ad71`.
The current guest driver exposes left/right buttons; middle-button and wheel
reporting need additional guest-driver support.

To prepare another installation from a stopped, immutable parent:

```sh
python3 tools/stage_mouse_driver.py PARENT.pcd DESTINATION.pcd
```

The staging tool verifies the archive and reads all staged files back from
`C:\QEMOUSE`. In Windows, right-click `qemouse.inf`, choose Install, then reboot.
This installs `qemouse.drv` and updates `[boot] mouse.drv` in SYSTEM.INI. The
standard mouse driver remains available in the unchanged parent disk.

Headless validation covers protocol/register results, partial reads, notification
packets, coalescing, overflow, reset, snapshots, scaled SDL coordinates, cursor
visibility, first clicks, focus loss and relative fallback. The configured guest
was cold-booted at 1440×900×32; absolute movement reached the expected framebuffer
pixels, left-click opened Start, right-click opened the desktop menu, and normal
shutdown completed with a disk flush. To repeat the live-guest mouse checks after
bootstrapping an unobscured headless desktop:

```sh
python3 tools/check_win95_mouse.py CONTROL FRAME.ppm STATE.json --output build/mouse-check
```

## Native host cursor

`run95.sh` now prefers `media/win95-native.pcd`. Its immutable parent is
`media/win95-before-host-cursor.pcd`, a checkpoint of the seamless mouse disk.
Windows supplies cursor shapes and visibility, while SDL creates a native OS
cursor. Movement follows the host compositor without waiting for guest IRQ12,
Windows drawing, framebuffer uploads or the emulator's frame interval. Arrow,
text and resize shapes keep their Windows hotspots. Outside the guest image,
the normal host default cursor is restored. Keyboard/click input still uses the
negotiated absolute mouse path.

The display DDI adapter is source in `guest/host_cursor.asm`. The staging tool
adds a relocation-free 16-bit code segment to the pinned VMDisp9x NE driver and
redirects only display ordinals 102–104 (`SetCursor`, `MoveCursor`, `CheckCursor`).
The original code, data and relocations remain intact. It suppresses the DIB
Engine software cursor and sends `CURSORSHAPE` bytes through PCEmu I/O ports:

* 5659h, word C501h: begin a shape upload.
* 565Ah, byte: 12-byte CURSORSHAPE header, AND mask, then XOR pixels.
* 5659h, word C502h: atomically publish a complete validated shape.
* 5659h, word C500h: hide the cursor and cancel an unfinished upload.

Shapes up to 64×64 support monochrome AND/XOR masks and 8/16/24/32-bit colour;
indexed colour uses the VGA palette sampled when publishing. Unsupported guest
shapes are hidden. Screen inversion in monochrome cursors follows SDL/host
support. If a host backend cannot create a cursor, SDL's default arrow is used.
Snapshot version 6 preserves the published cursor, palette and unfinished upload;
versions 1–5 remain readable. State JSON includes shape hash, hotspot, visibility
and upload revision for headless verification.

Prepare another native-cursor installation only from a stopped, immutable,
QEMouse-enabled parent with the pinned QEMU display driver installed:

```sh
./tools/build.sh pcemu disk-tool
# Requires nasm; the archive is the same verified VMDisp9x package used above.
python3 tools/stage_host_cursor.py PARENT.pcd media/win95-native.pcd
```

This adapter is specific to PCEmu. The tool checks both the archive checksum and
installed driver, replaces it only in a new compressed child, and verifies the
installed bytes. It refuses fragmented directory layouts. No guest binaries or
VM disks are checked into Git. The driver ABI is documented by the upstream
[VMDisp9x cursor implementation](https://github.com/JHRobotics/vmdisp9x/blob/main/dibcall.c)
and [CURSORSHAPE definition](https://github.com/JHRobotics/vmdisp9x/blob/main/cursor.h).

Validation includes actual 16-bit DDI execution in a 512-byte headless guest,
malformed/partial/oversized upload rejection, native SDL visibility and focus
handling, and snapshot round trips. The live Windows 95 checker verifies that
absolute movement leaves no cursor pixels in the framebuffer, a native shape
was uploaded, and left/right clicks still open the expected menus. A separate
headless Notepad check verified arrow → text I-beam → arrow uploads, including
the I-beam hotspot (15,16), followed by a flushed clean shutdown:

```sh
python3 -m unittest discover -s tests -p test_host_cursor.py
python3 tools/check_win95_mouse.py CONTROL FRAME.ppm STATE.json \
  --native-cursor --output build/native-cursor-check
```

Headless tests cannot measure perceived macOS cursor latency or visually inspect
the real compositor. Native cursor movement itself is owned by the host OS.
