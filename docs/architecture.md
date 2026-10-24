# Machine and translation architecture

This is an original software x86 machine for Apple Silicon. It currently boots
MS-DOS 6.22 and Windows 3.11 **standard mode** to Program Manager. C++20 implements
the processor, memory, disk and devices. SDL2 supplies the host window and input.
No existing CPU emulator, hypervisor or translation engine is linked or run.

## CPU

`CPU` stores eight 32-bit general registers, EIP, EFLAGS, six hidden segment
caches, control registers, GDT/IDT/LDT and task-register state. The interpreter
implements common 8086–386 integer operations, 16/32-bit effective addresses,
protected-mode descriptors and call/interrupt gates, privilege-stack transitions,
restartable segment faults, and basic two-level page translation. It is a
compatibility implementation, not a complete or validated 80386.

Unimplemented valid instructions stop with the guest CS:EIP. Selected architectural
faults are delivered through the guest IDT. Windows uses #UD for its software
emulation machinery and #NP to demand-load NE segments. Fault delivery restores
register state to the faulting instruction before constructing its interrupt
frame. Other fault/protection cases remain incomplete.

The real-mode BIOS is implemented here as a service layer. IVT entries point to
small callable guest ROM stubs. Guest hooks take precedence, including DOS's
interrupt handlers. BIOS functionality includes text output, keyboard, CHS and
EDD disk transfers, extended-memory reporting and VGA palette services. This
boots the disk's actual MBR, partition boot sector, IO.SYS and MSDOS.SYS; DOS and
Windows services execute as guest code, rather than host DOS/Windows API shims.
The original firmware reset entry at FFFF:0000 and 8042/port-92 reset
requests rebuild firmware/device state and boot the disk again. VGA ROM
initialization is repeated when enabled. Full ROM POST is not implemented.

## AArch64 backend

`Jit` translates up to 64 x86 register operations, direct branches and guarded
8/16/32-bit RAM moves into native AArch64.
Eight guest GPRs stay in w20–w27 throughout a block; x19 holds the CPU state.
The opt-in `--jit-follow-jumps` mode follows unconditional direct jumps while
forming a block. The original linear and physical code page limits and total
64-instruction budget still apply, including repeated backward jumps. Each
executed jump contributes to the returned guest instruction count. Registers
and pending flags remain cached across these jumps; conditional branches,
CALL/RET and counter branches still terminate the block. Unsupported targets
or failed speculative fetches leave the last jump terminal. Existing memory
exit descriptors retain the original guest IP and prefix instruction count.
This is bounded block formation, with no native links between cached blocks.
Default execution continues to use basic blocks.
It lowers immediate/register moves, ADD/SUB/AND/OR/XOR, CMP, TEST, NOP,
short/near JMP and Jcc, LEA, register PUSH/POP, immediate PUSH, and MOV memory
operands (including moffs), plus read-only memory CMP/TEST forms, with
8/16-bit partial-register preservation, including the high byte registers.
Register INC/DEC, XCHG, MOVZX/MOVSX, IMUL, immediate-count SHL/SHR/SAR,
LOOP/JCXZ, direct near CALL and near RET also have native lowering.
Byte memory operations use the same precise fault, MMIO and code-invalidation
guards as wider moves. Backward flag liveness removes dead computations within
a block. Supported flag calculations and conditional branches emit ARM64 code;
flags materialize at block/guarded exits and before partial flag writers which
need preceding bits. Successful RAM operations retain cached GPRs and pending
register-ALU flags.
A declined guard commits prior effects and the faulting IP before interpreter
replay; a code-changing store commits its own successful effects before exit.
A terminating conditional branch consumes the materialized flags in generated
code; its destination executes on the next dispatch.
LEA forms offsets without segment or RAM access. Stack blocks validate SS.B
at entry independently of CS size; stack pointer updates commit only after
successful accesses. POP SP/ESP applies the increment before the popped
value, including mixed operand/stack widths. MMIO and observed writes replay
in the interpreter so callbacks see the architectural stack pointer.
Instructions outside that lowering set execute in the interpreter.

The macOS code arena uses `MAP_JIT`, per-thread write protection, and instruction
cache synchronization. Guest writes overlapping translated instruction bytes update page generations. Cached blocks also
validate CS base, mode, A20 and control-register state. A block cannot span a
physical page. Paged block entry validates permissions and the physical code
page. A TLB epoch change rechecks the mapping; unchanged mappings retain
decoded code, while remapping or revoked fetch permissions still invalidate/fault. Single-stepping uses the interpreter. Cold addresses run in the interpreter
for eight visits before compilation to avoid translating one-shot installer code.

