# Keyboard, mouse and display integration

PCEmu implements the keyboard controller, PS/2 mouse, VGA/DISPI hardware and PCI
configuration itself. The existing SeaVGABIOS remains the allowed BIOS asset.
The Windows display driver is the external, MIT-licensed VMDisp9x guest driver;
no emulator implementation is linked or copied from the products named in its
compatibility list.

The configured Windows 95/Office desktop is available through `./run-office97.sh`.
It restores `build/snapshots/office97-integrated` into a private writable delta.
The original VGA benchmark desktop remains available with `--vga`.

## Host controls

SDL physical key events feed the guest in both real and protected mode. Right
Ctrl/Alt, navigation keys, keypad keys, Windows/Menu keys, F11/F12, Print Screen
and Pause have their own scan sequences. F12 and Ctrl+Q reach Windows/Office.
Losing focus releases held host keys and mouse buttons.

Click inside the window to capture relative mouse movement. **Cmd+G** releases
or captures the mouse; **Cmd+Q** quits the emulator. The capture click is consumed. The window title shows capture state.
The guest keyboard layout controls characters; host IME/text composition is
disabled for this physical-keyboard path.
The first implementation uses relative PS/2 input, not an absolute pointer or
shared clipboard. Windows controls its own pointer acceleration. A wheel is
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
