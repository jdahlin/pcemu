# Office 97 on Windows 95

Current local availability (2026-10-25): cleanup removed the installed Office
checkpoints and the installer ISO. The retained `media/office97-base/disk.raw`
is a pre-Office Win95 baseline; it boots Explorer but does not contain Word or
Excel. The historical installation and measurements below do not establish
that Office is currently runnable. The user requested disk-light testing, so
current work uses RAM-only benchmarks and existing OS images rather than
recreating the Office installation or large startup fixtures.

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
