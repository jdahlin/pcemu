# External x86 test tiers

`make test` remains offline. It runs the existing native regressions, parser and
negative-oracle checks, and a pinned sample of 61 Intel 386EX hardware vectors
through interpreter and JIT (122 executions). No DOS image or QEMU is needed
for that sample. The JIT's native-instruction counter distinguishes native work
from interpreter fallback; not every tested instruction has a native lowering.

The broader suites have separate targets:

| Command | Scope |
|---|---|
| `make test-x86-hardware` | All cases in 28 selected opcode/size shards: about 54,500 hardware vectors, both execution backends. Downloads only those pinned shards. |
| `make test-x86-hardware-full` | All 941 available opcode/size shards. About 576 MiB of compressed input on first use; explicitly unsupported adapter cases are reported. |
| `make test-x86-rom` | Standalone test386.asm BIOS replacement, interpreter/JIT and independent QEMU 486 TCG reference. |
| `make test-freedos-extended` | DOSEMU2-derived DOS HMA availability/allocation checks on the pinned FreeDOS fixture, interpreter/JIT. |
| `make test-freedos` | Existing FreeDOS CPU and driver acceptance matrix. |
| `make test-freedos-vga` | Existing VGATEST/Abrash/DOSLIB/X-VESA matrix on FreeDOS. |

CPU-heavy shards run in a bounded pool, default four workers. Each worker owns a
separate native subprocess; compilation finishes before workers start, so they
never launch competing Ninja builds. Suites are not all dependencies of
`make test`. Choose the particular target needed, and adjust parallelism:

```sh
make test-x86-hardware X86_TEST_JOBS=2
python3 tools/test_x86_hardware.py --opcodes 6701 --index 5 --backend interpreter --jobs 1
python3 tools/test_x86_hardware.py --opcodes 00,01,6601 --limit 100 --jobs 2
```

Workers operate on RAM-only CPU instances, not VM disks. FreeDOS checks use small
private compressed overlays and remove them after success. Runs retain JSON
reports and per-shard logs under `build/x86-hardware-*`, `build/x86-test386-*` or
`build/freedos-hma-*`. Failures return nonzero; they are not converted to passes
because the underlying feature is unfinished.

## Hardware oracle

[SingleStepTests/80386](https://github.com/SingleStepTests/80386) is pinned to
`459d49fbe6280e9ed46fee887b58dacd9cb880ab`. The vendored MIT-licensed sample and
full-file Git identities are in `tests/vendor/80386/`. Cached downloads are
verified before use. Each comparison checks all supplied general, segment,
control and debug register values, defined flags, exception identity and the
supplied RAM bytes, including unchanged bytes supplied in the initial state.

MOO captures include a terminating hardware HLT, sometimes injected at a branch
or exception destination. The adapter executes the instruction under test and
accounts for that terminator's IP advance; it does not execute arbitrary bytes
at an injected destination. The upstream per-opcode flag masks govern undefined
flags, including their saved exception-stack image. The reader rejects malformed
lengths/counts/register payloads and unknown CPU versions. Negative tests prove
wrong registers, flags, exception identities and RAM expectations are rejected.

The pinned revocation list is honored. The upstream-documented scaled-no-index
SIB quirk is excluded by encoding and counted separately, since it is undefined
386 behavior. Register/memory mismatches and defined exceptions are not excluded
because PCEmu fails them. Hardware I/O bus inputs are not currently modeled;
those shards are explicitly unsupported and cannot produce an all-passed report.
Bus-cycle timing and writes to RAM bytes absent from the input/expected lists
are outside this adapter's oracle. The available upstream corpus is real-mode;
protected-mode and V86 suites are not supplied by this revision.

## Standalone ROM

[barotto/test386.asm](https://github.com/barotto/test386.asm) is pinned to
`cfd052d1e64d5375dea5a681c1eadeed64ceda2c`, archive SHA-256
`6da205bb4364adf0b357b73a02c045b0b5b58e4a606530b74c9f1672164749db`.
Source and GPL license stay in the ignored cache/work directory. NASM builds a
64 KiB ROM; only its documented OUT_PORT configuration changes, to E9h. The
runner has no host BIOS callbacks. Success requires a halt after POST FF; if
QEMU completes too, arithmetic transcripts must also match. Instruction and
wall-clock budgets prevent a failed guest from hanging validation indefinitely.

The current pinned ROM stops at POST 20 (ring-3 checks) in both PCEmu backends
and QEMU 486 TCG. Consequently the reference comparison is **inconclusive**;
`test-x86-rom` returns nonzero. This shared failure must be understood before
using this build/configuration as a full protected-mode conformance oracle.
We do not patch its checks or claim PCEmu-specific bugs from that shared result.

## DOS regression ports

Two [DOSEMU2 HMA tests](https://github.com/dosemu2/dosemu2/blob/7e7f3da9596ce8035274843f61dc6c71e9733d3a/test/func_memory_hma.py)
are ported from C int86x calls to equivalent NASM INT 2Fh calls. The GPL license,
revision, source digest and adaptation notes are in `tests/vendor/dosemu2/`.
They require HMA availability, allocate 35 bytes, and verify the returned segment,
size, subsequent allocation offset and remaining free space.

There is an important DOS variant: FreeDOS kernel 2043's public INT 2Fh/4A02h
allocates and reports exactly 35 bytes, while the original DOSEMU/MS-DOS test
expects paragraph rounding to 48. The FreeDOS port checks its actual 35-byte
contract and the matching offset/free-space changes, as implemented in
`kernel/inthndlr.c` inside the fixture's pinned kernel source package. This is
explicitly a port with an OS-specific expectation, not an unchanged upstream
binary or evidence of MS-DOS HMA rounding compatibility.

DOSEMU2's wider DPMI and FPU tests, v86's assembly/paging tests, QEMU TCG tests and
Berkeley TestFloat remain integration candidates. The current setup does not
claim to run those suites.

## Findings

The new inputs exposed and fixed three instruction issues: invalid MOV into CS
now raises #UD before operand access; invalid LOCK forms raise #UD; DIV/IDIV
zero-divisor/overflow and AAM base zero now deliver restartable #DE instead of
host errors. Each has offline regressions in `x86-task-x87-tests`.

The wider hardware target currently returns nonzero for remaining architectural
mismatches, notably real-mode segment/access boundaries and instruction fetches
crossing FFFFh. They remain visible in the extended logs and are not silently
filtered into the passing smoke sample. Recorded scopes, counts, failures and
reference status are in [the validation record](measurements/x86-external-2026-10-27.json).

The first follow-up enforces complete real/V86 memory operand and stack bounds
in the interpreter and native memory paths. The selected 28-shard run now has
10 failing executions (previously 1,854); all address-size shards pass. The
remaining selected cases involve instruction fetch wrapping and an interrupt
vector aliased by the exception stack. Focused segment regressions include
pre-access device isolation, #SS versus #GP, the last valid word, and LEA.

Architectural rules and per-fix validation are tracked in
[the real-mode specification notes](x86-real-mode.md).
