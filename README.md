# PCEmu — DOS and Windows on Apple Silicon

Machine presets and future virtualization targets: [machine profiles](docs/machine-profiles.md).
Use `--list-machines`, `--machine win311|win95` and `--backend auto|interpreter|jit`.

An original x86 software emulator in C++20, with an AArch64 baseline JIT and an
SDL2 window. **Boots the supplied MS-DOS 6.22 disk and Windows 3.11 in standard
mode to Program Manager.** The legacy PC path uses an original CPU core, with
no QEMU/DOSBox/Unicorn core or hypervisor. A separate experimental
[ARM64 Hypervisor.framework backend](docs/hypervisor.md) runs bare-metal test
guests on Apple Silicon (`make test-hvf`). Its
[ARM64 UEFI platform](docs/arm64-uefi.md) reaches the EDK II Shell and preserves
UEFI variables across VM instances (`make smoke-uefi`).
[Virtio PCI/MMIO storage](docs/virtio.md) boots ARM64 EFI applications and
preserves guest file writes in compressed COW deltas (`make smoke-virtio`).
The [minimal Debian ARM64 server](docs/debian-arm64.md) boots from installed
virtio storage and accepts SSH over virtio-net (`./run-debian.sh`).
[NVMe storage](docs/nvme.md) boots the original Windows 11-based ARM64
Validation OS VHDX to an administrator command prompt using its inbox driver.
[ICH9 HD Audio playback](docs/intel-hda.md) supports ARM64 Windows with a native
audio driver and is enabled by `./run11.sh`.
Interactive Windows input and Windows shutdown/restart validation remain work
in progress.

The supplied rtts Windows 95 image also reaches Explorer and runs Calculator
and Notepad. The original downloaded image is preserved; `media/win95.raw` is a
private working copy or a link to a preserved QCOW2 base. Use `./run95.sh` to open it.
The seamless-mouse disk `media/win95-mouse.qcow2` is selected by default when
available, providing a 1440×900, 32-bit desktop with a configured monitor profile. Use `./run95.sh --vga` for the
original VGA disk. Earlier monitor and SVGA disks remain available as fallbacks. Driver setup for a fresh disk is described in
[input and display integration](docs/input-video.md).
For a QCOW2 base, the launcher creates and reuses `media/win95-working.qcow2`,
a compressed writable overlay that retains Windows changes between runs. Keep
its base image available; raw disks and existing native overlays open directly. Network and configured Cirrus hardware are absent, so that older disk uses VGA
fallback. Original PS/2 mouse and VBE/DISPI display hardware are now available;
see [input and display integration](docs/input-video.md) for guest driver setup. Both the rtts image and independent OSR2 installation pass guest
restart, exact saved-file survival and clean shutdown to the full safe-to-turn-off
screen on the inline RAM backend. The separate OSR2
installer has completed installation. The current compressed installed disk is
`media/win95-fresh-installed.qcow2`, with backing in `build/snapshots`. Historical
intermediate raw disks have been removed; original media and saved checkpoints
are retained. Guest reset requests now have a firmware bootstrap path. A Win95 Restart
check returned to Explorer in the same process and verified exact Notepad file
contents after reboot (`build/win95-inline-restart` and `build/rtts-inline-restart`). Full ROM POST is not
implemented. See [status](docs/GOAL_STATUS.md) and [clean installer preparation](docs/win95-setup.md).

Office 97 Professional SR-1 now installs on the independent Windows 95 disk.
Word edits/saves documents and Excel calculates/saves workbooks; both saved files
reopen after a guest restart. Run `./run-office97.sh` for the configured
**1024×768, 32-bit desktop with PS/2 mouse and VMDisp9x display driver**,
or add `--setup` to repeat the original installer from a checkpoint. Separate
Word/Excel startup fixtures measure the rendered application, with five-run
SDL medians of **2.21 s / 1.02 s**, about 12.5% / 15.4% below the pre-profile
build. [Whole-startup profiling](docs/startup-profiling.md) identifies interpretation,
dispatch and device polling as the main remaining costs. Those timings use the
original VGA fixtures (`./run-office97.sh --vga`), which remain unchanged.
See [Office installation and benchmarks](docs/office97.md).

Quake 1.06 shareware now renders and accepts keyboard movement under MS-DOS 6.22
with the x86 interpreter. Run `./run-quake.sh` after preparing its private disk;
see [DOS Quake](docs/quake.md) for headless validation, setup and remaining x87/JIT
limits. `./run-quake.sh --benchmark` runs a bundled timedemo headlessly, reports
FPS and quits; add `--frames 50` for a short run. Sound is disabled.

## Run this checkout

```sh
./run.sh
```

