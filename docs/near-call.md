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

Native register-indirect CALL remains the next performance step; this change
corrects the existing reference and direct-call paths first.
