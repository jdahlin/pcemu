# Compiled scalar port I/O coverage, 2026-10-27

All eight scalar IN/OUT opcodes (E4/E5/E6/E7/EC/ED/EE/EF) now use a compiled
exit with decoded immediate/DX port, byte/word/dword width and direction.
This targets frequent Office device accesses, especially Excel's IN/OUT byte
loops. String INS/OUTS remain interpreted.

## Execution and fault semantics

ARM64 commits the exact prefix GPRs and flags and returns a dedicated exit token
with the I/O restart IP. After the generated frame has returned, the dispatcher
executes decoded I/O through the shared C++ permission/device backend. Thus
callbacks and exception delivery cannot unwind through generated frames.
This is a compiled decoded I/O path, not direct ARM access to PC hardware ports.

The prefix is accounted before the decoded instruction increments its clock.
Callbacks see the full-width instruction count, post-fetch IP and canonical
GPR/flag state. IN preserves unwritten AX/EAX bits; absent devices return all
ones for the selected width. DX ports use only its low word. OUT reads the
selected accumulator width after permission checking, matching the reference.

The existing CPU instruction transaction is shared between interpreter and
decoded I/O: faults restore GPRs, segments, flags and the restart IP, retain
page-table effects, clear interrupt shadow state, notify the observer and
perform normal guest exception delivery. Real/protected/V86 mode, CPL/IOPL,
TSS validity/limits, every byte of wide-port bitmap checks and paging therefore
retain the same implementation. Ordinary callback exceptions propagate through
C++ with the same post-fetch state as the interpreter; fault callbacks roll back.

An I/O block is terminal and retains the interpreter's device-poll boundary.
It has a separate deferred entry in optional native state. Ordinary dispatch
can reuse its fast metadata lookup, while its null ordinary function pointer
stops cached/directly linked tails. I/O-only metadata does not enlarge the
ordinary block header. No additional check is added to the ordinary cached
hot-hit loop. The dispatcher checks the
exit token on the normal run path. Metadata is copied before callbacks, which
may safely invalidate or clear the JIT. A prefix code write or fault exits
without the token, so it cannot accidentally execute stale decoded I/O.
CPU snapshot fields and persistent guest state formats are unchanged.

## Validation

Headless RAM-only validation passes:

- 33,241 new checks cover all scalar opcodes, widths, code sizes and prefixes,
  immediate/DX ports, clocks above 32 bits, high-half preservation, canonical
  callback state and mutations, absent devices, real/protected/V86 permissions,
  TSS bitmap denial and limits, port 65535 crossing, paging and supervisor bitmap
  reads, callback faults/ordinary exceptions, exact rollback, code invalidation,
  callback cache clearing and explicit polling with links enabled/disabled in
  basic/follow/trace modes.
- A real V86 permission fault enters the guest #GP handler through the TSS
  ring-0 stack. All ten dwords of its error/restart/segment frame are checked.
- Reference core: 354,964; bounded batch: 18,555; cached batch: 20,456,574,
  including 103,944 linked transitions; flag liveness: 102,230,994.
- Nine host-sample and two Instruments-accounting tests pass. Deferred I/O
  and descendants have their own `Compiled port I/O and devices` category,
  distinct from dispatch and interpreted execution. Existing disk/poll
  attribution retains its priority.

Build directory: `build/office-port-io/optimized`. Targets: `pcemu`,
`pcemu-profile`, `jit-port-io-tests`, `tests`, `jit-batch-tests`,
`jit-cached-batch-tests`, and `jit-flags-tests`.

## Coverage

One diagnostic restore per workload/build, prior SETcc candidate versus new:

| Task | Interpreted scalar I/O | Prior | Candidate |
| --- | --- | ---: | ---: |
| Word | All observed scalar I/O | 53,098 | 2,792 |
| Word | EE OUT DX,AL | 33,339 | 1,028 |
| Word | E6 OUT immediate,AL | 7,910 | 153 |
| Word | E4 IN AL,immediate | 6,034 | 68 |
| Word | EF OUT DX,AX/EAX | 5,724 | 1,452 |
| Excel | All observed scalar I/O | 382,429 | 1,469 |
| Excel | E6 OUT immediate,AL | 135,735 | 115 |
| Excel | E4 IN AL,immediate | 132,602 | 52 |
| Excel | EE OUT DX,AL | 111,374 | 383 |
| Excel | EF OUT DX,AX/EAX | 2,693 | 894 |

