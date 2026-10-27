# Native LAR/LSL descriptor queries

The short Excel load's previous five diagnostic profiles contained 56,511 LAR
and 13,741 LSL interpreter attempts. Memory selectors accounted for 33,349 LAR
attempts, so this change covers both register and memory operands rather than
only the easier register case.

Decoded queries now call a guarded C++ helper from compiled blocks. The helper
reads a 16-bit selector, checks GDT/LDT bounds, type and privilege visibility,
then computes the access rights or expanded segment limit. Operand size selects
the 16/32-bit destination. Invalid queries clear ZF and preserve the destination;
successful queries set ZF. Other flags retain the reference behavior. Cached
code reads live table bases, limits and descriptor bytes rather than caching
unversioned descriptor results. Singleton and pair fragments use normal warmup.

Memory selectors obey caller permissions. Descriptor reads use supervisor
permissions, including for ring-3 callers. Every read must already translate to
safe RAM; otherwise the helper returns before architectural mutation and the
dispatcher replays the instruction. This preserves cold page walks, precise
faults, unusable source-segment faults, and MMIO callback order/state. Both
descriptor words are probed before committing results. Prior lazy flags and
registers are canonical before helper entry; the destination is reloaded after
success so following compiled instructions see the new value. Debug maps name
these regions DescriptorQuery.

## Reference correction and independent validation

Intel's [SDM Volume 2A, LAR/LSL type tables](https://cdrdv2-public.intel.com/868140/253666-089-sdm-vol-2a.pdf#page=644)
marks interrupt and trap gate types 6, 7, E and F invalid for LAR. The old
interpreter accepted those four system types. Its LAR predicate is corrected;
LSL's type predicate was already correct. The test's independent valid-type
bitmaps and privilege rules prevent merely copying that defect into native code.

`jit-descriptor-query-tests` passes **114,774 checks** in Release. These cover
all descriptor types and application/system, present, granularity, CPL, RPL and
DPL combinations against independent expected values, followed by native versus
interpreter comparison. Further checks cover every GPR source/destination alias,
code/operand/address sizes, GDT/LDT and null/out-of-bounds selectors, memory
address aliases, cold/warm paging, ring-3 supervisor descriptor reads, partial
descriptor page faults, MMIO callback state, real/VM86 #UD priority, normal
warmup singleton/pair admission, cached execution with links enabled/disabled,
and descriptor/table changes between cached calls. Comparisons include GPRs,
flags, IP, instruction count, CR2, system-access nesting and RAM/page effects.

Regression suites pass: core CPU 354,964 checks; flags 102,230,994; batch 18,525;
cached batch 20,456,574; segment operations 23,162; guarded reads 255,535;
profile timing 33. Full retained log counts are in the JSON report.

## Office timing

Four rotated-order, single-run rounds compare original `57ce863`, prior
`95aaa5f` and candidate on each workload: **36 independent headless restores**,
all verified by their existing pixel gates. The Library VM is paused during
the scored cohort and resumed in `finally`. Tests, builds and diagnostics do
not overlap scored timing. Existing snapshots and small temporary compressed
disk children are reused; successful benchmark children are removed.

Median milliseconds below retain elapsed and process CPU separately. The
original-relative improvements include preceding optimization work.

| Workload | Original elapsed / CPU | Prior elapsed / CPU | Candidate elapsed / CPU | Below original elapsed |
| --- | ---: | ---: | ---: | ---: |
| Word launch + 5 MB DOC cover | 2,266 / 2,186 | 1,643 / 1,581 | 1,594 / 1,539 | 29.7% |
| Excel launch + 650-formula XLS | 1,679 / 1,626 | 1,310 / 1,268 | 1,272 / 1,231 | 24.2% |
| Excel D1 edit + 4,096-row recalculation | 7,952 / 7,899 | 6,810 / 6,786 | 7,003 / 6,972 | 11.9% |

The prior-relative elapsed changes are -3.0%, -2.9% and +2.8% respectively.
These are provisional observations, not an established incremental speedup:
there are four samples per engine/workload, host and guest work vary, and this
cohort has no new identical-binary A/A control. The recalculation slowdown is
retained explicitly rather than hidden in an aggregate Office score. The
previous unit's A/A experiment demonstrated substantial short-load variation.

The load timers include application launch and file load to the rendered gate;
Word's gate is the cover title, not complete background pagination. The Excel
load gate verifies its ten calculated summary values. Recalculation starts with
an already-open workbook. VM restore is excluded. Process CPU includes emulator
threads and control/video; elapsed time additionally includes polling and
scheduling. The frozen engine and task hashes and every scored sample are in
the adjacent JSON report.

## Coverage and Instruments

Independent diagnostic restores compare run-0 counters with the preceding
bit-scan build. These timings are excluded from the score.

| Workload | Prior LAR + LSL attempts | Candidate attempts | Candidate warming / replay / declined |
| --- | ---: | ---: | ---: |
| Word load | 12,478 | 870 | 784 / 86 / 0 |
| Excel load | 13,516 | 893 | 767 / 126 / 0 |
| Excel recalculation | 11,588 | 2,392 | 383 / 2,009 / 0 |

This establishes a 93.0%, 93.4% and 79.4% reduction in interpreted query attempts.
Replay guards remain important, particularly in recalculation. Guest instruction
totals vary between restores despite the immutable starting checkpoint and the
same rendered result, so these are observed intervals rather than identical
fixed-instruction kernels. Raw profiles are retained in the adjacent zstd JSON
bundle.

A separate Excel-load Instruments run with timestamped JIT maps passed the same
gate. Of 1,177 task-window main-thread samples, generated code/helpers account
for 24.4%, compilation 24.0%, dispatch/cache/validation 22.3%, interpreter/MMU/
faults 15.7%, and capture/video/control 8.2%. Only one sample resolves to the
descriptor-query helper, insufficient to estimate its cost precisely.
Compilation, dispatch and warming therefore remain material. ARPL, privileged
0F groups, and guarded-memory replay still leave the broader Word/Excel native
coverage goal unfinished.

Local reproduction uses `build/office-descriptor-query/measure.py` and the
existing three task JSONs with `tools/bench_snapshot_task.py --repeat 1`.
Use `--guest-profile` only with the profile binary; record Instruments separately
with `--instruments --jit-debug-map`. Raw traces and generated bytes stay under
ignored `build/office-descriptor-query`.
