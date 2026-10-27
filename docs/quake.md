# DOS Quake shareware startup probe

Quake 1.06 shareware does **not yet run** in the x86 emulator. On 27 October
2026, both JIT and interpreter cold boots of MS-DOS 6.22 reached the original
CWSDPMI executable, then stopped with:

```text
unsupported far transfer to system descriptor at 0018:00002b78
CR0=80000011 CR3=0002e000
```

The failure occurs around 10.1 million guest instructions with 32 MiB RAM,
before Quake displays its startup console or renders gameplay. This is a CPU
protected-mode compatibility failure, not evidence of a VGA or sound problem.
`CPU::farTransfer` currently supports call gates but rejects other system
descriptor types. The exact target descriptor and required transition still
need investigation; the diagnostic address above is the calling instruction.

A second known requirement is x87 arithmetic. The bundled Quake README explicitly
requires a floating-point processor. Our D8–DF handlers currently implement an
absent coprocessor: they decode instructions without floating-point results
when CR0.EM/TS are clear. Working DOS/Windows integer workloads do not establish
Quake compatibility. Resolving CWSDPMI alone will therefore not establish success.
Audio is disabled for this probe.

## Reproduce

Download the original [Quake 1.06 shareware archive](https://www.dosgamesarchive.com/file/quake/quake106),
also available from the [FU Berlin idstuff mirror](https://ftp.fu-berlin.de/pc/games/idgames/idstuff/quake/quake106.zip).
Keep all downloaded/extracted files under ignored `build/` or `media/`.
The tested archive SHA-256 is
`ec6c9d34b1ae0252ac0066045b6611a7919c2a0d78a3a66d9387a8f597553239`.

```sh
mkdir -p build/quake
# Save quake106.zip in build/quake first.
unzip -oq build/quake/quake106.zip -d build/quake/archive
7zz x -y build/quake/archive/resource.1 -obuild/quake/game
python3 tools/probe_quake.py --dos /absolute/path/to/local-dos-base.qcow2 \
  --source build/quake/game
# Repeat with --no-jit to use the reference interpreter.
```

Use a stopped, locally readable MS-DOS disk with C:\DOS\HIMEM.SYS. The recovered
DOS base on the SMB volume needs a local copy (about 9 MiB compressed), because
QEMU backing-file locks are unsupported on that mount. Do not flatten it into
a multi-gigabyte raw image.

The tool verifies the original shareware PAK hash, creates a private compressed
child, stages and reads back QUAKE.EXE, CWSDPMI.EXE and PAK0.PAK, then boots
headlessly with 32 MiB. It replaces CONFIG.SYS/AUTOEXEC.BAT only in that child.
`-basedir C:\` allows the existing FAT writer to stage QUAKE and ID1 as root
folders. It captures stdout, stderr, framebuffer, CPU/device state and a report
under `build/quake-probe-*`. Failures retain their private disk; a normal capture
removes it unless `--keep-disk` is supplied. Exit zero means only that the
instruction budget completed, and is never a gameplay assertion.

Next compatibility gates are a completed CWSDPMI startup, correct x87 behavior,
and a visibly rendered shareware demo advancing across multiple frames. No
playability or performance claim is made by the current capture.

The 386 TSS far-JMP/CALL and nested IRET implementation now passes focused
save/load, backlink, busy-bit and CR0.TS checks. The original CWSDPMI bootstrap
advances past its task transfer; the next observed screen is Quake's SIGFPE
"Coprocessor not available" at EIP 0005425b. This supersedes the initial
protected-mode stop above, but still does not establish gameplay.

The x87 implementation now executes stack loads/stores, real/integer arithmetic,
comparisons, rounding-controlled integer conversions, common transcendental
operations and 14/28-byte environments with 80-byte register images. Snapshots
use schema 9 and include the floating-point registers and environment; older
snapshots default to FINIT state. BIOS/CMOS advertise the coprocessor. Nested-task
IRET declines the JIT fast return helper and uses the task-switch implementation.

Arithmetic currently uses binary64 internally: extended precision, full exception
mask/timing semantics, stack-fault handling and some rare instructions remain
incomplete. This is a compatibility implementation, not full 80387 conformance.
Focused tests cover rounding modes/ties, arithmetic operand order, compare flags,
80-bit stores and FSAVE/FRSTOR; snapshot tests cover live floating-point state.
