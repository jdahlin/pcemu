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

ENTER frame-chain reads now use the stack address width, not operand width;
this matters with an arbitrary high EBP and a 16-bit stack. Native POP-to-memory
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
