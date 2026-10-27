# Quake shareware on MS-DOS

**Quake 1.06 shareware runs in the x86 interpreter on MS-DOS 6.22.** It renders
the start map, weapon and HUD and accepts keyboard movement. The 27 October 2026
headless smoke test matched the original health display, injected an Up-arrow
make/break through IRQ1 and captured two different 3D views. The second view
entered the normal-skill hall. Both captures remained in protected mode.

![Quake after walking into the normal-skill hall](measurements/quake-shareware-2026-10-27.png)

```sh
./run-quake.sh
```

The launcher selects the verified **interpreter backend with 32 MiB RAM**.
Use arrow keys to move; Ctrl fires, Space jumps, Escape opens the menu.
Sound, CD audio, joystick, DOS mouse and game networking are disabled in the
prepared AUTOEXEC.BAT. This is a silent keyboard-controlled session.
`QUAKE_DISK=/absolute/path/to/disk.qcow2` selects another prepared disk.

The JIT still fails in CWSDPMI's segment trampoline: a lock-memory request can
restore DS=0033 instead of the application selector 00AF, eventually causing a
page fault at Quake EIP 00044d65. Disabling basic-block joining does not resolve
it. The launcher deliberately uses the working interpreter; Quake performance
and JIT compatibility are not claimed.

## Prepare the local disk

Downloaded game files, DOS images and private working disks remain ignored.
No game assets or Microsoft files are distributed with this repository.

Download [Quake 1.06 shareware](https://www.dosgamesarchive.com/file/quake/quake106)
from the [FU Berlin idstuff mirror](https://ftp.fu-berlin.de/pc/games/idgames/idstuff/quake/quake106.zip).
The archive SHA-256 must be
`ec6c9d34b1ae0252ac0066045b6611a7919c2a0d78a3a66d9387a8f597553239`.

```sh
mkdir -p build/quake
curl -fL https://ftp.fu-berlin.de/pc/games/idgames/idstuff/quake/quake106.zip \
  -o build/quake/quake106.zip
shasum -a 256 build/quake/quake106.zip
unzip -oq build/quake/quake106.zip -d build/quake/archive
7zz x -y build/quake/archive/resource.1 -obuild/quake/game
python3 tools/probe_quake.py --dos /absolute/path/to/local-dos-base.qcow2 \
  --source build/quake/game --prepare-only --output media/quake.qcow2
```

Use a stopped DOS base containing C:\DOS\HIMEM.SYS. Keep its backing chain
available. If the base lives on the SMB volume, first copy the compact standalone
QCOW2 locally: QEMU backing-file locking is unsupported on that mount. The
recovered DOS base occupies about 9 MiB; no multi-gigabyte raw image is needed.
This checkout's prepared disk uses `media/quake-dos-base.qcow2` as its parent.

Preparation refuses an existing output. It creates a private compressed child,
verifies the shareware PAK hash, stages and reads back QUAKE.EXE, CWSDPMI.EXE and
PAK0.PAK, then replaces CONFIG.SYS/AUTOEXEC.BAT only in that child. Quake runs
from C:\ with its assets in C:\ID1 and executables in C:\QUAKE. Its default
`./id1` path resolves correctly there. The initial `-basedir C:\` experiment
failed to load the assets and is no longer used.

## Validate headlessly

```sh
python3 tools/smoke_quake.py
# One bounded cold-boot capture, without claiming gameplay automatically:
python3 tools/probe_quake.py --dos media/quake-dos-base.qcow2 \
  --source build/quake/game --no-jit --steps 700000000 --timeout 90
# Use --demo to play the original bundled demo1 instead of the start map.
```

The smoke uses a temporary compressed overlay of the prepared disk, selects the
interpreter and checks the original 320x200 health-display pixels. It pauses to
capture the first view, holds the forward key for at least 100 million guest
instructions and verifies a changed 3D scene in a second capture. It checks
protected-mode state and an orderly emulator exit. Success removes its disk;
failure retains it for diagnosis. Reports, frames and logs live under
`build/quake-smoke-*`. The image above is a direct emulator framebuffer capture.
This is a compatibility check, not a benchmark or a complete playthrough.

The generic startup probe retains failed disks and removes a normally completed
capture disk unless `--keep-disk` or `--output` is given. Exit zero alone means
only that the instruction budget completed.

## Implemented CPU support and limits

CWSDPMI bootstraps through 386 TSS far JMP/CALL transfers. The CPU now saves and
loads general registers, segments, EIP, flags, CR3 and LDT state; maintains TSS
busy bits and nested-task backlinks; handles NT IRET returns; and sets CR0.TS
for lazy floating-point switching. Focused tests cover the transfer and return.
16-bit TSS task switching and complete task-switch exception validation remain
outside this implementation.

The x87 interpreter covers real/integer loads and stores, stack arithmetic and
comparisons, integer rounding modes, common transcendental instructions and
FSAVE/FRSTOR. BIOS and CMOS report an installed coprocessor. Snapshot schema 9
preserves register values, control/status/tag words and environment fields;
versions 1–8 restore default FINIT floating-point state. Tests exercise rounding
ties, operand order, comparisons on read-only pages, a status store at a page
boundary, 80-bit memory stores, save/restore and snapshot compatibility.

Arithmetic uses binary64 internally. Bit-exact 387 extended precision, precision
control, full exception/stack-fault semantics and instruction/data-pointer
tracking remain incomplete. This is sufficient for the verified Quake workload;
it is not full 80387 conformance.
