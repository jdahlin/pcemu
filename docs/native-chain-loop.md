# Iterative native controller trial

The private cross-block bridge now uses one C++ controller loop rather than
recursing through a resolver for every target. It clears the transient chain
budget before calling a child, so the child returns directly to the controller.
The same context/fetch/code guards remain, including generation revalidation
after a fetch walk. CPU instructions commit after native prefixes return, and
logical dispatch counts preserve the existing batch/control calendar.

All checks pass: 77,961 cross-block checks (4,256 linked batches), 1,084 focused
memory/replay checks, 3,500 batch/timer checks, 354,948 core checks and 759 context
mutation checks. The API calendar and modeled interrupt points match ordinary
single-block execution. CPU size and existing field offsets remain those of the
recursive prototype; its fields use existing padding.

Six alternating, uninstrumented headless pairs all favor this controller for
loaded Excel recalculation. Median baseline/candidate process CPU ratio is
1.0583, equivalent to about 5.5% less process CPU time; response ratio is 1.0591.
This compares with the frozen native-batch baseline, not a cumulative speedup.
All expected task pixels pass. Word has a median CPU ratio of 0.9892 and response
ratio 1.0005: response is neutral, but CPU is slightly worse.

Three paired CPU-only pointer-chase runs per dispatch API expose tradeoffs:

| Working set | Batched flat ratio | Batched paged ratio |
| --- | ---: | ---: |
| 4 KiB | 1.143 | 1.104 |
| 32 KiB | 1.159 | 1.094 |
| 512 KiB | 1.057 | 1.034 |
| 8 MiB | 0.960 | 0.975 |

Single-block control ratios span 0.982–1.027. Shared-host variation remains, and
these samples do not establish formal confidence intervals. Exact counts and
ring checks pass every case. No guest disk is needed for these RAM controls.

This is a promising **private candidate**, not a default change. The generated
native exit still contains extra budget checking and controller-call code even
for ordinary single-block execution. Moving controller entry outside generated
blocks could retain the fast cached-target loop while removing that overhead
and code-arena growth. That is the next experiment, not a demonstrated gain.
The recursive prototype's linked-call diagnostic must not be quoted as a newly
measured adoption count for this version.

[Exact source patches, build commands, test outputs, all six Office pairs and RAM controls](measurements/native-chain-loop-2026-10-26.json).
Existing external Office checkpoints are reused and successful small children
removed. No fresh installer acceptance claim is made for this private binary.
The production runtime remains unchanged and another twofold useful-work gain
remains unproven.
