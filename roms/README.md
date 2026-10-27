Normal x86 sessions use PCEmu's original embedded VGA/VBE option ROM. Building
it requires NASM and Python; see [VGA BIOS](../docs/vga-bios.md).

An optional local `vgabios.bin` can be retained for comparison with
`--vgabios roms/vgabios.bin`. The existing local copy came from Homebrew QEMU's
`/opt/homebrew/share/qemu/vgabios.bin` (Bochs VGABIOS). Matching source and license:
https://www.nongnu.org/vgabios/

ROMs and operating-system images are local inputs, excluded from source control.
