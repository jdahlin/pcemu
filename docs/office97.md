# Office 97 on Windows 95

Current recovery (2026-10-25): DOS, Win3.11, rtts and a fresh Windows 95
OSR2 installation have been recovered on `/Volumes/data/pcemu/recovered/`.
Win3.11 and Win95 now have standalone QCOW2 bases. Office 97 Professional
SR-1 was installed again into `office97.pcd`, a zstd compressed writable child
of the immutable `win95.qcow2` base. Its ISO remains external; all 5,781 staged
files were read back before Setup. No raw staging disk or extracted tree was
created.

Word saved and reopened `C:\RECOVER.DOC` after a firmware restart. Excel
saved and reopened `C:\RECOVER.XLS`, preserving A1's `=2+2` formula and value
4. Stopped-guest readback verified both compound-file headers/hashes and the
Word text payload. The new compressed checkpoint is
`/Volumes/data/pcemu/snapshots/office97-recovered-desktop`; an independent
headless restore reached Explorer. `build/snapshots/office97-desktop` points
to it, so `./run-office97.sh --vga` uses this recovered VGA installation.
The checkpoint adds about 10 MiB; its disk is a small child above the installed
Office disk, avoiding another full Office copy. See
[recovery evidence](measurements/office97-qcow2-recovery-2026-10-25.json).

Earlier Office overlays and performance checkpoints still refer to a missing
parent. They cannot be substituted onto the new base: their old measurements
remain historical. A replacement dependent-recalculation fixture is now validated on the recovered
chain; use the external checkpoint described below for new comparisons. The remaining sections describe the earlier
installation, driver setup and measurements; those old checkpoint paths have
not been restored by this recovery.

The supplied Office 97 Professional SR-1 media installs through Microsoft's
normal Setup in the independent Windows 95 OSR2 installation. Word edits and
saves `C:\WORD97.DOC`; Excel calculates `=2+2` as 4 and saves
`C:\EXCEL97.XLS`. Windows returns to Explorer after an actual guest restart.
The saved files reopen after restart (`build/office97-reopen/word.png` and
`excel.png`). Installation evidence is in `build/office97-install`, including `setup-complete.png`,
`word-saved.png`, `excel-saved.png`, and `restart/result.json`.

Run the configured 1024×768, 32-bit Office desktop with the Windows 95
PS/2 mouse driver and VMDisp9x display driver:

```sh
./run-office97.sh
```

Use Ctrl+Esc, R to open Run, then enter `winword` or `excel`. Each launch of
the emulator restores the checkpoint into its own writable disk delta.
To repeat installation, `./run-office97.sh --setup` resumes the original
wizard before choosing Typical and Continue. Its name and key have already
been entered using the supplied media. This does not modify the installed
checkpoint. `--vga` restores the original 640×480 desktop used by the existing
startup benchmarks. Click inside the window to capture the mouse; Cmd+G releases
it and Cmd+Q quits. F12 reaches Word's Save As command. See
[input/video integration](input-video.md) for driver and hardware details.

The integrated checkpoint is `build/snapshots/office97-integrated`. Its 18 MB
zstd state includes the configured devices and VRAM. Its 2,453,493-byte compressed
disk delta has the original Office desktop disk as its immutable parent; the
consolidation was verified over all 2,147,483,648 logical bytes. Driver setup did
not require another raw disk image. Evidence under `build/win95-integration`
includes Word reopening `WORD97.DOC`, Word's F12 Save As dialog, Excel reopening
`EXCEL97.XLS` with `=2+2` displayed as 4, and the mouse-driven Excel context menu.

## Repeating startup measurements

The existing VGA benchmark fixtures remain unchanged. Their timings are not
measurements of the new higher-resolution display driver. Both fixtures derive
from the same desktop after a Windows restart, with
first-run name/Assistant prompts handled and both applications closed. The
Run dialog is filled in, awaiting Enter. Each sample has a fresh JIT cache and
the same guest RAM, devices, and Windows file-cache state. The host filesystem
cache is not flushed. These are reproducible post-boot application launches,
not measurements of cold physical storage.

