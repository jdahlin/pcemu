# Generated ARM64 debugging with Instruments

`--jit-debug-map FILE` records a timestamped sidecar for generated code. It uses
the normal 16 MiB JIT arena, including ordinary cache recycling. Collection
copies emitted code and metadata into memory; JSON formatting and disk writes
occur on clean guest shutdown, after the benchmark stops Instruments. This
adds diagnostic memory and compilation overhead, so these timings are not
performance scores. A crash can leave an incomplete map.

```sh
python3 tools/bench_snapshot_task.py build/snapshots/word-launch-local \
  --task docs/measurements/word-m1-instruments-2026-10-26.task.json \
  --emulator build/pcemu --repeat 1 --restore-timeout 30 \
  --instruments --jit-debug-map --output build/word-jit-debug-new
python3 tools/analyze_instruments.py build/word-jit-debug-new \
  --output build/word-jit-debug-new/summary.json
```

This enriches exported Instruments samples through the analyzer. It does not
register a Mach-O image or dSYM with Instruments, so the native Instruments UI
still displays anonymous addresses for generated code. Ordinary C++ symbols
and source lines remain available there. Guest block names identify CR3, CS and
IP; recovering Windows DLL/function names requires a separate guest symbol map.

## Recorded information

The JSONL file begins with a format version, PID, arena capacity and clock
definition. `reset` events close the previous arena lifetime. Each `load` has:

- The publication timestamp in Unix nanoseconds and arena generation.
- A half-open host address range and exact generated ARM64 bytes in hex.
- Guest CS, CS base, IP, CR3, physical code page and code generation.
- Host offsets for the prologue, each lowered guest operation, register commit,
  IP commit, flags/branch handling, instruction count and host epilogue.
- The guest instruction start for each region, including prefixes and followed
  branches. An operation can include guards, helper calls and replay exits.

No extra ARM64 instructions are emitted for these markers. Several markers
can share an offset when an operation emits no code; the last marker at an
offset describes the following nonempty region. Code bytes are retained across
arena recycling so the same host address can be interpreted differently at
different times. Diagnostic storage therefore grows with total compilation,
rather than only resident arena size. Use bounded task recordings.

The analyzer checks PID identity, event order, machine-code lengths, region
bounds and overlaps within each arena generation. It matches each sample to
the code lifetime active at that timestamp. Since Xcode exports the trace
start date to millisecond precision, it conservatively leaves samples within
one millisecond of code publication or retirement unresolved. ARM64 address
tag bits are removed; ancestor return addresses are moved back one instruction
to identify the BL/BLR site. Helper descendants are counted separately from
samples executing generated code itself.

## M1 Pro Word validation, 26 October 2026

One normal-arena headless Word launch passes the existing exact pixel check and
clean shutdown. Its 1,100 main-thread task samples include **129 generated self
samples** mapped to emitted regions and **21 helper descendant samples** mapped
to their generated callers. Eight anonymous self samples remain unresolved.

| Generated self region | Samples | Share of all task samples |
| --- | ---: | ---: |
| Memory operation, guards and replay | 59 | 5.36% |
| Prologue | 35 | 3.18% |
| Flags and branch | 14 | 1.27% |
| Instruction count | 2 | 0.18% |
| Host epilogue | 5 | 0.45% |
| ALU | 3 | 0.27% |
| IP commit | 1 | 0.09% |

The remaining mapped generated self samples are four each in Shift and GPR
commit, and one each in Move and Test. These are statistical samples from one launch, not exact instruction
costs. Memory regions include address calculations and checks. Helper costs
are additional entries in the JSON report, not included in the table's counts.
The [measurement record](measurements/word-m1-jit-debug-2026-10-26.json) contains
per-run attribution and binary/map hashes.

Local raw artifacts are in `build/word-m1-jit-debug-final/`. The native
`.trace`, sidecar and exported XML must stay together for reanalysis. This
recording includes normal address reuse; no enlarged diagnostic arena is used.

Tests cover execution with mapping enabled, explicit lifetime resets, overwrite
rejection, guest instruction starts, recycled-address attribution, malformed
code/ranges, process mismatch and ARM64 leaf/caller PC handling. Core, bounded
batch and cached-batch checks provide independent execution regression coverage.

## Implication for dispatch work

Current blocks are standalone C++-callable functions: each saves host registers,
loads guest registers, executes, commits state and returns. `runCached()` then
performs another target lookup and a broad set of context/fetch checks. Batching
reduces device polls but preserves much of this transition work. Short or
unsupported fragments also revisit dispatch and the one-instruction interpreter.

The previous recursive bridge bypassed ordinary native entry for 91.6% of calls
but slowed Office; it retained a guarded C++ resolver and nested frames. The
retained iterative shortcut gives a small Excel improvement and nearly neutral
O3 Word results. See [bridge](native-chain-bridge.md),
[controller](native-chain-loop.md) and [retained cached dispatch](cached-tail-dispatch.md).

QEMU's direct chains patch eligible same-page exits to jump to the next block,
and undo those links on invalidation. Its indirect-target path can look up a
block and jump without returning through the main loop. This is a useful
system-emulation reference for replacing repeated entry validation with explicit
link validity and exits at state changes. [QEMU translator internals](https://www.qemu.org/docs/master/devel/tcg.html)

FEX's ARM64 dispatcher emits its cache lookup and branch in generated code and
uses a static guest-register convention. Box64 documents larger translated
regions and CALL/RET paths that bypass its jump table when possible. Both are
application translators, so their address-space and device obligations differ
from our Windows 95 system emulator. Their common lesson is to reduce guest/host
transition work on frequent paths.
[FEX dispatcher source](https://github.com/FEX-Emu/FEX/blob/main/FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp),
[Box64 dynarec controls](https://github.com/ptitSeb/box64/blob/main/docs/USAGE.md)

The next architectural experiment should start with a restricted same-page
direct edge and a shared native entry/exit convention. It needs invalidation
links and exits for code writes, translation/context changes, replay, debugging
and interrupt deadlines. Its measured targets are fewer C++ transitions,
register spills/reloads and repeated guards per useful guest instruction.
The new map makes those generated entry/exit costs observable before expanding
the design.
