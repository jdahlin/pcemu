# 386 task switching and x87 specification checks

`src/cpu.cpp` documents the implemented hardware task-switch subset alongside
its state transitions. `src/cpu_x87.cpp` documents the register stack, encoded
fields, memory formats, rounding and host arithmetic policy. References are
pinned to exact Intel manual revisions and one-based PDF page anchors:

| Reference | Used for |
| --- | --- |
| [Intel 253668-085US, October 2024, volume 3A, 9.2–9.4](https://cdrdv2-public.intel.com/835754/253668-sdm-vol-3a.pdf#page=267) | TSS byte layout, descriptor validation, task gates, address-space changes, nested-task linkage and fault context. |
| [Intel 253665-081US, September 2023, volume 1, 8.1](https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf#page=203) | Physical versus logical x87 stack, tags, control/status words and environment layout. |
| [Intel 325383-085US, October 2024, volume 2](https://cdrdv2-public.intel.com/835757/325383-sdm-vol-2abcd.pdf#page=506) | Instruction encodings, integer-indefinite results, initialization, save/restore and operand exceptions. |

Run the dedicated RAM-only suite with:

```sh
make build/x86-task-x87-tests
./build/x86-task-x87-tests
ctest --test-dir build -R '^x86-task-x87-tests$' --output-on-failure
```

It is also included in `make test`. It needs no DOS image, download, guest disk
or graphical frontend. The fixtures use literal instruction bytes, descriptor
values, TSS offsets and expected memory contents rather than implementation
constants. The original core tests still cover rounding modes/ties, read-only
operands and two-byte status stores at page boundaries; snapshot tests cover
floating-point state and older-format compatibility.

The dedicated suite covers:

- Task-gate CALL/JMP, complete general/segment state, busy-bit transitions,
  backlinks, NT IRET through the JIT fallback, TS and local debug enables.
- Invalid/null/local/out-of-bounds TSS selectors, unavailable/busy/type/presence
  checks, minimum TSS size, gate privilege/presence errors and incoming LDT use.
- CR3 changes with a warm translation and a missing incoming TSS page before
  the outgoing state is saved.
- Eight-entry stack wrap, physical tag placement, FXCH, FFREE, comparison codes
  and FNCLEX preserving comparison results.
- ADD/MUL/SUB/SUBR/DIV/DIVR across D8/DC/DE register forms, with distinct operands
  and explicit pop checks.
- Masked invalid integer conversions at 16/32/64 bits, including NaN, infinity
  and the positive signed-range boundary.
- Protected-mode 14/28-byte environments and 94/108-byte save images with nonzero
  TOP, logical register ordering, restore mapping and FNSTENV exception masking.
- Faulting cross-page FSTP/FNSAVE preserving stack/environment/destination bytes,
  and CR0.EM/TS raising #NM before stack mutation.

These tests establish the supported subset, not complete 80387 or task-switch
conformance. Source `FIXME` comments explicitly identify extended precision,
real-result rounding, exception and stack faults, pointer tracking, real-mode
environment packing, transcendental details and reserved encodings. Task-switch
gaps include 16-bit TSS support, full late-fault sequencing and incoming segment
qualification, trap-bit handling, static TSS-field writes and CR3 loads with
paging disabled. This documentation/test change preserves existing behavior;
it does not silently treat these gaps as architectural guarantees.

For end-to-end compatibility, `python3 tools/smoke_quake.py` uses an existing
prepared DOS/shareware disk and a temporary compressed overlay, runs headlessly
with the interpreter and verifies rendered gameplay and keyboard movement.
See [Quake setup and verified limits](quake.md).