```sh
python3 tools/bench_snapshot_task.py build/snapshots/office97-word-startup \
  --task build/snapshots/office97-word-startup/task.json \
  --repeat 5 --output build/word-startup-new
python3 tools/bench_snapshot_task.py build/snapshots/office97-excel-startup \
  --task build/snapshots/office97-excel-startup/task.json \
  --repeat 5 --output build/excel-startup-new
```

Add `--no-jit` for the reference interpreter. Use a fresh output directory
for each experiment. Timing begins when Enter is sent and ends at an exact
framebuffer match of Word's rendered page/section status or Excel's Ready
status. This avoids mistaking a splash screen for the application. Times
include control/capture/poll overhead. `executionWallMs` measures CPU/device
dispatch batches and excludes captures, video refresh and halted sleeps;
it still includes host preemption. Instruction and native-instruction deltas
help distinguish coverage changes from latency noise. The first matching
status bar does not prove every background initialization task has finished.

Successful runs remove their own writable deltas and temporary framebuffers;
failures leave the guest and diagnostics alive for inspection.

## Media and disk layout

The original `.7z` download is retained. ISO files were staged directly into
`C:\OFFICE97` without retaining an extracted file tree, and all 5,781 files
were read back against the ISO before running Setup. The temporary ISO and
raw staging copy were removed after successful installation. Installer files
remain inside the guest for repairs or additional components.

`media/office97-base/disk.raw` is an immutable APFS clone of the pre-Office
Windows disk. Checkpoint disks use compressed COW records. The historical
`media/office97-win95.pcd` working copy was removed after proving it identical to
the retained Office desktop disk. Snapshot copies share APFS extents; restores add small private
children. Keep the base, checkpoint directories and their absolute parent
paths in place. The format is currently intended for this local checkout.

For staging other 8.3 ISO9660 media into an offline disposable FAT16 raw copy:

```sh
python3 tools/stage_iso.py SOURCE.iso PRIVATE.raw --directory OFFICE97
make build/disk-tool
build/disk-tool import-diff IMMUTABLE-BASE.raw PRIVATE.raw RESULT.pcd
```

The staging tool verifies all file payloads through the resulting FAT chains.
`--verify-only` repeats that read-back without staging again. Do not stage or
convert a disk attached to a running guest. A failed staging copy is disposable;
the original image should remain the immutable parent.

## Measured startup improvements

Five independent restores per cell, measured on the local Apple M4:

| Application | Initial JIT | LEA and stack JIT | Interpreter |
| --- | ---: | ---: | ---: |
| Word 97 | 2.522 s | 2.210 s | 2.214 s |
| Excel 97 | 1.184 s | 1.048 s | 0.979 s |

The JIT improvements are about 12% for Word and 11% for Excel. Word is roughly
at interpreter parity; Excel still has JIT overhead. These measurements do
not demonstrate Rosetta-class performance. Native coverage on Word startup
increases from 18.7% to 31.3%; many instructions remain interpreted.

Median process-to-checkpoint-ready time drops from about 1.06 s to 0.24–0.25 s
by mapping compressed disk record headers instead of seeking for every record.
Individual first runs can be much slower; all samples, including those outliers,
are retained. The execution-batch medians show the startup improvement too:
Word 2.216 s to 1.944 s, Excel 1.040 s to 0.914 s.

Raw results: `build/office97-{word,excel}-{baseline,interpreter,lea-index,stack}`.
`build/office97-startup-summary.json` gathers the medians. The pre-optimization
executable is retained as `build/pcemu-office97-baseline` for comparisons.
The full suite passes 539,036 checks, including 23,192 stack JIT checks for
mixed widths, page/segment faults, SP aliasing, MMIO and self-modifying code.
Windows 3.11 save-file and DOS warm/KBC/fast-reset smoke tests also pass on the
optimized backend.

A subsequent [whole-startup profile](startup-profiling.md) includes the SDL
frontend and identifies decoder exceptions and repeated interrupt-priority
scans. Removing those costs gives unprofiled alternating-pair SDL medians of
2.524 → 2.209 s for Word and 1.210 → 1.023 s for Excel. Earlier tables above
are retained as headless measurements of the preceding versions.


## Direct compressed installer staging

