# Cached validation and block-transition overhead

This change reduces host work between compiled blocks. It assigns an identity
to each exact set of common compilation assumptions, reuses the fully checked
root's identity during a cached batch, hoists stable lookup fields, and inlines
the warm instruction-translation probe. Generated blocks retain their existing
entry/exit ABI. Background compilation remains deferred.

## Validation design

`run()` retains its complete root validation. At compilation, `contextFor`
interns the block's execution epoch, RAM address/size, code-bitmap address,
CR0/CR3, CS base, A20 state, VM state and write-watch presence. An exact-key
comparison resolves hash collisions; identities are not hashes. A cached
successor with the same identity has the same common assumptions as the root
that just passed full validation. Identity assignment occurs during compilation,
with no runtime snapshot of those fields on each batch or single-block call.

Other protections remain explicit:

- The fast key still matches CS selector, code default and IP. Stable key-prefix
  and hash terms are computed once per cached tail.
- A stack-using target still checks SS.B, because a register-only root cannot
  validate that target-specific assumption.
- Linked targets still check the generation-array address embedded in a stub.
- Every target checks its code generation and bounds. Generation is checked
  again after instruction translation, which can update page-table A/D bits
  overlapping code.
- Every paged edge checks the live TLB tag, epoch, CR3 and fetch permission, with
  the existing slow translation/fault path on a miss. No instruction-translation
  cache bypasses mapping or permission changes.
- TF, interrupt shadow, replay, poll boundaries and deferred callbacks retain
  their existing behavior.

The shared warm-translation helper is inlined into the cached path. CS base,
CR3, paging/privilege state and A20 mask are stable during a callback-free
cached tail; live TLB tag/epoch/permissions are still examined on each edge.
Exceptions restore CR2 before returning to the reference path as before.

Cacheable native bodies cannot change this execution context. Operations that
can change it, deliver callbacks, or require a device poll must end the batch
through their existing boundary/shadow mechanism. This is the contract future
native operations must preserve. IDs survive arena recycling alongside their
immutable keys; `clear()` drops blocks and identities together. Exhausted ID
space disables the shortcut for new contexts through reserved identity zero.

## Correctness

The new `jit-cached-context-tests` target passes **12,364 checks**. Small RAM-only
programs exercise multiple cached transitions in basic/follow/trace modes with
links on/off and paging on/off. Context mutations include host execution
identity, RAM replacement/resize, code-bitmap and generation-array replacement,
CS base/selector/default, CR0/CR3, A20, VM state, write-watch presence, SS.B,
TLB flushes, code writes and page remapping. Complete GPRs, flags, IP, instruction
counts, CR2 and RAM/page-table effects are compared with the interpreter.

Targeted cases prevent weak guard tests: snapshot-like code changes retain old
generation values while changing execution identity; an unchanged root leads
to a stale compiled successor; old RAM and code-bitmap allocations remain live
and poisoned; shrinking RAM retains its address; and a register-only root
reaches a stack-using successor after SS.B changes. These catch stale code,
embedded RAM/buffer assumptions and conditional target guards.

Release regression suites pass: cached batches **20,456,574** checks, including
103,944 linked transitions; native batching/poll latency **18,525**; core CPU
**354,964**; guarded reads **255,535**; cross-page fetch **4,760**; compiled #UD
**11,212**; profile accounting **33**. Logs are retained in the JSON report.

## Controlled comparison

Final source was isolated from concurrent CPU/VGA work in a worktree based on
prior `653dbf7`. The final frozen candidate changes only this dispatch work.
Original baseline `57ce863`, two labels for the identical prior binary, and
candidate were rotated across four single-run rounds per workload: **48 verified
headless restores**. The prior column below pools the eight identical-binary
control samples per workload. Every sample and executable/task hash is retained.

The Library VM was paused for scored cohorts and resumed in `finally`. Our
builds, tests and profiles ran outside scored intervals. External build/test
workers were observed around three Word control samples; those samples remain
in the main table. The report also supplies the worker-free sensitivity and a
separate **24-run loading holdout**, whose endpoint checks saw no workers.
Host quiescence between checks is not guaranteed.

Median milliseconds, with elapsed / process CPU kept distinct:

