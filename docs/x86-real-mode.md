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

Far-pointer loads validate the offset and selector as one operand before either
read; invalid register forms raise #UD. Register-indexed bit strings wrap the
adjusted offset to address size. Hardware vectors `0FAB#21` and `660FB3#1`
establish this wrap, and literal tests cover it independently. See the manual's
[BT entry](https://pdos.csail.mit.edu/6.828/2018/readings/i386/BT.htm) and
[LDS/LSS/LES/LFS/LGS entry](https://pdos.csail.mit.edu/6.828/2018/readings/i386/LDS.htm).
The focused five-shard validation (BT/BTS/BTR, invalid LEA, LGS) now passes;
289 literal task/x87/CPU checks and 355,094 native core checks pass as well.