`tools/stage_iso.py ISO PRIVATE.pcd` detects PCEMUCOW disks and writes through
`CowFile`, keeping the parent immutable. Create a new private child with
`build/disk-tool overlay PRIVATE.pcd IMMUTABLE-PARENT` first. No raw staging
copy or extracted ISO tree is required. `--verify-only` rereads the staged
payloads against the ISO. Both forms must be used only with offline disks.
`python3 tests/test_stage_iso.py` covers raw/COW payload verification, duplicate
staging rejection and unchanged parent bytes using tiny synthetic media.

## Recalculation workload on the restored installation

`/Volumes/data/pcemu/office97-steady-use.pcd` is a small child of the restored
Office installation, containing `C:\PERF4096.XLS`. B1:B4096 contain
`=ROW()*3+7`; the selected-range sum is 25,200,640 and B4096 is 12,295.
The reopened original workbook preserved A1's `=2+2` formula and value 4 after
Windows restart. Screenshots and diagnostic fill/recalculation counts are in
`measurements/office97-excel-steady-use-2026-10-25.json` and adjacent files.

The recalculation interval sends eight Ctrl+Alt+F9 requests after load. These
are opcode coverage observations, including guest message/idle handling and
input settling, not elapsed-time benchmarks or per-opcode host costs. The
control transcript records the exact input. Repeated timing needs fixed task
boundaries and validated completion, plus an idle baseline; do not infer a
speedup from diagnostic counts alone. The saved workbook has an offline
compound-file header/hash check; its full formula range has not been parsed
from BIFF or reopened after this new save.

## Reproducible dependent recalculation task

`/Volumes/data/pcemu/snapshots/excel-dependent-recalc` retains a compressed
13 MiB RAM snapshot plus a small disk child. Excel is already loaded, D1 is 3,
B1:B4096 contain `=ROW()*$D$1+7`, and C1 contains `=SUM(B1:B4096)`. The task
changes D1 to 4 and waits for C1's exact rendered total to change from
25,200,640 to 33,591,296. This dependency requires recalculation, rather than
merely detecting an already-visible Ready label.

```sh
python3 tools/bench_snapshot_task.py \
  /Volumes/data/pcemu/snapshots/excel-dependent-recalc \
  --task docs/measurements/excel-dependent-recalc-2026-10-25.task.json \
  --restore-timeout 90 --repeat 3 --output build/excel-recalc-new
```

The restore timeout accommodates the SMB-mounted external volume separately
from the task's 30-second completion timeout. One independent positive run
passed at 12.58 seconds end-to-end task response (11.04 seconds execution
batches). This is a single observation, not an optimization speedup. A no-input
negative stayed at the exact old-total pixels and was rejected; its owned guest
and restore child were cleaned up. Task timing includes control/capture/poll
work and starts with a cold JIT cache. Boot, load and restore are outside it.
Evidence: `measurements/excel-dependent-recalc-2026-10-25.json`. Paired baseline/
candidate timings and steady-state cache measurements remain to be collected.

## Recovered dependent-recalculation fixture

`/Volumes/data/pcemu/snapshots/excel-dependent-recalc-recovered` replaces the
broken earlier checkpoint. It adds about 14 MiB of compressed state plus a
110 KiB disk child, sharing the recovered Office desktop and immutable
QCOW2-backed installation. Keep every ancestor named in its recovery manifest.

B1:B4096 contain `=ROW()*$D$1+7`, D1 starts at 3, and C1 contains
`=SUM(B1:B4096)`. The task changes D1 to 4 and waits for C1 to change from
25,200,640 to 33,591,296. The same checked-in task's before/after pixels match
exactly. An independent restore passed, and a no-input negative control retained
the initial pixels and correctly failed the completion gate. The saved workbook
also has a stopped-guest compound-file header/hash check; BIFF formulas were
validated in Excel rather than parsed offline. Successful/negative test children
and the source session child were removed.

```sh
python3 tools/bench_snapshot_task.py \
  /Volumes/data/pcemu/snapshots/excel-dependent-recalc-recovered \
  --task docs/measurements/excel-dependent-recalc-2026-10-25.task.json \
  --restore-timeout 90 --repeat 3 --output build/excel-recovered-measurement
```

The initial observed NAS restore took about 29 seconds; restore readiness has
its own timeout and is excluded from task response timing. This fixture
validation is not a speedup comparison.
[Recovery and positive/negative evidence](measurements/excel-recovered-task-2026-10-25.json).
