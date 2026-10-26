# Word 97 launch on the M1 Pro: Instruments profile

Measured 26 October 2026 on an Apple M1 Pro (`MacBookPro18,1`) with Xcode
`xctrace` 16.0. This is the current `main` x86 emulator built `Debug -O1 -g`.
The task starts at a restored Windows 95 Run dialog containing `winword` and
ends when the first Word document displays `Page 1 / Sec 1`. The Windows OS is
warm in the checkpoint; the emulator's JIT cache is cold on each restore.

## Local checkpoint and task

The old checkpoint was on `/Volumes/data`. Its 137 KiB legacy disk delta
depended on an older Office disk chain. `tools/convert_qcow2.py` flattened the
guest-visible disk directly into a standalone, zstd-compressed QCOW2 at
`build/snapshots/word-launch-local/disk.raw`. The local disk is 621 MiB for a
2 GiB guest-visible disk; `qemu-img check` and whole-disk guest-byte comparison
passed. The 14 MiB RAM snapshot and preview were copied beside it. The
original files remain on `/Volumes/data`. The local directory is ignored by Git.

The exact RGB readiness patch and Enter command are in
[the task fixture](measurements/word-m1-instruments-2026-10-26.task.json).
It passed a headless restore and launch before profiling. Successful runs used
small temporary QCOW2 children and removed them after clean shutdown.

## Capture method

`bench_snapshot_task.py --instruments` attaches the **Time Profiler** to the
emulator PID, waits for the recorder to start, then crosses the existing named
`profile-wait` attachment barrier. It writes UTC timestamps immediately before
sending Enter and after the expected Word pixels are observed. It stops and
saves the trace before guest shutdown. `analyze_instruments.py` uses the trace's
start timestamp to retain only CPU samples inside that task interval. It
resolves Xcode's XML frame references, counts the main thread separately from
workers, and recognizes anonymous ARM64 JIT addresses under a JIT call.

Three independent headless traces contain **3,087 main-thread CPU samples**
inside the verified task windows (1,041, 1,043, 1,003 per run). Time Profiler
samples are statistical shares of on-CPU work, not exact function durations.
The interval includes repeated framebuffer capture and host control work. A
separate `PCEMU_PROFILE` run supplies guest instruction and dispatch counts;
its timings are not used as a benchmark.

| Sampled task CPU | Share | Range across runs |
| --- | ---: | ---: |
| Interpreter, MMU and guest faults | 30.7% | 30.6–31.0% |
| JIT dispatch, cache and validation | 24.3% | 23.3–25.9% |
| Generated ARM64 and native helpers | 13.7% | 12.4–14.4% |
| JIT compilation | 12.3% | 11.3–13.2% |
| Frame capture, video and control | 11.0% | 10.7–11.2% |
| Other host work | 4.4% | 3.8–4.7% |
| Device polling and interrupts | 3.6% | 2.9–3.9% |

The approximately 250 ms quartile windows are not separate guest phases: Windows
loading, Word initialization and capture activity overlap. In equal-duration
task quartiles, compilation rises from 4.6% in the first to 17.8% in the
second, then 15.3% and 11.7%. Interpreter plus dispatch stays above 50% in
each quartile. The display/capture category stays around 10–12% throughout.

The largest individual self functions are `CPU::stepImpl` (10.2%), `Jit::run`
(7.3%), `CPU::physical` (6.1%) and `Jit::runCached` (4.8%). These self counts
overlap the category analysis and must not be added to it. `Jit::run`'s Xcode
source line is reported as 0, so its samples identify the function but not a
specific statement. Anonymous generated-code PCs account for most of the
generated-code category. `CPU::stepImpl` samples span instruction fetch and
individual interpreted operation cases, rather than one single opcode.

The separate guest-counter run executed **32.1 million guest instructions**,
including **7.78 million interpreted attempts**, **14.64 million dispatches**
and **5.72 million negative-cache hits** (39.0% of dispatches). MOV `8B` is
7.9% of interpreted attempts, extended `0F` 5.7%, byte shifts 3.4% and string
byte loads 3.3%. Opcode attempts are not CPU shares; common supported opcodes
also get interpreted in cold or short fragments. Only 16 read sectors and 58
write sectors were counted during this task; disk and compression do not show
up as a material sampled CPU category.

## Direction from this profile

The highest combined costs are interpreted fragments and per-dispatch cache,
permission and page checks. A focused next experiment should reduce repeated
work at those boundaries while preserving exact faults, code invalidation and
interrupt points. Compilation is meaningful for a cold Word launch, but its
12.3% share rules it out as a sole route to a large improvement. Lowering one
common opcode should be evaluated against complete launch CPU time, because
the interpreted counts include many cold or short fragments.

Three unprofiled restores also passed (process CPU 1.077, 1.086, 1.102 s).
The profiled runs executed 34–35 million guest instructions while these ran
31–32 million; task completion can include varying trailing guest work and
capture. The two sets are not a before/after speed comparison, nor are these
M1 numbers directly comparable with the earlier M4 executable and fixture.

## Reproduce and inspect

```sh
cmake --build build --target pcemu pcemu-profile disk-tool -j 4
python3 tools/bench_snapshot_task.py build/snapshots/word-launch-local \
  --task docs/measurements/word-m1-instruments-2026-10-26.task.json \
  --emulator build/pcemu --restore-timeout 30 --repeat 3 --instruments \
  --output build/word-m1-instruments-new
python3 tools/analyze_instruments.py build/word-m1-instruments-new \
  --output build/word-m1-instruments-new/summary.json
open -a Instruments build/word-m1-instruments-new/run-0/instruments.trace
```

[The compact measurement record](measurements/word-m1-instruments-2026-10-26.json)
includes per-run timing, process CPU, category/source attribution, guest
counters, hashes and trace directory identities. The three original `.trace`
bundles and full XML/JSON analysis are in `build/word-m1-instruments-repeat/`
on this Mac. They are ignored build artifacts, not committed VM media.
