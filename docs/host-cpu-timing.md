# Host process CPU time for snapshot tasks

`bench_snapshot_task.py` records `processCpuTime` for each task interval on macOS,
and `medianProcessCpuMs` when all runs have valid readings. User and system CPU
time come from `proc_pid_rusage(RUSAGE_INFO_V0)`, read immediately before sending
the task commands and after observing the expected pixels. The two queries do
not sample, suspend or instrument the emulator.

The counters require Mach absolute-time conversion with `mach_timebase_info`.
On the tested Apple Silicon host, the numerator/denominator are 125/3. Treating
the raw counters as nanoseconds under-reports CPU time by about 41.7x. The ABI and
counter assignment are described in Apple's
[resource header](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/resource.h)
and [task resource-accounting implementation](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/bsd_kern.c).

The live-child test sleeps for 120 ms, then consumes 80 ms according to Python's
independent process CPU clock. It verifies that sleep is excluded, the CPU units
are correct, and another process can be queried. Missing processes, changed
identities and counter regressions cannot become zero-time measurements.
Unsupported platforms or query failures produce an explicit unavailable status;
the benchmark can still verify the guest task.

Run the accounting tests with:

```sh
python3 tests/test_host_process_cpu.py -v
```

Process CPU includes all emulator threads, guest execution, control/video output
and guest idle work inside the response interval. It excludes restore, blocked
I/O wait and descheduled time. It is not isolated guest-execution time: capture
frequency, cache warmup, host clock frequency and incidental guest work still
affect it. The existing response and execution-wall metrics retain their own
scopes and remain available alongside it.

Three alternating Excel pairs compare the frozen native-register-CALL baseline
with the subsequent inline instruction-fetch TLB candidate. All six restores
produce the expected recalculation pixels. Candidate process CPU is lower in all
three pairs: baseline/candidate ratios are 1.119, 1.045 and 1.018, with a median
of 1.045 (about 4.3% less process CPU). Response favors the candidate in two pairs.
The first baseline includes more observed guest instructions than the other
runs; no samples were discarded or normalized. This is one loaded workload on a
shared host, not a general performance claim.

A separate calibration measured 100.049 ms against Python's 100.024 ms process
clock. All three accounting tests and Python syntax checks pass. The emulator
binary is unchanged by this tooling change. Successful temporary snapshot disks
are removed as before.

[Raw paired results, calibration, source hashes and test output](measurements/process-cpu-excel-2026-10-25.json).