Combined interpretation falls 94.7% in Word and 99.6% in Excel. Residual
attempts include warmup/admission, cold code and shadow/trace fallback. Native
instruction counters now include decoded I/O attempts and permission faults;
they do not mean the C++ device backend became generated ARM64. Instrumented
timings are not benchmark scores, and timing changes can alter guest polling.

## Original-baseline comparison

Eight rotating Word rounds and four Excel rounds each include original-A,
original-B (identical frozen 57ce863 executable), prior SETcc build and this
layout-preserving candidate. All 48 unprofiled restores pass their pixel gates
and exit cleanly; none are discarded. Median native instruction shares reach
96.339% for Word and 99.758% for Excel.

| Task | Original pooled CPU ms | Prior CPU ms | Candidate CPU ms | Below original | Below prior |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word 5M document | 1,888.036 | 1,361.271 | 1,359.663 | 27.99% | 0.12% |
| Excel dependent recalculation | 7,264.477 | 6,367.820 | 6,328.495 | 12.88% | 0.62% |

Word wins five of eight pairs, with a 0.12% paired median improvement;
absolute A/A differences have median 0.77%, maximum 2.27%. Excel wins two of
four pairs, with a 0.28% paired median improvement; A/A median is 0.45%, maximum
0.72%. Small incremental differences remain within control variation. The
verified result is much higher compiled coverage with approximately unchanged
whole-task CPU cost, not a reliable new speedup.

All workloads remain headless and reuse existing snapshots with small private
overlays removed on success. No builds, tests or profilers overlap unprofiled
timing. The separate GUI VM and macOS background work remain active. CPU time
includes all emulator threads and control/capture/video work; it excludes
restore and blocked/descheduled time, and does not eliminate contention/DVFS
effects. Word retains the unchanged 4,807,680-byte DOC and cover-title readiness
gate, not full pagination. Original-baseline gains include preceding changes
and contemporaneous control variation.

## Retained preliminary comparisons

Two earlier 48-run comparisons exposed overhead that the final structure avoids:

| Implementation | Word CPU change versus its prior controls | Excel CPU change versus its prior controls |
| --- | ---: | ---: |
| Unpublished I/O metadata | +1.69% | +0.41% |
| Fast metadata with enlarged block header | +1.39% | -0.11% |
| Optional I/O metadata, original hot header layout | -0.12% | -0.62% |

The first implementation forced ordinary I/O dispatch through the block map.
A deferred entry restored fast metadata reuse while keeping cached execution
stopped, but initially enlarged every block's hot header. The final version
moves I/O state into optional native metadata and preserves the ordinary block
header layout. All 96 preliminary runs, source/binary hashes, counters and logs
are retained alongside the final 48. These are separate contemporaneous
comparisons, not a direct randomized three-candidate ranking. No preliminary
scores are substituted for the final build's results. Binaries remain local
under `candidate`, `final` and `optimized`, respectively.

## Separate Instruments evidence

A preliminary Word trace has 1,501 task-window main-thread samples: generated
code/helpers 28.2%, dispatch/cache validation 22.7%, compilation 18.8%,
interpreter/MMU/faults 16.3%, capture/video/control 7.9%, device polling 3.3%,
other host work 2.5%, disk/compression 0.3% and compiled port I/O/devices 0.2%.
It informed the dispatch investigation; it is not evidence for final timing.

After all final unprofiled timing, a separate final Excel trace passes the same
readiness check and records 6,574 task-window main-thread samples. Dispatch/cache
validation accounts for 45.7%, generated code/helpers 37.5%, capture/video/control
6.6%, device polling 2.9%, interpreter/MMU/faults 2.7%, other host work 2.4%,
compilation 1.7% and compiled port I/O/devices 0.6%. The remaining dominant cost
is dispatch/native execution, rather than scalar I/O decoding. These are sample
shares, not isolated function CPU durations; instrumented clock times are
excluded from benchmark scores. Raw traces/maps remain in local build output.

[All runs, hashes, counters, tests and Instruments summaries](office-port-io-2026-10-27.json)
and [compressed raw profiles from all three candidates](office-port-io-2026-10-27.profiles.zst)
preserve the evidence. Executable hashes are verified for each comparison, and
compressed profiles pass byte-for-byte decompression comparison.
