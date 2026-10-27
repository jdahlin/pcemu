# FreeDOS x86 validation

FreeDOS provides a downloadable, open-source guest fixture for headless x86
acceptance tests. No MS-DOS installation, host mount, GUI installer or existing
private OS disk is required.

```sh
make freedos                # fetch/verify/build the cached fixture
make smoke-freedos          # boot and driver profiles, JIT
make test-freedos           # profiles + CPU checks, interpreter and JIT
make test-freedos-extended  # DOSEMU-derived HMA checks, interpreter and JIT
make test-freedos-vga       # external VGA edge suites, interpreter/JIT + QEMU
```

The ordinary `make test` includes the offline FAT/startup tests and native
CPU/device/ROM regressions. Downloadable guest tests are explicit targets so an
ordinary build never needs installation media or network access. `FREEDOS_DISK`
can select a different generated fixture path. Individual checks are available:

```sh
python3 tools/smoke_freedos.py --profile xms --no-jit
python3 tools/bench_guest_perf.py --disk build/freedos/freedos.qcow2 --suite micro --loops 1024
python3 tools/smoke_vga_edges.py --disk build/freedos/freedos.qcow2 --case doslib
```

Requirements: Python 3, curl, NASM, QEMU's `qemu-img`, and the normal native build
dependencies. CPU microkernels also require Clang and an ELF-capable `ld.lld`
(the existing guest-performance builder locates the configured LLVM installation).
VGA comparisons additionally require QEMU TCG, Pillow and Tesseract. No guest
run opens a GUI.

## Fixture and provenance

`tools/freedos_fixture.py` pins [FreeDOS 1.4 LiteUSB](https://www.freedos.org/download/)
to SHA-256 `857dcd2ebf9d3d094320154db5fb5b830acba6fb98f981a95a0ca7ab3350338b`,
matching the distribution's [verification file](https://download.freedos.org/1.4/verify.txt).
The download is about 17 MiB; its bootable disk is 32 MiB logical, compressed
as `build/freedos/freedos.qcow2`. It retains the original distribution packages,
including source and license material. The loader never executes the installer.

The small [FDNet source/binary archive](https://gitlab.com/FreeDOS/net/fdnet/-/tree/1fa2d28a107f726bb1cc11087b2b9a68ab345fc8)
is pinned to revision `1fa2d28a107f726bb1cc11087b2b9a68ab345fc8`, SHA-256
`b770ce088a23b2b0f223989d74cf8072c4511eb8f5262f5bc7a21eb7a50af6db`.
It supplies the unmodified Crynwr NE2000 packet driver and mTCP DHCP client;
its source and license remain in the ignored download cache. HIMEMX, JEMMEX and
CuteMouse come from the pinned LiteUSB distribution.

Only the generated startup files and a small `DRIVERS` directory differ from
the upstream disk. Both preferred FreeDOS names (`FDCONFIG.SYS`, `FDAUTO.BAT`)
and standard DOS names are staged explicitly. FreeCOM is told which batch file
to run, avoiding silent startup-file precedence errors. Cached fixture hashes
are checked before reuse; a modified or stale fixture is rejected. After an
intentional fixture-builder update, remove the generated parent and its JSON
manifest and run `make freedos` again.

Each run freezes the emulator, ROM and probe inputs, uses its own small
compressed overlay, records hashes/results under `build/freedos-<profile>-*`,
and removes successful child disks. The parent hash must be unchanged. Failed
runs retain their child disk and logs for diagnosis. Downloads, disks and
binaries stay out of Git.

## Acceptance checks

`tests/freedos/drivers.asm` is an original DOS COM probe with exact PASS records
and nonzero failure status. Driver banners are not accepted as proof of working
hardware. DOS redirects the probe's output to a guest file; the host reads and
validates that file only after the guest process has stopped.

| Profile | Checks |
|---|---|
| Minimal | FreeDOS/FreeCOM startup; create/write/close/reopen/read/compare/rename/delete an 8192-byte file; ATA IDENTIFY and PIO sector read compared against BIOS INT 13h; BIOS keyboard ring insert/peek/read; PIT interrupt tick advancement. |
| XMS | HIMEMX loading/HMA use; allocate extended memory, move 8192 bytes into it and back through the installed XMS API, compare, free; CuteMouse PS/2 detection, position set/get, and actual host-injected motion received through IRQ12. |
| EMS | JEMMEX protected/V86/paging operation; XMS checks; allocate four EMS pages, map/write/remap/read/compare all 65536 bytes, release; the same mouse IRQ check. |
| Network | Crynwr NE2000 at I/O 300h, IRQ10, packet interrupt 60h; mTCP DHCP exchange with the local emulator gateway; require an acknowledged offer and committed IP address 10.0.2.15/gateway 10.0.2.2; minimal probe checks also run. |

The mouse test clears motion counts, prints a readiness fence, and blocks for a
key. The host injects a PS/2 motion packet before releasing that fence; the
installed guest driver must then report nonzero motion. This tests the actual
controller/IRQ/BIOS callback/driver path, not only INT 33h software positioning.

The network profile enables only the emulator's unprivileged socket gateway.
DHCP is answered locally; the test needs no public server, host interface
configuration, privileged networking or external connectivity.

The CPU matrix reuses the existing original 16-bit real-mode and 32-bit
protected-mode microkernels: ALU, memory updates, branches, call/return, REP copy,
LOOP, byte exchange and REP fill. All 32 cold/warm intervals have independently
calculated checksums and must return to DOS. The short 1024-loop acceptance runs
are correctness checks, not publishable benchmark scores. The minimal profile
has no memory manager because that loader owns its temporary extended-memory
region. Driver profiles test memory managers separately.

`make test-freedos-vga` runs the [external VGA edge matrix](vga-bios.md) on this
same minimal FreeDOS parent, including DOSLIB, VGATEST, Abrash and X-VESA. Its
QEMU comparisons are functional checks, not physical raster-timing evidence.

## Boundaries and findings

The emulator currently exposes writable storage in parts of the ISA upper
memory hole. JEMMEX's automatic scan consequently regards that area as occupied.
The EMS profile explicitly reserves E0000h..EFFFFh for the EMS frame with
`I=E000-EFFF FRAME=E000 X=C000-DFFF X=F000-FFFF`. This validates EMS mapping at a
known board layout; it does not establish correct automatic option-ROM/UMB
hole discovery. Full ROM write protection and ISA open-bus decoding remain
separate emulator work.

This setup found and fixed two firmware/diagnostic issues: native text transcripts
read the character plane instead of a stale RAM shadow, and INT 15h/C0h now
advertises the installed second 8259 and RTC without claiming a nonexistent
EBDA. The latter lets the real packet driver select IRQ10.

These tests cover the implemented PC/AT, VGA, ATA, PS/2 and NE2000 paths. They do
not imply support for every FreeDOS driver, ATAPI/CD-ROM, USB, sound, every x86
instruction or exact hardware timing. Results are recorded in
[the validation measurement](measurements/freedos-2026-10-27.json).

The [external x86 test tiers](x86-external-tests.md) add a small offline hardware
sample to `make test` and keep broader CPU suites behind separate targets.

The [latest conformance record](measurements/x86-conformance-2026-10-27.json)
rechecks all baseline profiles, CPU checksums, HMA and both VGA matrices after
segment-limit enforcement. HIMEMX's real-mode DS/ES reloads retain their flat
cached limits; the fixture and upstream driver binaries are unchanged.
