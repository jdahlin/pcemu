Normal x86 sessions use PCEmu's original embedded VGA/VBE option ROM. Building
it requires NASM and Python; see [VGA BIOS](../docs/vga-bios.md).

An optional local `vgabios.bin` can be retained for comparison with
`--vgabios roms/vgabios.bin`. The existing local copy came from Homebrew QEMU's
`/opt/homebrew/share/qemu/vgabios.bin` (SeaVGABIOS, verified by embedded identification strings). Matching source and license:
https://github.com/coreboot/seabios/tree/master/vgasrc

ROMs and operating-system images are local inputs, excluded from source control.

For PCI option-ROM discovery under QEMU, use its `vgabios-stdvga.bin` build.
The local `vgabios.bin` is a legacy/ISA image without a PCIR structure; direct
PCEmu initialization can execute it, but QEMU PCI discovery need not load it.