This is a baseline backend. The register-only microbenchmark is fast, but it does
**not** establish Rosetta-like application or whole-system performance. Memory
ALU operands, remaining stack forms, far transfers, indirect CALL/JMP, per-instruction fallback
dispatch, remaining translation gaps
and frequent mode transitions are the major remaining optimization targets.

### JIT source modules

| Module | Responsibility |
| --- | --- |
| `src/jit.cpp` / `jit.hpp` | Dispatch, cache validation, warmup, code-arena lifetime and compilation orchestration. |
| `src/jit_decode.cpp` | Decode x86 instructions and addressing modes into a single-page block. |
| `src/jit_ir.hpp` | Shared typed instruction kinds and decoded block representation. |
| `src/jit_memory.hpp` | Guest memory-access descriptors shared by decoding and guarded runtime access. |
| `src/jit_flags.cpp` / `jit_flags.hpp` | Architecture-independent flag liveness and precise-exit requirements. |
| `src/jit_arm64.cpp` | ARM64 instruction emission, register allocation, guards, prologue/epilogue and code publication. |
| `src/jit_runtime.cpp` / `jit_runtime.hpp` | C++ ABI helpers for generated code, including restartable guarded memory access. |

The decoder can update block metadata such as stack-width dependence. The
dispatcher applies the existing admission policy before passing a decoded
block to the ARM64 emitter. Runtime helper exceptions never unwind through
generated frames; declined accesses return to the dispatcher for interpreter
replay. These interfaces do not change guest execution or snapshot formats.

## Devices

- Raw writable disk with BIOS CHS translation matching the partition's BPB.
- Private VHD-to-sparse-raw conversion implemented in Python; original VHD stays intact.
- VGA text display, four 64 KiB graphics planes, latches, write modes 0–3,
  read modes, chain-4 addressing, indexed registers, palette and DAC.
- Keyboard scan-code queue and IRQ1 delivery, extended scan sequences and focus-loss release.
- Original PS/2 mouse, 8042 AUX interface/IRQ12 and INT 15/C2 firmware callbacks.
- Original DISPI/VBE banked/linear framebuffer, 16 MB VRAM and 8/15/16/24/32-bit display.
- PCI display identification/configuration, real-mode PCI BIOS and original BIOS32 firmware.
- ATA primary task file with CHS/LBA28 PIO reads, writes, and IDENTIFY.
- Dual 8259 PICs with priority, IRR/ISR, EOI, initialization and cascade.
- Instruction-clocked 8254 PIT, CMOS and A20 controls.
- Keyboard controller output latch, IRQ1 backpressure, command replies, LEDs,
  interface enable/disable and one-shot controller commands. Host input always
  enters the scan-code stream, including temporary real-mode transitions inside
  Windows. Firmware translates scan codes into BIOS keys.

The disk controller, DMA, PIC, PIT and keyboard controller are not complete
hardware models. BIOS disk calls and ATA PIO share the raw disk backing. There is no
audio or hardware FPU. See [input/video integration](input-video.md)
for the guest display driver and host controls. VM86 transitions and page protection
are implemented but broader compatibility remains under test. Windows
386 enhanced mode is not supported. The timer is instruction-driven, not a
cycle-accurate PIT.

## References

Downloaded reference: `docs/i386.pdf`, Intel *80386 Programmer's Reference Manual*
(1986), from MIT's course reference mirror:
https://pdos.csail.mit.edu/6.828/2018/readings/i386.pdf

VGA register and memory pipeline specification:
https://www.osdever.net/FreeVGA/vga/graphreg.htm

IBM BIOS palette interface specification:
https://www.ardent-tool.com/docs/pdf/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf

Microsoft Windows 3.1 Resource Kit, setup-related files and WIN.COM composition:
https://vtda.org/docs/computing/Microsoft/OS/030-31645_Microsoft_Windows_3.1_Resource_Kit_1992.pdf

Current installer and Windows 95 evidence: [GOAL_STATUS.md](GOAL_STATUS.md).

A `fault-trace PATH [HEX_VECTOR_TO_STOP]` command in the headless control file records guest exception
vectors, error codes, instruction addresses, flags, CR2, LDT base and registers.
The observer runs after instruction rollback and before guest fault delivery,
so a nested failure in the error handler does not obscure the original fault.
The optional vector deliberately stops before guest delivery and leaves a RAM
dump of the original faulting state for descriptor/stack inspection.

The guarded memory helper accepts single-page RAM after runtime segment,
A20 and MMU checks. It catches faults internally and declines before device
effects; the dispatcher replays exactly one interpreter instruction after the
native frame returns. Cross-page accesses and watched stores also decline.
Successful stores that invalidate translated code exit immediately before any
future stale IR executes. Memory operand metadata is owned by its cached block
and survives for the lifetime of the emitted native function.

