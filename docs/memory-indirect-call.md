# Native memory-indirect near CALL

The Excel dependent-recalculation guest profile identified roughly 9.86 million
interpreted `FF /2` memory CALLs. The native path now accepts those addressing
forms as well as the existing register form. This targets a measured remaining
interpreter path; it does not remove the interpreter or enable every x86 opcode.

The decoder keeps separate pointer-read and stack-store descriptions. Generated
code computes the pointer offset from cached pre-push registers, calls the
existing guarded RAM-only read helper, saves the operand-sized destination,
checks the current CS limit and then uses the existing guarded native push.
The target commits only after that push succeeds. This includes pointers using
old ESP and pointers located in the return-address slot itself. The two metadata
entries are allocated before their addresses are embedded in generated code.

A declined pointer read has no device callback or guest data write. If the
subsequent stack access declines, replay may reread ordinary RAM; device reads
occur only in interpreter replay. Cross-page reads, MMIO, watched stack writes
and faults retain the existing precise replay path. Page-table accessed/dirty
updates follow the existing helper semantics. Both pointer-decline and invalid
CS-target guards share the pre-instruction replay exit. A successful stack write
that changes the code page still commits the dynamic destination before leaving
native code. The rules follow the CALL operation in the
[Intel instruction reference](https://www.intel.com/content/dam/support/us/en/documents/processors/pentium4/sb/25366621.pdf),
as documented in [near-call validation](near-call.md).

This version deliberately calls the C++ RAM helper for the pointer read; the
stack fast path remains emitted ARM64. It is native instruction lowering with a
memory helper, not a helper-free two-access instruction. A future pointer-read
inline path should be judged on measured whole-task benefit and the same fault
and device-ordering tests.

## Validation

`tests/jit_memory_call_test.cpp` passes 8,677 checks. It compares native state,
RAM (including page-table effects), flags, destination and counts against the
interpreter over 16/32-bit operand/address/stack combinations, paging, FS/GS,
SIB/no-base addressing and pointer/return-slot aliasing. Reusing a compiled block
after a pointer change and TLB flush sees the new target. MMIO callbacks occur
exactly once; pointer/stack page faults, invalid-target precedence, unusable SS,
cross-page faults, CR2, watched writes and self-modifying stack writes are checked.
The existing 33,360 stack/JIT and 354,948 core checks also pass.

A RAM-only CALL/RET/checksum loop executes 50 million guest instructions per
case, spanning eight code-width, stack-width and paging combinations. Three
alternating pairs have case-median baseline/candidate ratios of 1.47–1.72.
A separate interpreter cross-check passes. The runner source, exact counts,
checksums, commands and times are retained in the measurement record. This is
a deliberately CALL-heavy kernel, not an application or historical-CPU rating.

Run the focused RAM-only correctness test without a disk or frontend:

```sh
mkdir -p build/memory-call
clang++ -std=c++20 -O3 -Wall -Wextra -Wpedantic -Isrc \
  src/cpu.cpp src/jit.cpp src/jit_decode.cpp src/jit_flags.cpp \
  src/jit_runtime.cpp src/jit_arm64.cpp tests/jit_memory_call_test.cpp \
  -o build/memory-call/memory-tests
build/memory-call/memory-tests
```

## Application measurement

Six alternating restored Excel pairs keep the same x86 core, VGA frame renderer,
task, snapshot and headless observation method. Only the decoder, IR and native
emitter differ. Native coverage rises from roughly 97.69% to 98.41%. Within each
pair, observed guest instruction counts differ by less than 0.04%.

Process CPU ratios (baseline/candidate) are 1.055, 1.081, 0.833, 1.452, 0.744,
and 1.140: four favorable, two unfavorable. The median is 1.068, but the wide
variation under a shared desktop host does not establish a stable application
percentage. Another ARM/HVF guest and desktop processes were observed running;
none were stopped. Restore is outside the CPU interval; capture/control/video
and guest idle work remain inside it. Both batches, including the unfavorable
pairs, are retained. No guest disk is created for the RAM kernel; successful
Excel private compressed children are removed by the task harness.

[Build provenance, complete RAM kernel and all six application pairs](measurements/native-memory-call-2026-10-25.json).

The frozen candidate also passes headless DOS 6.22, Win3.11 Notepad/save/DOS
readback, and both rtts Win95 and installed OSR2 Calculator/clipboard/Notepad
with guest restart and exact stopped-guest FAT readback. All successful private
compressed disks are removed. Fresh Setup is not rerun in this iteration; its
previous evidence predates this new lowering. It remains a final compatibility
gate before declaring the full goal achieved.
