# Diskless dependent-load benchmark

`memory-chase-bench` runs a 32-bit x86 loop directly in guest RAM. A deterministic
random ring uses 64-byte nodes with a next pointer and a value of one. Each
iteration follows the pointer, adds the next node's value, decrements ECX and
branches. It exercises dependent memory loads rather than arithmetic alone.

```sh
./tools/build.sh memory-chase-bench
ctest --test-dir build -R '^memory-chase-' --output-on-failure
./build/memory-chase-bench --loops 20000000 > /tmp/memory-chase.json
./build/memory-chase-bench --backend interpreter --loops 1000
./build/memory-chase-bench --dispatch single --paging paged
```

Options are `--loops` (1–100,000,000, default 10,000,000), `--backend`
(`jit`/`interpreter`), `--dispatch` (`batch`/`single`) and `--paging`
(`flat`/`paged`/`both`). Defaults are JIT, batch, both. JIT requires an Apple
ARM64 host; the interpreter is also available for correctness comparisons.
Output is JSON; a nonzero exit indicates an invalid argument or failed result.

Working sets are 4 KiB, 32 KiB, 512 KiB and 8 MiB, within a 16 MiB guest.
Paged cases identity-map RAM with 4 KiB pages. Initialization, allocation and
verification are outside the measured interval; cold JIT compilation is inside.
There are no guest disks, devices, snapshots or frontend. Every case verifies
`4 * loops + 1` instructions including HLT, the sum, final pointer and all ring
links/values. JSON records native instruction and slow-memory-helper counts.
A traversal contains two dependent reads; reported time is not isolated host
RAM latency. This does not measure bulk bandwidth, writes or Windows behavior.

The initial three-run, 20-million-traversal record uses the default CMake Debug
`-O1 -g` executable, not the frozen Office `-O3` build. Each case executes
80,000,001 guest instructions. Flat cases make zero slow-memory calls; the
8 MiB paged case makes 10,127,014, versus 121 at 512 KiB. This exposes a useful
TLB-pressure workload beyond the current small hot-memory kernel. Timings and
build identity are preserved in the [baseline record](measurements/memory-chase-baseline-2026-10-26.json).
The fixed case order and shared host show visible timing drift, so the record
is a reproducible starting point rather than evidence of an optimization.

Interpreter/JIT and single/batch combinations pass at one and 1,000 traversals
for all eight cases. Both CTest checks pass. Future comparisons should preserve
compiler flags, alternate candidate/baseline runs and retain exact checksums.