Ordinary RAM accesses now have an inline ARM64 path. It checks segment usability,
page boundaries, RAM bounds, the VGA aperture and A20. Paged accesses validate
the software TLB epoch/tag, user permissions and (for stores) an already-set
dirty bit. Misses and required page-table updates use the guarded helper. Stores
fall back if their 64-byte code bitmap word contains any translated bytes, or
if they cross a bitmap word; successful inline stores therefore cannot change
translated code. The original helper handles precise byte-level invalidation.

Cached blocks validate the VM flag, write-observer presence, RAM pointer/size
and code-bitmap pointer before executing embedded host pointers. Segment bases
are loaded dynamically. `nativeMemoryCalls` counts slow helper entries; unit
tests verify zero calls for hot flat and paged loops, with exactly two initial
paged calls for TLB fill and dirty-bit update.

The native emitter caches the RAM base in callee-saved x28, and the bitmap base
in an unused guest GPR slot when available. Save/restore masks include these
host-only assignments; guest register state is untouched. x29 anchors a valid
host frame record. TLB pointer calculations use CPU-relative ADD instructions.

REP string faults now retain completed elements rather than rolling back the
entire repetition. SI/DI/CX and the LODS accumulator retain their progress; the
fault IP returns to the prefixed instruction. Repeated CMPS/SCAS restore the
original flags on a fault, including when the host has yielded between bounded
4096-element batches. An internal continuation record carries that flag state
across host yields and is canceled on interrupts or TLB/reset transitions.
The Intel SDM repeat-prefix description is downloaded in `docs/intel-sdm.pdf`
and `.txt` from https://cdrdv2-public.intel.com/835781/325462-sdm-vol-1-2abcd-3abcd-4.pdf .
Tests exercise forward/backward, byte/word/dword, 16/32-bit addressing, source
and destination faults, repaired-page resume, I/O strings and multi-batch
comparisons under both REPE and REPNE.

## Restorable snapshots and compressed disks

`Machine::saveSnapshot` pauses at the control-loop instruction boundary and
serializes explicit little-endian fields. The versioned state includes CPU
registers/descriptors/control/debug state, RAM, software TLB entries, REP
continuation, VGA planes/registers/DAC/latches, PICs, PITs, keyboard controller
and queues, ATA task-file/buffer/progress, firmware source, ports and timers.
Zero RAM pages are omitted; the payload uses zstd level 1 and an FNV-1a
integrity check. RTC time is rebased to the saved instant on restore.

Restore validates the complete state and disk dependencies before replacing
the live machine. Host callbacks are rebound, diagnostic observers remain
attached, and a monotonic execution epoch prevents stale JIT code even for
callers retaining a JIT object. Native code and raw host pointers are excluded.
The CLI can restore live or start using `--load-snapshot`; instruction budgets
are relative to the restored counter. `--state-json` avoids full RAM dumps
during repeated task probes.

The original `PCEMUCOW` version-1 disk format uses an immutable parent and
zstd-compressed 4096-byte records. Header fields identify the logical sector
count and parent path; records include block index, packed/raw lengths and a
content/index checksum. Opening rebuilds the latest-block index. Identical
writes add nothing. Obsolete records compact once the log exceeds 8 MiB and
more than half its record bytes are obsolete. Raw disks remain supported.
Both BIOS disk services and ATA now use the same disk read/write interface.

The first snapshot seals an APFS-cloned backing image and directs subsequent
live writes into compressed deltas. Checkpoints of overlays clone their compact
record files; restores create empty child overlays instead of copying full
disks. Parents are immutable and shared, so keep dependent checkpoint
directories together. Parent paths currently use absolute local paths; this
version targets local regression checkpoints rather than relocatable bundles.
Save is an atomic directory publication. Corrupt state is rejected without
rewinding the live VM; malformed block data produces a disk-read error.
Snapshots are taken synchronously, and concurrent writers to the same disk
remain unsupported.

COW index reconstruction scans record headers through a constructor-scoped,
read-only file mapping. It bounds-checks headers and lengths before advancing,
and retains the existing read-time compressed-payload checksum checks. This
avoids one seek/read syscall per record without an expanded disk allocation;
the mapping is released before mutable disk operations begin.

### NE2000 and host socket gateway

`src/ne2000.cpp` implements an original ISA DP8390/NE2000 at 0300–031F, IRQ10,
with station PROM, remote DMA, packet rings, filtering and interrupts.
`src/network.cpp` implements ARP/DHCP and IPv4 TCP/UDP forwarding through
nonblocking host sockets, polled outside CPU batches. No external stack is used.
Snapshot version 4 stores card registers, 32 KiB card address space and bounded
wire queue. Host sockets close on restore/reset; stale TCP receives RST.
Versions 1–3 still load with a reset card. See [networking](networking.md).