This builds the emulator, boots `media/win311.raw`, and enters `WIN /S`. This
disk was installed by the complete Windows setup wizard and includes its normal
program groups. The launcher falls back to the older `media/dos.raw` if needed. The original
DOS VHD and Windows floppy archive are preserved.

For the native SwiftUI VM library, use `make app-native` and open
`build/PCEmu Library.app`. Existing Windows 3.11, Windows 95 and Windows 11 disks
are registered automatically. See [the native frontend and library API](docs/swiftui-frontend.md)
for snapshot restore, lifecycle controls, input capture and current engine limits.

For a double-click launcher tied to this checkout:

```sh
make app
open build/PCEmu.app
```

For a DOS prompt, use `make dos`. Press Ctrl+Q to close the emulator. Windows is
keyboard-operated: Option maps to Alt, and Tab/Enter/arrows and function keys are
available. Program Manager's Alt+F, R opens Run; try `notepad.exe` or `calc.exe`.
Mouse support is not implemented yet.

Dependencies are Xcode command-line tools, CMake, Ninja, SDL2, zstd and QEMU
(`brew install cmake ninja sdl2 zstd pkg-config nasm python qemu`). Build with `./tools/build.sh`
or `cmake -S . -B build -G Ninja && cmake --build build --target pcemu`.
Ninja caches individual objects, tracks included headers, and shares core objects
between executables. Development builds default to `-O1 -g`; opt into `-O3` with
`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release` (switch back with
`-DCMAKE_BUILD_TYPE=Debug`). Existing `make` build and test aliases delegate
compilation to Ninja. Dependencies are located through pkg-config. The original 32-KiB VGA/VBE 2.0 option ROM is assembled with NASM/Python and
embedded in the executable. Normal sessions execute it without external VGA
firmware files. `--vgabios FILE` selects a comparison ROM;
`--no-execute-vgabios` retains the host video BIOS path for diagnostics.
See [VGA BIOS](docs/vga-bios.md) for specifications, headless tests,
third-party diagnostics and compatibility limits. The system BIOS remains
host-backed; ARM UEFI remains a separate firmware dependency.

## Validate

[FreeDOS validation](docs/freedos-validation.md) provides a reproducible guest
fixture and headless CPU, memory-manager, mouse, storage and network driver checks.

```sh
make test
make test-freedos     # downloadable FreeDOS CPU + driver checks, interpreter/JIT
make test-freedos-vga # external VGA suites on FreeDOS, with QEMU comparisons
make bench
./build/pcemu --benchmark-memory # checked flat/paged load-add-store loops
make smoke-win311 # installed disk + tesseract required; private compressed overlay, file readback
make smoke-reset  # DOS requests warm, 8042 and fast resets; verifies second boot
make smoke-win95  # original rtts disk + tesseract; private compressed overlay
./build/pcemu --headless --disk media/dos.raw --steps 30000000 \
  --type $'cd windows\nwin /s\n' --frame build/desktop.ppm
sips -s format png build/desktop.ppm --out build/desktop.png
```

For a cold boot followed by a guest-requested restart, run
`python3 tools/smoke_win95.py --restart`. Use
`--disk media/win95-fresh-installed.qcow2` to verify the locally installed OSR2 disk.
The test handles known startup dialogs, checks an unobscured Explorer desktop,
and verifies exactly one firmware reset when restart is requested. Successful
runs remove their private disk; failures retain their owned guest and diagnostics
under `build/win95-smoke-*`. These are compatibility tests, not performance scores.

Add `--sample` to collect macOS host stacks through desktop verification (and
restart when requested). Named attachment waits are excluded from the summary;
OCR/dialog handling remains in the workload. `host-profile.json` and raw
`host-profile/run-1/sample.txt` are diagnostics, not isolated boot-time scores.

For application and file-persistence validation, run:

```sh
python3 tools/smoke_win95.py --applications
python3 tools/smoke_win95.py --applications --disk media/win95-fresh-installed.qcow2
```

This implies restart, calculates 2+2, copies Calculator's result into a fresh
Notepad file, and verifies every saved byte directly from the compressed disk
after restart and guest exit. `--sample` can be combined with this check.

