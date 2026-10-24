The local `vgabios.bin` was copied from the existing Homebrew QEMU installation:
`/opt/homebrew/share/qemu/vgabios.bin`. This project uses its 8×8 character font;
it does not execute QEMU, link QEMU, or use QEMU's emulation code.

The VGA BIOS is the Bochs VGA BIOS distributed by QEMU. Obtain its matching
source and license through the installed QEMU package or the upstream project:
https://www.nongnu.org/vgabios/

ROMs and operating-system images are local inputs, excluded from source control.