| Workload | Original | Prior combined | Candidate | Candidate below original elapsed |
| --- | ---: | ---: | ---: | ---: |
| Word launch + 5 MB DOC cover | 2,180 / 2,107 | 1,615 / 1,541 | 1,532 / 1,478 | 29.7% |
| Excel launch + 650-formula XLS | 1,668 / 1,630 | 1,268 / 1,231 | 1,250 / 1,212 | 25.1% |
| Excel edit + 4,096-row recalculation | 7,684 / 7,668 | 6,668 / 6,643 | 5,670 / 5,649 | 26.2% |

The recalculation gain is **15.0% elapsed / 15.0% CPU versus prior**, well above
the identical-control spread. Its four candidate elapsed samples span
5,575–5,697 ms; all eight prior samples span 6,616–6,809 ms. The final loading
cohort suggests smaller gains, but the holdout does not establish consistent
loading improvement:

| Loading holdout | Prior combined elapsed / CPU | Candidate elapsed / CPU |
| --- | ---: | ---: |
| Word | 1,600 / 1,545 | 1,600 / 1,536 |
| Excel | 1,313 / 1,272 | 1,350 / 1,299 |

These mixed loading results remain explicit. No reliable Word/Excel loading
speedup is claimed. Original-relative improvements are cumulative across earlier
work. The goal's existing gates and scopes remain: Word includes application
launch and opening the 4,807,680-byte DOC until its cover appears; Excel loading
includes launch and all ten calculated summary values; recalculation edits D1
in an already-open workbook. Restore and Windows boot are excluded. Process CPU
includes emulator threads and control/video; elapsed additionally includes
polling and scheduling. The Word gate does not cover complete background pagination.

## Block-linking decision and prototypes

An eight-run final-code loading check compared direct linking off/on. Word
elapsed medians were 1,738 / 1,820 ms; Excel 1,395 / 1,471 ms. All gates passed,
but neither workload benefited. Direct linking remains disabled in the selected
configuration. Its generated-stub validation and entry/exit handoff remain a
separate opportunity; enabling the existing mode is not justified here.

Earlier runtime-stamp and lazy-runtime-snapshot prototypes also passed all
gates, but added startup bookkeeping. Their two 48-run cohorts and a 16-run
early direct-link comparison are retained separately, excluded from the final
table. The final implementation moves identity work to compilation and shares
the warm translation logic. Altogether the report retains **80 final-code scored
runs** and **112 superseded-prototype runs**, plus separate diagnostics.

## Counter and Instruments evidence

Separate profile restores passed the same gates. `cachedContextValidation`
counts eligible full root validations, `cachedContextChange` counts newly
interned classes during the profile window, and `cachedContextHit/Miss` counts
target identity tests before the remaining fetch checks.

| Workload | Native calls | Full root validations | Compact context hits | New classes / misses |
| --- | ---: | ---: | ---: | ---: |
| Word load | 19,842,205 | 3,092,151 | 15,990,470 | 57 / 0 |
| Excel load | 11,053,745 | 2,315,807 | 8,013,077 | 56 / 0 |
| Excel recalculation | 342,576,283 | 24,027,769 | 316,355,530 | 45 / 20 |

Raw profile JSON is retained in the adjacent zstd bundle. Diagnostic timings
are excluded from the scored comparison. Three separate Instruments traces
with timestamped JIT maps also passed:

| Workload | Main-thread task samples | Dispatch/cache | Generated/helpers | Compilation | Interpreter |
| --- | ---: | ---: | ---: | ---: | ---: |
| Word load | 1,690 | 21.4% | 30.9% | 18.3% | 13.4% |
| Excel load | 1,380 | 20.8% | 24.2% | 22.5% | 16.7% |
| Excel recalculation | 6,131 | 35.9% | 44.3% | 2.1% | 1.5% |

The preceding same-prior recalculation trace had 6,793 samples and 45.1% in
dispatch/cache validation. Its cached-loop self samples were 30.2% and separate
instruction-page validation 9.1%; the new cached-loop self share is 27.9% and
now includes the inlined warm fetch probe. These are diagnostic sample shares,
not exact function durations. Compilation and warming still limit short loads;
native entry/exit and block transitions still have substantial cost.

Frozen binaries, scripts and raw traces live under the primary checkout's
ignored `build/office-cached-context`. Use `final-measure.py` for the main cohort,
`holdout-final.py` for loading controls, and the existing task JSONs with
`tools/bench_snapshot_task.py --repeat 1`. `--guest-profile` and
`--instruments --jit-debug-map` are separate diagnostic runs. Existing guest
images and small private compressed children were reused; successful children
were removed. No VM images, build products or raw traces are committed.