The tests include exhaustive 8-bit ADC/SBB flags, randomized interpreter/JIT
comparisons with paging, remapping and code invalidation, VM86 transitions,
interrupt shadows, segment-fault restart, page permissions, ATA, PIC, PIT,
VGA planes/latches, BIOS keyboard/video, and disk-geometry register preservation.
The default benchmark is a **register-only synthetic loop**; the memory
benchmark checks flat and paged RAM loops. Their speedups do not establish
application speed or parity with Rosetta. Native blocks include guarded
8/16/32-bit MOV memory operands, including AH–BH byte registers, LEA,
register PUSH/POP and immediate PUSH. Memory ALU operations, other stack forms,
calls/returns and indirect control flow still fall back to the interpreter. Rosetta-class
whole-system performance remains an optimization target, not an achieved claim.
Ordinary RAM and valid TLB hits execute inline in ARM64. Checked memory loops
measure 2.08 billion guest instructions/s flat and 0.92 billion/s paged, about
35x and 22x faster than the interpreter. The matching Rosetta kernel reaches
12.35 billion equivalent guest instructions/s; substantial performance work
remains. See [performance measurements](docs/performance.md).

## Snapshots and repeatable tasks

Snapshots restore CPU, RAM, device queues, pending ATA transfers and matching
on-disk state. From a running instance with `--control-file build/control.txt`:

```sh
printf 'save-snapshot build/snapshots/my-point\n' >> build/control.txt
printf 'load-snapshot build/snapshots/my-point\n' >> build/control.txt
```

Or start directly from a checkpoint, with an isolated writable disk delta:

```sh
./build/pcemu --load-snapshot build/snapshots/win95-desktop
```

RAM state omits zero pages and uses zstd level 1. Disk changes and snapshot
layers use standard QCOW2. The first checkpoint shares its backing through an
APFS clone; later checkpoints and restores share immutable parents. Keep parent
checkpoints with their children. JIT code and host callbacks are rebuilt after
restore. The legacy PCEMUCOW reader remains read-only for migration and recovery.

Two local checkpoints are ready: `win95-desktop` and `win95-calculator-zero`.
The latter contains a recorded Calculator task with pixel checks for 0 and 4:

```sh
python3 tools/bench_snapshot_task.py build/snapshots/win95-calculator-zero \
  --task build/snapshots/win95-calculator-zero/task.json \
  --repeat 5 --output build/my-calculator-bench
```

Add `--no-jit` to compare the interpreter. Successful runs discard their private
disk deltas and temporary frames; failures retain the live guest and diagnostics.
Times include process, control, capture and polling overhead. The verified local
median is about 52 ms to restore and become ready. This task currently takes
about 119 ms with the JIT and 82 ms interpreted, exposing a performance target.
Use `--state-json FILE` for capture metadata without writing full RAM dumps.

## Reproduce media preparation

The tools expect the user's archives in this directory. The Microsoft media comes from the user
or the explicitly requested rtts image; no disk images are distributed with source.

1. Extract the Windows `.7z` and DOS VHD `.7z` under `media/` with macOS `tar`.
2. Run `python3 tools/prepare_media.py --minimal-dos`. It makes a private sparse
   `media/dos.raw`, with minimal CONFIG.SYS/AUTOEXEC.BAT and backups of the originals.
3. Run `python3 tools/stage_windows.py`. It stages all six Windows floppies in
   `C:\WINSETUP`. This works directly on the FAT16 working image.
4. Run the real installer first stage in the emulator:

   ```sh
   ./build/pcemu --headless --disk media/dos.raw --steps 210000000 \
     --type $'cd winsetup\nsetup\n\v\n\v\n'
   ```

   A fresh disk reaches graphical Setup. If retrying a failed installation, Setup
   asks an additional recovery question; use the window or add a scripted answer.
   The full graphical installer has now completed on the separate
   private `media/win311-install.raw` test disk (since retired after verification).
   The earlier apparent wait after name
   confirmation was driver decompression, not a confirmed stall. See
   [current verification status](docs/GOAL_STATUS.md).

The following steps describe the older minimal installation retained in
`media/dos.raw`; they are not required for the full installer path.

5. Run `python3 tools/configure_desktop.py` to change `SYSTEM.INI` from the Setup
   shell to `progman.exe`. Preserve or clone the disk before experimenting.
   Ensure CONFIG.SYS includes `DEVICE=C:\DOS\HIMEM.SYS /TESTMEM:OFF`.
6. In guest DOS, create WIN.COM with Microsoft's documented binary concatenation:

   ```text
   CD \WINDOWS
   COPY /B SYSTEM\WIN.CNF+SYSTEM\VGALOGO.LGO+SYSTEM\VGALOGO.RLE WIN.COM
   ```

7. Run `python3 tools/install_apps.py`, then run `C:\INSTALL.BAT` inside DOS. It
   uses Microsoft's own guest `EXPAND.EXE` for the compressed application files:

   ```sh
   ./build/pcemu --headless --disk media/dos.raw --steps 500000000 \
     --type $'install\n' --until-text 'Applications installed.'
   ```

8. Start `WIN /S`, or use `./run.sh`.

