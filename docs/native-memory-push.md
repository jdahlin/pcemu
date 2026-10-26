# Native FF /6 PUSH lowering

The loaded Excel profile contains 1,332,398 interpreted `FF /6` attempts.
This change accepts register and memory forms using existing guarded stack
writes. It leaves JIT dispatch and compilation thresholds unchanged.

For memory sources, generated code computes the source offset using cached
pre-decrement registers and calls the guarded RAM-only read helper. It saves
the operand-sized value before attempting the stack write. SP/ESP commits only
after that write succeeds. Register forms reuse the existing PUSH-register path.
This follows the old-ESP operand/address rule in Intel's
[PUSH reference, Volume 2B, pp. 4-212–4-215](https://www.intel.com/content/dam/support/us/en/documents/processors/pentium4/sb/25366721.pdf).

The source read intentionally still uses a C++ native-memory helper; this is
instruction lowering, not a helper-free implementation. Declined MMIO,
cross-page, watched and faulting accesses replay with precise pre-instruction
state. A later stack failure may reread ordinary RAM, but device callbacks occur
only during replay. A code-writing successful push advances IP and commits its
stack value once. Source metadata is allocated before generated pointers are
embedded, including multiple memory PUSH operations in a single block.

## Validation

The new memory suite passes 8,682 checks, including full RAM/page-table state,
flags, GPRs, IP and counts against the interpreter. It covers operand/address/
stack width combinations, paging, FS/GS, SIB/no-base forms, changed source values,
old-ESP and source/stack aliasing, MMIO callback counts, page-fault priority,
CR2, unusable SS, cross-page faults, watched writes, self-modifying stack writes
and several consecutive PUSH operations. The stack suite expands to 43,600
checks with FF /6 register aliases; 354,948 core and 8,677 existing memory CALL
checks also pass. Current-core CMake/CTest builds verify the new and stack suites.

```sh
cmake --build build --target jit-memory-push-tests jit-stack-tests tests
ctest --test-dir build -R '^(jit-memory-push-tests|jit-stack-tests|tests)$' --output-on-failure
```

Headless Excel recalculation and Word warm launch pass their expected rendered
pixels using existing checkpoints. The separate instrumented Excel run reduces
interpreted FF /6 attempts to 60,228, around 95.5% fewer. Total interpreter
attempts fall from 22.53M to 20.22M. These diagnostic runs are not lockstep traces
or timing comparisons, and smaller dispatch reductions do not imply a matching
application speedup. No application timing gain is claimed from this collection.

A RAM-only PUSH/POP/checksum loop executes 50 million guest instructions per
case across eight code-width, stack-width and paging combinations. Three
alternating baseline/candidate pairs give case-median elapsed speed ratios of
2.63–2.85. Every checksum, stack pointer and count is checked; a separate
interpreter cross-check also passes. This deliberately PUSH-heavy kernel is not
an Office or general Win95 performance result. No other timed run or compiler
was started by this task during the kernel pairs; the desktop host remains shared.

The frozen matched-source executable passes headless DOS 6.22 boot, Win3.11
Notepad/save/DOS readback, rtts Win95 and installed OSR2 Calculator/clipboard/
Notepad/restart with exact stopped-guest 38-byte file readback. All owned
compressed smoke disks and orphan indexes are removed. The Win3.11 child grew
to only 4,768 bytes; no new large VM or Office fixture was made.

[Build commands, exact source identity, kernel source and pairs, raw diagnostic
profiles and OS verification](measurements/native-memory-push-2026-10-26.json).
Only IR, decoder and native emitter differ from the frozen memory-CALL reference;
OS results identify this executable by hash rather than attributing concurrent
frontend changes to it.
Fresh OSR2 Setup now also passes from clean DOS through all three resets,
application verification, normal exit status 0 and exact 38-byte readback in
one process. See [fresh installation evidence](fresh-win95-validation.md#uninterrupted-run-with-native-ff-6-push-26-october).
The twofold useful-work target remains unachieved.
