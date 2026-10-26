# Native transition profiling

Host profiles identify dispatch/validation as a large Excel cost. A private
counter diagnostic now identifies which native transitions create that work.
It samples native function returns with jittered 128–383-call intervals while
the existing task profiler is active. Each numeric record includes address
space, selector/base, start/returned IP, completed instruction count, memory
metadata count, physical code page and generation. Guest code is not embedded
in the committed records. Production runtime code is unchanged.

Two independently restored Excel calculation tasks and one warm Word launch
pass the expected initial/final pixels. This uses the frozen native-batch
source and a private `PCEMU_PROFILE` build. Its timings are not performance
comparisons. The existing host stack profiles remain the evidence for time
attribution; these sampled counts explain execution topology.

| Sampled native-call property | Excel run 1 | Excel run 2 | Word |
| --- | ---: | ---: | ---: |
| Samples | 1,328,435 | 1,328,667 | 26,980 |
| Return to same start IP | 0.279% | 0.277% | 3.050% |
| Register-only self-loop, 2–32 instructions | zero observed | zero observed | zero observed |
| 1–5 completed guest instructions | 71.597% | 71.611% | 81.268% |
| Blocks containing memory metadata | 73.157% | 73.142% | 78.188% |
| Top 40 start addresses | 82.949% | 82.951% | 25.104% |

Retaining edges with at least 0.5% of sampled calls yields a 33-address cycle
in Excel, with 33 edges accounting for 81.172% and 81.163% of sampled transitions.
Most individual hot edges contribute about 2.5%. Word has no comparable hot
multi-address component under that threshold. Small same-block repetition was
therefore poorly matched to this Excel workload. The next sustained-execution
experiment should target cross-block transitions, with separate Word controls.

This is a graph of sampled native returns, not proof that every edge executes
consecutively without interpretation. Returned IPs precede possible replay;
interpreted gaps and rare replay exits need explicit handling. Address graphs
also merge generations, although the underlying records retain their identity.
A safe linking design must preserve helper-data lifetime across metadata
replacement, reject invalid code/fetch mappings, deliver precise replay state,
and keep bounded instruction counts and the existing interrupt/control cadence.
No speedup or safe linking implementation is claimed by this collection.

[Exact diagnostic patch, build identity, analysis source, task results and compressed numeric records](measurements/native-transition-profile-2026-10-26.json).
A known weighted graph fixture checks accounting, histogram shares, self-loop
classification and component size. Numeric artifacts pass zstd round trips.
Both Excel runs use the same deterministic jitter seed; this is repeatability
evidence rather than a formal independent-sampling confidence estimate.
Existing external checkpoints were reused and successful children removed.
