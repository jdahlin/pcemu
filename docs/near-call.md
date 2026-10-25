# Near CALL target validation

Near relative and absolute CALL validate the operand-size-masked target against
the cached CS limit before pushing the return address. An out-of-limit target
raises #GP(0) at the CALL instruction without changing SP or writing the stack.
This follows the CALL operation in the
[Intel instruction reference](https://www.intel.com/content/dam/support/us/en/documents/processors/pentium4/sb/25366621.pdf).
The downloaded manual is retained externally under `/Volumes/data/pcemu/specs/intel/`.

The interpreter checks both E8 and FF /2, including register and memory
operands. The native direct-CALL path reads the current CS limit on every
execution and declines invalid targets before the guarded stack access. Its
replay path explicitly requests interpreter delivery of the fault. A cached
block remains correct when the segment limit changes without code changes.

Tests cover direct/register/memory forms, word/dword operands, cached segment
states, precedence over unusable SS and MMIO, and preservation of the preceding
ADD's register result and flags. A target equal to the limit succeeds; shrinking
the limit then faults before touching the poisoned return slot. Wide-target
positive fixtures explicitly configure a 4 GiB code segment.

The stack/JIT suite passes 27,641 checks and the core suite 352,644. The fixed
binary boots DOS and passes headless Win3.11 save/exit, rtts Win95 applications/
restart, and fresh OSR2 applications/restart/exact file readback. OSR2's initial
observation was blocked by a delayed Welcome window; the same live guest was
resumed after activating and closing that known window. Successful private
disks were removed.

[Build, source/specification hashes and runtime evidence](measurements/near-call-target-limit-2026-10-25.json).

Native register-indirect CALL (`FF /2`, register operands) now shares the guarded
native stack-store path. The emitter captures the masked target before changing
SP/ESP, checks the current CS limit before the push, and commits the destination
only after success. An otherwise unused host scratch slot preserves the target
across stack helpers. Memory-indirect CALL remains interpreted.

Expanded tests cover every target register, recently modified targets, SP/ESP
aliasing, mixed operand/stack widths, paging and stack faults, MMIO replay, and a
push overwriting the CALL's own code. The stack/JIT suite passes 33,360 checks,
the core suite 352,644, and flag-liveness tests 91,745,234. This candidate's Windows
and Excel performance validation is pending: `/Volumes/data` was unmounted and
the comparison's baseline exited before loading the external snapshot. No
application speedup is claimed for this change.

[Native register CALL build and validation evidence](measurements/native-register-call-2026-10-25.json).


A subsequent RAM-only loop checks register CALL, an increment, RET, decrement
and conditional branch with 16/32-bit code, 16/32-bit stacks, and flat/paged RAM.
Each measured case executes 10 million calls and validates 50 million guest
instructions, the exact checksum, destination and restored stack pointer. Three
alternating baseline/candidate pairs observed per-case median speed ratios of
1.44–1.71x (whole-matrix median 1.57x). This isolates a CALL-heavy path, excludes
devices and guest OS work, and ran alongside another ARM guest on a shared host.
It does not establish application performance. A 17-iteration interpreter
cross-check also passes in all eight modes. No guest disk was allocated.

[Complete kernel, build commands, hashes and paired RAM results](measurements/native-register-call-ram-2026-10-25.json).
