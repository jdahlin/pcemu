# Proposed Win95 performance targets

These are proposed targets discussed on 25 October 2026, not achieved hardware
ratings or a replacement for the existing DOS/Win3.11/Win95 correctness goal.
Rosetta parity remains unverified and is a poor primary acceptance criterion
for our complete machine emulator. We emulate guest paging, devices, interrupts
and a guest OS as well as application instructions.

A useful first historical reference is a responsive Pentium 133–200-class PC;
a later stretch reference is a Pentium II 300. Establish those comparisons with
identical guest programs and data on a measured reference, not an assumed
conversion from guest instructions/s to MHz. Integer, memory, x87, storage and
graphics results need separate reporting. Our current results do not establish
those historical performance classes.

For modern comparisons use QEMU TCG system emulation on the same ARM host,
with matched guest CPU features, RAM, input data and disk/cache conditions.
Report whole-task wall time and host process CPU separately, plus correctness,
translation cost, native coverage and code-cache occupancy. A reasonable
engineering milestone is within 2× QEMU's task duration across the selected
suite, followed by parity; neither milestone is currently established. Avoid
claiming success from a geometric mean that hides a badly regressing workload.

The user experience gate should include responsive menus, typing and window
movement, stable Office/Netscape use, low idle host CPU, and smooth presentation
and audio where supported. Installer completion and boot remain compatibility
and startup gates. Loaded application computation, memory-working-set kernels,
and actual file/network operations test useful work after boot. Compare on the
same guest state; capture/render cost belongs in interactive measurements but
should be reported separately when measuring guest CPU performance.

QEMU describes itself as translating code when first encountered and documents
block chaining, self-modifying code invalidation and MMU handling. This makes
it a relevant full-system translator reference:
[translator internals](https://www.qemu.org/docs/master/devel/tcg.html).
Its instruction counting explicitly does not claim cycle-accurate CPU timing:
[TCG instruction counting](https://www.qemu.org/docs/master/devel/tcg-icount.html).

## Compilation threshold

The current executable constructs `Jit` with a warmup of eight block visits.
Metadata is created before full native translation; supported blocks compile
at the threshold. Some small fragments are deliberately retained in the
interpreter, and unsupported instructions still fall back. The threshold is
visits to a cached block, not executions of each source-level function.

A simple baseline compiler can justify a low threshold because compilation is
cheap relative to repeated interpretation. A future expensive optimization tier
could wait for many more visits. Do not copy 100/1000-visit thresholds from a
language runtime without measuring our compiler and workloads. Approximate
break-even is compilation cost divided by the time saved per future block
execution; code lifetime, invalidation and arena churn also matter. The recent
Excel host profile attributes only about 0.8% of task samples to compilation,
so it does not support raising the threshold as the next major performance fix.
Those diagnostic shares are not an experiment comparing threshold settings.

## Rejected cache-store experiment

The dispatcher currently republishes its fast lookup slot after validation.
A candidate checked whether its key/pointer had changed before storing, retaining
all existing guards and rechecking after compilation could clear the table.
Three alternating RAM-only CALL/RET pairs (eight code/stack/paging modes) did
not support keeping it: six case medians regressed. The intervals were short
and the desktop host was shared; this is rejection of an unsupported change,
not a precise causal slowdown claim. No application improvement was established.
The runtime source was restored byte-identically. The candidate patch, runner
source, commands, hashes and raw times remain in
[the experiment record](measurements/jit-cache-publish-rejected-2026-10-25.json).


## Twofold useful-work stretch target

The user considers current Win95 speed usable and identifies another 2× as a
worthwhile improvement. Treat that as a stretch target on real loaded Win95
workloads, alongside the existing installer and OS correctness gates. Do not
claim it from native coverage, a deliberately CALL-heavy loop, or rendering
throughput alone. The earlier historical-PC references remain proposals rather
than measured ratings.

Freeze the memory-indirect CALL candidate as the current reproducible reference:

- Source change: `1c5a879`, using the frozen x86-only source set in the measurement record.
- Executable SHA-256: `f6cd1657efe65efa799838685d60f4c5526756d29c2335722c9372542a5821fc`.
- Existing loaded Excel task: 4,096 dependent rows; change D1 from 3 to 4 and
  verify SUM changes from 25,200,640 to 33,591,296. Task SHA-256:
  `7c55d8acf4ada11814ac488b48014a01437e7ba970372d4476a80a947a159d47`.

[Complete baseline results](measurements/native-memory-call-2026-10-25.json)
include every result from both batches. Their large host-dependent spread means
we must rerun this reference alongside future candidates rather than compare
a future favorable sample against the old slowest sample. Report process CPU,
wall response, correctness, native instruction counts, host contention and
render/capture scope separately. Add other useful-work tasks and a memory kernel
to prevent optimizing only this calculation. The twofold target is not achieved
by the measurements currently recorded.
