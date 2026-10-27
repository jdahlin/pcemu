# 80386 real-mode specification and regression notes

The architectural reference is the **Intel 80386 Programmer's Reference Manual
(1986)**, [chapter 14](https://pdos.csail.mit.edu/6.828/2018/readings/i386/c14.htm).
These notes track implemented rules and outstanding work; they do not redefine
the architecture or replace the pinned external hardware results.

| Rule | Implementation and independent validation |
|---|---|
| A complete memory operand must fit its cached segment limit. SS violations use #SS; other segments use #GP. | `CPU::address` accepts the access width; decoded scalar operands retain their segment; native memory guards validate the complete span. Literal regression cases check word/dword endpoints, address-size overrides and absence of MMIO side effects. [14.7, items 7 and 17](https://pdos.csail.mit.edu/6.828/2018/readings/i386/s14_07.htm). |
| Sequential instruction fetch does not wrap at FFFFh. An instruction can commit before its following fetch faults. | Interpreter and JIT decoders retain the next offset beyond FFFFh. The hardware adapter models a fault on the injected terminator's fetch separately from a fault within the tested instruction. Literal regressions cover partial encodings at FFFFh. [14.7, item 8](https://pdos.csail.mit.edu/6.828/2018/readings/i386/s14_07.htm). |
| The entire instruction, including prefixes and immediates, is limited to 15 bytes. | Both decoders count all bytes; #GP restarts at the first prefix. The LOCK validation lookahead restores its decoder count. [14.7, item 6](https://pdos.csail.mit.edu/6.828/2018/readings/i386/s14_07.htm). |
| Interrupt dispatch uses IDTR and a four-byte far pointer; the frame stores FLAGS, CS and IP. | The pointer is read before stack writes. The overlap ordering is established by pinned hardware vector `F6.6#1403`, with SS=0 and SP=8, and an independent literal regression. The manual describes the table and frame but does not explicitly specify this alias ordering. [14.3](https://pdos.csail.mit.edu/6.828/2018/readings/i386/s14_03.htm). |

The tests run interpreter and JIT, including fallbacks, and keep hardware input
and expected states unchanged. `tests/x86_task_x87_test.cpp` contains the literal
regressions and runs under `make test`. The wider tests and their limitations
are described in [external x86 test tiers](x86-external-tests.md).

Protected-mode expand-down segments, access rights and full exception-delivery
sequencing need separate checks; the real/V86 limit fix does not establish those
features. Synthetic tests that intentionally use flat cached real-mode segments
must initialize their expanded limits explicitly, rather than relying on missing
architectural checks.

Far-pointer loads validate each complete component before accessing it; invalid
register forms raise #UD. The selector address wraps independently to address
size (`0FB2#1890`), unlike bytes within either component. Register-indexed bit strings wrap the
adjusted offset to address size. Hardware vectors `0FAB#21` and `660FB3#1`
establish this wrap, and literal tests cover it independently. See the manual's
[BT entry](https://pdos.csail.mit.edu/6.828/2018/readings/i386/BT.htm) and
[LDS/LSS/LES/LFS/LGS entry](https://pdos.csail.mit.edu/6.828/2018/readings/i386/LDS.htm).
The focused five-shard validation (BT/BTS/BTR, invalid LEA, LGS) now passes;
289 literal task/x87/CPU checks and 355,094 native core checks pass as well.

The hardware adapter applies count-dependent flag rules for shifts and rotates:
OF is compared for count one; count zero must preserve all flags. SHL/SHR CF is
undefined when the count reaches operand width. Double shifts with a count
above operand width have an undefined result and are counted separately as
`undefinedDoubleShift`, selected by encoding and initial count before execution.
Faulting cases remain in validation. This follows the manual's
[SHLD](https://pdos.csail.mit.edu/6.828/2018/readings/i386/SHLD.htm) and
[SAL/SAR/SHL/SHR](https://pdos.csail.mit.edu/6.828/2018/readings/i386/SAL.htm)
entries. Upstream expected states are unchanged. Six adapter unit tests include
count-zero, count-one, variable-count and undefined-width boundaries.

Additional scalar rules now have literal interpreter/JIT checks: BOUND compares
signed lower/upper bounds and raises #BR; the upper bound address wraps to the
address size independently (hardware `62#1725`). Zero-count SHLD still validates
the memory span without reading it. Whole-ring ROL/ROR update CF even when the
operand is unchanged. RET validates its destination before committing SP.
386 PUSHFD clears reserved bits 31:16 in the saved image. The corresponding
nine-shard hardware run passes, as do 299 literal CPU checks and 2,957,189 byte
JIT checks. See the manual's BOUND, SHLD, ROL/ROR, RET and PUSHF entries.

Segment POP with a 32-bit operand reads a 16-bit selector and advances SP by
four; far-return frame slots still validate their full operand width. Far
transfers and returns reject real-mode offsets above FFFFh. Operand decoding
now postpones address validation until the final accessed offset is known, so
bit-string adjustments can bring an initial offset into range and invalid
segment encodings retain #UD priority. x87 memory operations preflight their
complete segment span too. DAS retains carry from its low-digit borrow.

Real-mode REP comparison faults retain the last completed comparison flags,
as observed in hardware `66A7#460`; protected-mode restart flags retain their
existing Intel SDM behavior. The 1986 manual does not explicitly settle that
real-mode fault detail, so this is recorded as a measured 386EX compatibility
rule. A literal two-iteration boundary test distinguishes the behaviors.

The adapter reports requested software interrupts separately from CPU faults,
models a terminating HLT's unwrapped next IP, and handles the bounded prefetched
re-entry found in `C2#1489`. It executes supplied instruction bytes until control
leaves the original encoding; it does not derive CPU state from expected output.
A separate synthetic RET-to-self test verifies that path. The native input
format is now `PCX86V2` with an explicit encoding length. Seven adapter unit
tests and 307 literal CPU checks pass. Focused segment-pop, DAS, REP, interrupt,
bit-string, near-return and far-return shards pass.

ENTER frame-chain reads retain operand-width addressing in protected mode;
386EX real-mode captures with a 16-bit stack use BP for that chain. Both cases
are tested, including high virtual protected-mode frame addresses. Native POP-to-memory
checks both complete spans before committing SP or RAM. REP RAM spans stop at
the cached segment limit even with 32-bit address overrides.

The following are measured 386EX details beyond the manual's simple PUSHA/POPA
pseudocode, kept explicit rather than claimed as requirements for every x86 CPU:

- A faulting real-mode PUSHAD retains writes to the low stack slots performed
  before the crossing slot (`6660#302`). The saved SP is restored before fault
  delivery. The interpreter uses the observed low-to-high store order.
- POPA retains completed GPR loads on a later stack fault (`61#681`). Fault state
  carries an explicit committed-register mask; ordinary faults still restore all
  registers, and REP retains its separate continuation behavior.
- Real-mode POPAD with a 16-bit stack discards the popped ESP low word, but loads
  its high word (`6661#0`). This measured rule is limited to that mode; it is not
  presented as the manual's architectural discard-ESP rule for every processor.

Literal tests verify each partial-state result, ENTER's frame chain, pointer
wrapping and a fault after the first STOSD iteration. The four PUSHA/POPA shards
pass all 20,000 executions; 319 literal CPU checks, 255,625 read-JIT checks and
seven adapter tests pass. Wider hardware validation continues separately.

INC/DEC publish the preserved carry flag before an observable device write.
The previous interpreter temporarily exposed ADD/SUB's carry to the callback,
then restored it afterward. A literal MMIO test now checks the value and carry
at the write in both backends; the RMW suite passes 947,820 checks and the literal
CPU suite passes 323 checks.

I/O validation now decodes the MOO `CYCL` port transfers with strict 15-byte
record bounds. Input bytes come from the captured bus, never the expected final
registers. Output bytes, ports, directions and counts are checked. A wide device
callback may reorder the captured byte lanes within its single operation, since
the 386EX can perform split bus transfers in a different order. Architectural
operation ordering remains checked. `PCX86V3` includes the byte traces. Nine
adapter tests include truncated cycles, byte lanes, supplied input values and
negative output checks.

The focused I/O run executes 11,999 vectors in both backends. Output and string
I/O shards pass; six input vectors (12 executions) disagree between externally
captured bus data and the captured accumulator. For example `E5#422` records
FFh at both ports 21h/22h but a final AX of 7FFFh. These mismatches remain visible;
an integrated peripheral or capture issue is a possibility, not an established
explanation. The adapter does not copy expected accumulator bits to force a pass.

The stack/MMIO regression fixtures explicitly set their flat cached segment
limits, including wide return destinations. This preserves their intended
memory/device tests with architectural bounds enabled. All 43,600 stack-JIT
checks pass with those fixture limits.

The remaining byte-IDIV discrepancies have an independent ROM reproducer in
`tests/x86_idiv_reference.asm`, run by `make test-x86-reference`. Intel
[3.2.4](https://pdos.csail.mit.edu/6.828/2018/readings/i386/s03_02.htm) and the
[IDIV entry](https://pdos.csail.mit.edu/6.828/2018/readings/i386/IDIV.htm) require
#DE when the quotient does not fit AL. Hardware `F6.7#469` supplies AX=648Ch and
a divisor B7h (-73): its quotient is -352, outside -128..127. The capture instead
returns AX=0C80h without #DE. `F6.7#516` has the same contradiction for -25487/71.
The ROM checks both overflows and the valid -128 result; interpreter, JIT and
QEMU 486 TCG agree and pass. This is evidence of a reference/model discrepancy,
not a reason to replace the architectural overflow check with saturation. The
hardware failures remain visible. Faulting AAM base-zero status-flag differences
also remain under investigation; no additional masks were added to hide them.

Real-mode data/stack segment reloads retain their cached limits and size;
V86 loads install 64K attributes. Resetting a flat DS/ES limit on POP broke
HIMEMX's #GP handler: it enters protected mode to load flat descriptors, leaves
protected mode, restores DS/ES and retries its extended REP MOVSD. The limit
checks made that pre-existing cache bug visible. Preserving those limits fixes
the actual FreeDOS regression while retaining bounds checks. See Intel
[14.5, switching back to real-address mode](https://pdos.csail.mit.edu/6.828/2018/readings/i386/s14_05.htm),
which requires loading real-mode limits before clearing PE. Literal DS/ES/SS
reload and V86 reset tests cover the distinction.

Native indirect far transfers now validate both pointer components, and
zero-count double shifts validate the span inline without touching memory.
The extended JIT fixtures explicitly initialize flat caches where they test
address arithmetic or MMIO rather than segment bounds; invalid real-mode far
return targets count as replayed faults. All 35 JIT/CPU CTest suites pass,
including the original protected-mode high-EBP ENTER cases and native zero-count
requirements. The literal CPU suite has 327 checks.

`smoke_freedos.py --runner PATH` freezes a selected binary for isolated regression
comparisons. An archived pre-bounds binary passed XMS, the first bounds commit
reproduced the hang, and the cached-limit fix restores XMS on the current binary.
The fixture itself is unchanged; each run uses its own compressed overlay.

`make test-x86-extended` builds the CMake `x86-extended-tests` target and runs the
35 labelled CPU/JIT suites with `X86_TEST_JOBS` workers. Registration happens in
`pcemu_program`, so a new `jit-*` test is automatically included without extending
a second static target list. This tier is offline and independent of FreeDOS,
QEMU and hardware-corpus downloads.

The two contradictory IDIV overflow inputs also run as literal regressions in
`make test`, protecting the specified #DE behavior from a future change made only
to match those captures. The current literal suite passes 331 checks.

## Single-step debug traps and packed DOS executables

The instruction completion path samples TF before execution and delivers INT 1
as a trap after successful completion. The saved IP is the next instruction
(or the REP prefix while repetitions remain), DR6.BS is set, and interrupt
entry clears TF. POPF/IRET setting TF arm the following instruction; POPF
clearing an already-set TF still takes its pending trap. Software interrupts
and MOV/POP SS suppress their own single-step boundary. Faulting instructions
retain fault priority and do not also take a completion trap. Under TF, repeated
string instructions execute one element per trap, bypassing bulk RAM copies.
The JIT already routes TF execution through the interpreter boundary.

Reference: Intel 80386 PRM, [12.3.1.4 Single-Step Trap and 12.3.3 Debug
Exceptions](https://pdos.csail.mit.edu/6.828/2018/readings/i386/s12_03.htm).
This implements TF stepping, not execution/data breakpoints, DR7.GD detection,
or task-switch debug traps.

The original Second Reality executable exposed this missing behavior: its
protection/unpacking code executes with TF set and requires INT 1 processing.
Without traps it reaches encrypted bytes and raises #UD at 26a8:01a7 in the
minimal FreeDOS fixture. With stepping it reaches the setup menu. Loading
HIMEMX with DOS=HIGH is needed to retain its required 570,000 bytes of free
conventional memory with this FreeDOS configuration. Reaching setup does not
establish demo completion, raster correctness, or sound support.

Literal CPU tests cover both engine entry paths, return IP/flags, IRET, clearing
TF, SS suppression, REP progress, and INT3 priority. The RAM-only VGA guest probe
also exercises an INT 1 handler that clears saved TF and verifies the return IP;
`python3 tools/smoke_vga_bios.py --qemu` runs that exact guest on QEMU as well.