The older helper scripts write `media/dos.raw`; the independent
`prepare_win311.py` and `prepare_win95.py` scripts create separate installer disks. Never run two writable instances
against the same disk simultaneously. Historical `media/setup-stage.raw` and
other intermediate debug disks have been retired. Disk images, binaries, ROMs, and extracted Microsoft
files are excluded from source control.

## Debugging and scope

`--no-jit` uses the reference interpreter. `--verbose` logs BIOS services and DOS
file opens. `--trace FILE` records instruction state, and `--dump-memory FILE`
writes guest RAM on normal completion or an execution error. `--frame FILE`
exports a PPM framebuffer. `--steps N` limits headless execution, and `--until-text`
stops on a text-screen marker. A budget stop is not a boot-success assertion.

In `--type`, newline presses Enter, vertical tab (`\v`) waits for a blocking BIOS
keyboard read, and form feed (`\f`) waits until graphics mode has been active for
20 million instructions before continuing. Graphical input uses IRQ1 scan codes.
These controls make early boot tests reproducible without host UI automation.

This is an early compatibility implementation, not a complete PC. It supports
Windows standard mode; 386 enhanced mode, full CPU protection checks, complete x87
fidelity, audio and full ROM POST remain unfinished. Original NE2000 networking supplies
DHCP, DNS, TCP and UDP through macOS sockets; see [networking](docs/networking.md). PS/2 input and VBE
display integration are described in [docs/input-video.md](docs/input-video.md).
The original firmware reset entry and 8042/port-92 reset paths reboot through
the disk boot sector and optionally reinitialize the guest VGA ROM.
See [docs/architecture.md](docs/architecture.md) for the implementation and limits.

## Verified locally (Apple M4, 24 October 2026)

- 651,977 checks pass with `make test` (including 2,886 snapshot/COW checks).
- MS-DOS boots from the supplied disk's MBR and partition boot sector.
- `WIN /S` reaches Program Manager.
- `WIN /S NOTEPAD.EXE` opens Notepad and accepts `Hello from ARM` through IRQ1.
- `WIN /S CALC.EXE` evaluates keyboard-entered `2+2=` to `4`.
- Register-only benchmark: approximately 1.25 billion guest instructions/s with
  the JIT versus 51 million/s interpreted (about 24× on this workload).
- The SDL window initializes, but native UI automation could not inspect it
  because the Mac was locked. Framebuffers were verified directly instead.

Synthetic register and memory Rosetta baselines have been measured. A broad
application-performance comparison has not been completed.

NE2000 networking and Netscape 3.04 are configured in the `office97-networked`
checkpoint, now preferred by `./run-office97.sh`. Open the Netscape desktop icon
for the built-in HTTP test, then its external HTTP link. `--integrated` selects
the previous input/video checkpoint. See [networking](docs/networking.md).

Local storage was reduced from about 15 GiB to 3.4 GiB while preserving original
archives, current boot disks and snapshots. Smoke tests now discard successful
scratch disks by default. See [storage cleanup](docs/storage.md).

QCOW2 v2/v3 images are detected by their header. Immutable standalone bases
support sparse, zero, deflate and zstd-compressed clusters through a cached native
reader. Writable disks and backing chains use QEMU's maintained QCOW2 implementation
through a private Unix NBD socket (`qemu-nbd`); PCEmu does not implement its own
refcount or allocation writer. Each backend is owned and reaped by its disk.
Flush requests reach QEMU before snapshots and orderly shutdown.

```sh
./build/disk-tool overlay build/guest.qcow2 /path/to/base.qcow2
./build/pcemu --disk build/guest.qcow2
```

The overlay is standard QCOW2, readable by `qemu-img` and other compatible tools.
New writes use ordinary QCOW2 clusters for write performance. Offline conversion
packs them with zstd; compression is not performed on every guest write.

To flatten a stopped disk into a standalone zstd-compressed QCOW2 file:

```sh
./tools/build.sh disk-tool
python3 tools/convert_qcow2.py SOURCE NEW.qcow2 --size GUEST_SIZE_BYTES
```

This uses an existing read-only NBD bridge, avoids a raw staging copy, runs
`qemu-img check`, and compares every guest-visible byte before publishing output.
The source is retained. To migrate a set of legacy `.pcd` images together:

```sh
python3 tools/migrate_qcow2.py media/old.pcd media/child.pcd \
  --remove-originals --report build/storage-cleanup/migration.json
```

All sources and ancestors must be stopped. The migration tool locks the chain,
refuses unlisted dependents, and completes every verified conversion before
removing originals. Snapshot disks with legacy contents also need conversion
before writable restore. Never delete a backing file still used by another disk.
See [storage](docs/storage.md) for implementation and validation details.
