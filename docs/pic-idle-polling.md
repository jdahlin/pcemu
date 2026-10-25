# Empty-request PIC polling

The host profile after fetch-TLB validation attributed 2.39% of sampled self
stacks to `I8259PairDevice::nextVector`; board polling accounted for 14.15%
inclusively. These are diagnostic sample shares, not isolated timing bounds.

`nextVector` now handles zero IRR on both chips inline. It clears the cascade
line and refreshes every compatibility mirror before returning no vector.
Any requested IRQ, including masked requests or a CPU that cannot yet accept
interrupts, still enters the existing priority/cascade/acknowledge algorithm.
Polling frequency, PIT timing and snapshot state layout are unchanged. Inlining
also permits the compiler to defer CPU-readiness computation for the empty path;
the observed whole-process result cannot be attributed solely to a removed call.

## Differential validation

`tests/i8259_pair_poll_test.cpp` compares against the frozen previous algorithm:
all 65,536 ISR pairs, both CPU-ready and stale-cascade states, plus 100,000
seeded randomized pending, masked, priority and EOI configurations. It compares
returned vectors, chip fields, configuration mirrors and compatibility flags.
The standalone test passes 2,172,864 checks. Run it without the full emulator:

```sh
mkdir -p build/pic-idle
clang++ -std=c++20 -O3 -Wall -Wextra -Wpedantic -Isrc \
  src/pic.cpp src/i8259_pair.cpp tests/i8259_pair_poll_test.cpp \
  -o build/pic-idle/poll-tests
build/pic-idle/poll-tests
```

The frozen candidate also passes 354,948 core checks and headless DOS 6.22,
Win3.11 Notepad/save/DOS readback, rtts Win95 and installed OSR2
Calculator/clipboard/Notepad/restart checks. Successful private compressed disks
are removed; original disks remain unchanged. Fresh Setup was validated on the
immediately preceding fetch-TLB binary, not repeated for this PIC-only change.

## Exploratory application measurement

[Six raw measurements](measurements/pic-idle-excel-2026-10-25.json) record three
alternating baseline/candidate pairs on the same restored Excel 97 task: change
D1 in a 4,096-row dependent calculation and observe the expected sum. The frozen
source trees differ only in `i8259_pair.hpp` and `i8259_pair.cpp`.

| Pair | Baseline CPU ms | Candidate CPU ms | CPU reduction |
| --- | ---: | ---: | ---: |
| 1 | 9265.84 | 8040.54 | 13.22% |
| 2 | 8248.76 | 7821.65 | 5.18% |
| 3 | 8381.07 | 8260.80 | 1.44% |

Median paired baseline/candidate CPU ratio is 1.0546, about 5.18% less CPU.
Median response ratio is 1.0604 and execution-wall ratio 1.0508. Observed guest
instruction counts differ by less than 0.023% within each pair; the final pair
has identical counts. Native coverage remains approximately 97.69%.

Process CPU includes all emulator threads, control, capture and guest idle work;
restore and blocked/descheduled time are excluded. The first baseline restore
was cold (33.6 seconds versus under 0.85 seconds thereafter); excluding restore
does not eliminate subsequent cache effects. Host load and the wide pair spread
limit confidence in the exact gain. This is evidence for retaining a small
behavior-preserving optimization, not a general application speedup or a claim
of Rosetta-level performance.
