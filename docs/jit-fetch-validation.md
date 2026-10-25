# Cached instruction-fetch validation

Paged native blocks still validate their current physical instruction page on
every dispatch. The dispatcher now resolves a current, permitted TLB read hit
inline instead of calling `CPU::address` followed by `CPU::physical` for that hit.
It checks the current CS usability, CR3 identity, TLB tag and epoch, read
permissions, CPL/system access, and A20 masking. It does not retain an additional
translation cache or skip the existing native-block identity/generation guards.

Any miss, restricted user access or segment fault uses the existing reference
CPU path. Page-table walks, accessed-bit updates, CR2/error-code construction and
interpreter fault delivery remain there. The interpreter's translation function
remains out of line; this targets the frequently repeated JIT entry check.

The added 192-case matrix compares native dispatch with reference execution for
supervisor, user, VM86 and system accesses; all four read permission combinations;
TLB hits, tag/epoch misses, CR3 switches, remapping/flushes and unusable CS; and
A20 on/off. It verifies the selected code, segment-before-page fault priority,
restart IP, CR2, registers, flags and instruction counts. The core suite passes
354,948 checks, including 2,304 added checks, and stack/JIT passes 33,360.

The frozen candidate boots MS-DOS 6.22 and passes Win3.11 Notepad persistence,
rtts Win95 applications/restart, and the recovered OSR2 installation's same
application/restart and exact file readback. Successful private disks are removed.
The OSR2 regression check does not rerun Setup under this candidate.

Twelve independent Excel recalculation restores produce the expected result
pixels. Across six alternating pairs in two batches, the median baseline/candidate
ratio is 1.097 for response and 1.123 for execution wall time. Five response pairs
and all six execution-wall pairs favor the candidate. Native coverage stays at
97.695%. Other applications, an ARM guest and compilation were active; times
drift substantially, so these are shared-host observations rather than a reliable
general application speedup. Restore and instrumented runs are excluded from the
paired task scores.

A separate uninstrumented host sample retains 12,375 task samples after excluding
the attachment barrier. Dispatch/cache/validation accounts for 21.4%, compared
with 28.5% in the preceding diagnostic. Self-sample shares in `CPU::address` and
`CPU::physical` are 2.3% and 1.9%, compared with 5.5% and 6.4%. Different host
load and capture overhead make these diagnostic attribution, not a controlled
absolute-time comparison. The instrumented guest profile records 32,527,178
interpreter attempts; instruction coverage is essentially unchanged as expected.

[Commands, hashes, paired results, integration evidence and compressed profiles](measurements/jit-fetch-tlb-2026-10-25.json).
