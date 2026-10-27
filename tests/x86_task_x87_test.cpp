// RAM-only architectural checks for the 386 task/387 subset. Instruction bytes,
// descriptor encodings, TSS offsets and expected memory images are intentionally
// literal, independent of implementation constants. References and scope:
// Intel 253668-085US, 9.2-9.4; 253665-081US, 8.1; 325383-085US, x87 entries.
#include "cpu.hpp"
#include "jit.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>

using namespace pc;
static unsigned checks;
static void check(bool condition, const char *message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
// Install one instruction at zero; ordinary CPU fault delivery remains enabled.
static void execute(CPU &cpu, std::initializer_list<uint8_t> bytes) {
    cpu.ip = 0;
    for (auto byte : bytes) cpu.write(cpu.ip++, byte, 1);
    cpu.ip = 0;
    cpu.step();
}
// Isolate pre-commit selector faults from IDT exception-handler setup.
template<class F> static void expect_fault(F operation, unsigned vector, unsigned error) {
    bool caught = false;
    try { operation(); }
    catch (const Fault &fault) {
        caught = true;
        check(fault.vector == vector && fault.error == error && fault.hasError,
              "task rejection reports the architectural vector and selector error");
    }
    check(caught, "invalid task transfer must fault");
}
// Create two flat 32-bit tasks and one task gate using fixed GDT/TSS bytes.
static std::unique_ptr<CPU> task_fixture() {
    auto cpu = std::make_unique<CPU>(2 * 1024 * 1024);
    auto &c = *cpu;
    c.cr[0] = 1; c.gdtBase = 0x1000; c.gdtLimit = 0x47;
    c.write(0x1008, 0xffff, 4); c.write(0x100c, 0x00cf9a00, 4); // CS 08.
    c.write(0x1010, 0xffff, 4); c.write(0x1014, 0x00cf9200, 4); // DS 10.
    c.write(0x1018, 0x30000067, 4); c.write(0x101c, 0x00008b00, 4); // Busy TSS 18.
    c.write(0x1020, 0x40000067, 4); c.write(0x1024, 0x00008900, 4); // Available TSS 20.
    c.write(0x1028, 0x00200000, 4); c.write(0x102c, 0x00008500, 4); // Gate 28 -> TSS 20.
    c.trSelector = 0x18; c.trBase = 0x3000; c.trLimit = 0x67; c.tr32 = true;
    for (int n = 0; n < 6; ++n) c.segment(n, n == CS ? 8 : 16);
    c.ip = 0x600; c.flags = 0x202;
    for (int n = 0; n < 8; ++n) {
        c.r[n] = 0x7000 + n * 0x100;
        c.write(0x4028 + n * 4, 0x9000 + n * 0x100, 4);
    }
    c.write(0x4020, 0x800, 4); c.write(0x4024, 0x246, 4);
    for (int n = 0; n < 6; ++n) c.write(0x4048 + n * 4, n == CS ? 8 : 16, 2);
    return cpu;
}
// Check task-link tables through decoded CALL/JMP and the actual NT IRET path.
static void task_transfers() {
    for (bool nested : {false, true}) {
        auto owner = task_fixture(); auto &c = *owner;
        c.dr[7] = 0xffff;
        c.write(0x4000, 0xabcd, 2); // JMP must not overwrite an existing backlink.
        c.farTransfer(0x28, 0xdeadbeef, nested, 4);
        check(c.trSelector == 0x20 && c.trBase == 0x4000 && c.trLimit == 0x67 && c.tr32,
              "task gate resolves its TSS selector rather than the far offset");
        check(c.ip == 0x800 && (c.flags & 0x4000) == (nested ? 0x4000 : 0),
              "CALL sets incoming NT; JMP preserves clear incoming NT");
        for (int n = 0; n < 8; ++n) {
            check(c.r[n] == unsigned(0x9000 + n * 0x100), "task loads every GPR including ESP");
            check(c.read(0x3028 + n * 4, 4) == unsigned(0x7000 + n * 0x100), "task saves every outgoing GPR");
        }
        for (int n = 0; n < 6; ++n)
            check(c.s[n].selector == (n == CS ? 8 : 16), "task reloads all six segment caches");
        check(c.read(0x3020, 4) == 0x600 && c.read(0x3024, 4) == 0x202,
              "suspended EIP and EFLAGS are saved at their TSS byte offsets");
        check(bool(c.read(0x101c, 4) & 0x200) == nested && (c.read(0x1024, 4) & 0x200),
              "CALL keeps both tasks busy; JMP releases the previous task");
        check(c.read(0x4000, 2) == (nested ? 0x18 : 0xabcd), "only nested entry writes the backlink");
        check((c.cr[0] & 8) && c.dr[7] == 0xffaa, "task switch sets TS and clears only local breakpoint enables");
        if (nested) {
            c.write(0x800, 0xcf, 1); // Operand-size-32 IRET in the incoming code segment.
            Jit jit(c, true, 0);
            jit.run(); // A native NT return must decline to the task-return interpreter.
            check(c.trSelector == 0x18 && c.ip == 0x600 && c.r[SP] == 0x7400,
                  "NT IRET follows the backlink without reading an interrupt stack frame");
            check(!(c.read(0x4024, 4) & 0x4000) && !(c.read(0x1024, 4) & 0x200) &&
                  (c.read(0x101c, 4) & 0x200), "IRET saves NT clear and frees only the departing TSS");
        }
    }
    // Loading an LDT must precede resolving the incoming selectors that use it.
    auto owner = task_fixture(); auto &c = *owner;
    c.write(0x1030, 0x50000017, 4); c.write(0x1034, 0x00008200, 4); // LDT 30.
    c.write(0x5008, 0xffff, 4); c.write(0x500c, 0x00cf9a00, 4);
    c.write(0x5010, 0xffff, 4); c.write(0x5014, 0x00cf9200, 4);
    c.write(0x4060, 0x30, 2);
    for (int n = 0; n < 6; ++n) c.write(0x4048 + n * 4, n == CS ? 0x0c : 0x14, 2);
    c.switch_task(0x20, false);
    check(c.ldtSelector == 0x30 && c.ldtBase == 0x5000 && c.ldtLimit == 0x17 &&
          c.s[CS].selector == 0x0c && c.s[DS].selector == 0x14,
          "incoming task resolves code and data selectors through its new LDT");
}
// Before commitment, malformed TSS selectors/descriptors must leave the task live.
static void task_rejections() {
    for (unsigned selector : {0u, 4u, 0x24u, 0x48u}) {
        auto c = task_fixture();
        expect_fault([&] { c->switch_task(selector, false); }, 13, selector & ~3u);
        check(c->trSelector == 0x18 && c->ip == 0x600 && c->read(0x3020, 4) == 0,
              "invalid TSS selector is rejected before saving or replacing the old task");
    }
    for (auto high : {0x00008b00u, 0x00009900u, 0x00000900u}) {
        auto c = task_fixture(); c->write(0x1024, high, 4);
        expect_fault([&] { c->switch_task(0x20, false); }, high == 0x900 ? 11 : 13, 0x20);
        check(c->trSelector == 0x18 && c->flags == 0x202 && !(c->cr[0] & 8),
              "busy, non-system and nonpresent descriptors cannot start a task");
    }
    {
        auto c = task_fixture(); c->write(0x1020, 0x40000066, 4);
        expect_fault([&] { c->switch_task(0x20, false); }, 10, 0x20);
        check(c->trSelector == 0x18, "32-bit TSS limit 66h is one byte too short");
    }
    {
        auto c = task_fixture();
        expect_fault([&] { c->switch_task(0x20, false, true); }, 10, 0x20);
        expect_fault([&] { c->farTransfer(0x2b, 0, false, 4); }, 13, 0x28);
        c->write(0x102c, 0x500, 4);
        expect_fault([&] { c->farTransfer(0x28, 0, false, 4); }, 11, 0x28);
    }
}
// Incoming task bytes are fetched as supervisor data through the old CR3;
// translation caches must then follow the newly loaded page directory.
static void task_paging() {
    auto owner = task_fixture(); auto &c = *owner;
    c.write(0x10000, 0x11007, 4); c.write(0x12000, 0x13007, 4);
    for (unsigned n = 0; n < 16; ++n) {
        c.write(0x11000 + n * 4, n * 4096 + 7, 4);
        c.write(0x13000 + n * 4, n * 4096 + 7, 4);
    }
    c.write(0x13020, 0x15007, 4); // Linear 8000h changes physical backing.
    c.write(0x8000, 0x33, 1); c.write(0x15000, 0x77, 1);
    c.write(0x401c, 0x12000, 4);
    c.cr[0] |= 0x80000000u; c.cr[3] = 0x10000;
    check(c.read(0x8000, 1) == 0x33, "prime outgoing task translation");
    c.switch_task(0x20, false);
    check(c.cr[3] == 0x12000 && c.read(0x8000, 1) == 0x77,
          "new task CR3 replaces a warm translation from the previous address space");
    owner = task_fixture(); auto &bad = *owner;
    bad.write(0x10000, 0x11007, 4);
    for (unsigned n = 0; n < 4; ++n) bad.write(0x11000 + n * 4, n * 4096 + 7, 4);
    bad.cr[0] |= 0x80000000u; bad.cr[3] = 0x10000;
    expect_fault([&] { bad.switch_task(0x20, false); }, 14, 0);
    check(bad.cr[2] == 0x401c && bad.trSelector == 0x18 && bad.cr[3] == 0x10000 &&
          bad.ip == 0x600 && bad.read(0x3020, 4) == 0,
          "incoming TSS page fault precedes old-state save and task replacement");
}
// Stack mapping is circular, but FTW encodes physical registers, not ST indices.
static void stack_and_comparisons() {
    CPU c(2 * 1024 * 1024); c.s[CS].big = true;
    for (unsigned n = 0; n < 8; ++n) execute(c, {0xd9, 0xe8});
    check(c.fpTag == 0 && (c.fpStatus & 0x3800) == 0, "eight pushes wrap TOP and fill physical tags");
    for (unsigned n = 0; n < 8; ++n) execute(c, {0xdd, 0xd8});
    check(c.fpTag == 0xffff && (c.fpStatus & 0x3800) == 0, "eight pops wrap TOP and empty all physical tags");
    execute(c, {0xd9, 0xee});
    check(c.fpTag == 0x7fff && (c.fpStatus & 0x3800) == 0x3800, "FLDZ marks physical R7 zero after decrementing TOP");
    execute(c, {0xd9, 0xe8}); execute(c, {0xd9, 0xc9}); // FLD1; FXCH ST1.
    check(std::bit_cast<double>(c.fp[6]) == 0 && std::bit_cast<double>(c.fp[7]) == 1 && c.fpTag == 0x1fff,
          "FXCH exchanges values and their physical zero/valid tags");
    execute(c, {0xdd, 0xc1});
    check((c.fpTag & 0xc000) == 0xc000 && (c.fpStatus & 0x3800) == 0x3000,
          "FFREE changes a physical tag without popping TOP");
    struct Compare { double left, right; unsigned condition; };
    for (auto t : {Compare{1,2,0x100}, Compare{2,1,0}, Compare{1,1,0x4000},
                   Compare{std::numeric_limits<double>::quiet_NaN(),1,0x4500}}) {
        c.fpStatus = 0x21; c.fpTag = 0xfff0;
        c.fp[0] = std::bit_cast<uint64_t>(t.left); c.fp[1] = std::bit_cast<uint64_t>(t.right);
        execute(c, {0xdd, 0xe1}); // FUCOM ST1 (quiet NaN does not require an invalid exception).
        check((c.fpStatus & 0x4500) == t.condition && (c.fpStatus & 0x21) == 0x21,
              "FUCOM sets comparison codes while preserving prior exception flags");
        execute(c, {0xdb, 0xe2});
        check((c.fpStatus & 0x80ff) == 0 && (c.fpStatus & 0x4500) == t.condition,
              "FNCLEX clears sticky exception state but retains condition codes");
    }
}
// Distinguish all noncommutative register forms with unequal operands.
static void register_arithmetic() {
    const unsigned groups[] = {0,1,4,5,6,7};
    const double st0[] = {13,30,7,-7,10./3,3./10};
    const double sti[] = {13,30,7,-7,10./3,3./10};
    for (uint8_t opcode : {0xd8, 0xdc, 0xde}) for (unsigned n = 0; n < 6; ++n) {
        CPU c(2 * 1024 * 1024); c.s[CS].big = true;
        c.fpTag = 0xfff0; c.fp[0] = std::bit_cast<uint64_t>(10.); c.fp[1] = std::bit_cast<uint64_t>(3.);
        execute(c, {opcode, uint8_t(0xc1 | (groups[n] << 3))});
        check(std::bit_cast<double>(c.fp[opcode == 0xd8 ? 0 : 1]) == (opcode == 0xd8 ? st0[n] : sti[n]),
              "D8/DC/DE arithmetic selects the correct source and destination order");
        check(((c.fpStatus >> 11) & 7) == (opcode == 0xde ? 1 : 0),
              "only the DE arithmetic forms pop the register stack");
    }
}
// Check integer-indefinite stores at all three widths, including NaN/infinity.
static void integer_conversions() {
    for (unsigned width : {2u,4u,8u}) for (double x : {std::numeric_limits<double>::infinity(),
         std::numeric_limits<double>::quiet_NaN(), std::ldexp(1., int(width * 8 - 1))}) {
        CPU c(2 * 1024 * 1024); c.s[CS].big = true;
        c.fpTag = 0xfffc; c.fp[0] = std::bit_cast<uint64_t>(x);
        execute(c, {uint8_t(width == 4 ? 0xdb : 0xdf), uint8_t(width == 8 ? 0x3d : 0x1d),0,2,0,0});
        uint64_t result = c.read(0x200, width == 8 ? 4 : width);
        if (width == 8) result |= uint64_t(c.read(0x204, 4)) << 32;
        check(result == (uint64_t(1) << (width * 8 - 1)) && (c.fpStatus & 1),
              "masked invalid FISTP sets IE and stores signed integer indefinite");
        check(c.fpTag == 0xffff && (c.fpStatus & 0x3800) == 0x800, "completed invalid FISTP still pops once");
    }
}
// Validate protected-mode memory layouts using both operand sizes and nonzero TOP.
static void environment_images() {
    for (bool wide : {false, true}) {
        CPU c(2 * 1024 * 1024); c.s[CS].big = wide; c.cr[0] = 1;
        c.fpControl = 0x77f; c.fpStatus = 0x1821; c.fpTag = 0xff3f;
        c.fp[3] = std::bit_cast<uint64_t>(-2.5);
        c.fpIP = 0x1234; c.fpCS = 0x28; c.fpData = 0x5678; c.fpDS = 0x30; c.fpOpcode = 0x5ab;
        if (wide) execute(c, {0xdd,0x35,0,2,0,0}); else execute(c, {0xdd,0x36,0,2});
        unsigned stride = wide ? 4 : 2, environment = wide ? 28 : 14;
        check(c.read(0x200,2) == 0x77f && c.read(0x200+stride,2) == 0x1821 &&
              c.read(0x200+2*stride,2) == 0xff3f, "FNSAVE stores original control/status/physical tags");
        check(c.read(0x200+3*stride,stride) == 0x1234 && c.read(0x200+4*stride,2) == 0x28 &&
              c.read(0x200+5*stride,stride) == 0x5678 && c.read(0x200+6*stride,2) == 0x30,
              "protected-mode environment packs pointer offsets and selectors at documented strides");
        check(c.read(0x200+environment,4) == 0 && c.read(0x204+environment,4) == 0xa0000000 &&
              c.read(0x208+environment,2) == 0xc000, "FSAVE first register image is logical ST0 as extended -2.5");
        check(c.fpControl == 0x37f && c.fpStatus == 0 && c.fpTag == 0xffff,
              "FNSAVE initializes only after writing the old environment");
        if (wide) execute(c, {0xdd,0x25,0,2,0,0}); else execute(c, {0xdd,0x26,0,2});
        check(c.fpControl == 0x77f && c.fpStatus == 0x1821 && c.fpTag == 0xff3f &&
              std::bit_cast<double>(c.fp[3]) == -2.5, "FRSTOR maps logical images back through restored TOP");
        c.fpControl = 0x740; // All six exception masks clear before FNSTENV.
        if (wide) execute(c, {0xd9,0x35,0,3,0,0}); else execute(c, {0xd9,0x36,0,3});
        check(c.read(0x300,2) == 0x740 && c.fpControl == 0x77f && c.fpStatus == 0x1821,
              "FNSTENV stores old masks and retains register-stack state");
    }
}
// An ordinary cross-page operand fault must restart before popping, resetting
// FPU state, or partially overwriting memory. The observer stops before IDT entry.
static void operand_faults() {
    for (bool save : {false, true}) {
        CPU c(2 * 1024 * 1024); c.s[CS].big = true; c.s[CS].selector = 3;
        c.write(0x10000, 0x11007, 4); c.write(0x11000, 7, 4);
        c.cr[0] = 0x80000011; c.cr[3] = 0x10000;
        c.fpControl = 0x77f; c.fpStatus = 0; c.fpTag = 0xfffc;
        c.fp[0] = std::bit_cast<uint64_t>(2.5);
        for (unsigned n = 0xff0; n < 0x1000; ++n) c.mem[n] = 0xa5;
        bool caught = false;
        c.observeFault = [&](const Fault &fault) {
            check(fault.vector == 14 && fault.error == 6 && c.cr[2] == 0x1000,
                  "cross-page x87 store reports a user write page fault");
            caught = true;
            throw std::runtime_error("stop before fault delivery");
        };
        try {
            if (save) execute(c, {0xdd,0x35,0xf0,0x0f,0,0}); // FNSAVE spans missing page.
            else execute(c, {0xdb,0x3d,0xfc,0x0f,0,0}); // FSTP m80 spans missing page.
        } catch (const std::runtime_error &) { if (!caught) throw; }
        check(caught && c.ip == 0 && c.fpControl == 0x77f && c.fpTag == 0xfffc &&
              c.fpStatus == 0 && std::bit_cast<double>(c.fp[0]) == 2.5,
              "faulting FSTP/FNSAVE preserves IP, stack, control and status");
        for (unsigned n = 0xff0; n < 0x1000; ++n)
            check(c.mem[n] == 0xa5, "preflight prevents a partial cross-page store");
    }
    for (unsigned cr0 : {0x14u, 0x18u, 0x1cu}) {
        CPU c(2 * 1024 * 1024); c.cr[0] = cr0;
        c.fpTag = 0xfffc; c.fp[0] = std::bit_cast<uint64_t>(1.);
        bool caught = false;
        c.observeFault = [&](const Fault &fault) {
            check(fault.vector == 7 && !fault.hasError, "EM or TS generates #NM without an error code");
            caught = true; throw std::runtime_error("stop before fault delivery");
        };
        try { execute(c, {0xd9,0xe8}); } catch (const std::runtime_error &) { if (!caught) throw; }
        check(caught && c.ip == 0 && c.fpTag == 0xfffc && c.fpStatus == 0,
              "unavailable coprocessor faults before FLD changes TOP or tags");
    }
}
// External test386.asm POST 03: MOV CS,r/m16 is #UD even with a memory
// operand. The invalid encoding must not read that operand or change CS.
static void reject_mov_cs() {
    struct Stop {};
    for(bool native:{false,true})for(bool prefix:{false,true})for(uint8_t modrm:{uint8_t(0xc8),uint8_t(0x08)}) {
        CPU c;c.ip=0x100;c.s[DS].base=0xa0000;c.r[AX]=0x1234;
        unsigned at=0x100;if(prefix)c.mem[at++]=0x66;c.mem[at++]=0x8e;c.mem[at]=modrm;
        c.videoRead=[](uint32_t)->uint8_t{throw std::runtime_error("invalid MOV CS read its memory operand");};
        unsigned vector=255;c.observeFault=[&](const Fault &f){vector=f.vector;throw Stop{};};
        Jit jit(c,native,0,65536);
        try {if(native)jit.run();else c.step();}catch(const Stop &){}
        check(vector==6 && c.ip==0x100 && c.s[CS].selector==0 && c.r[AX]==0x1234,
              "MOV CS faults before source access and preserves restart state");
    }
}
// Hardware 386EX vectors include invalid LOCK register forms. Verify #UD
// priority, and retain legal memory read/modify/write execution on both paths.
static void lock_prefix_forms() {
    struct Stop {};
    const std::vector<std::vector<uint8_t>> invalid={{0xf0,0x00,0xc0},{0xf0,0x90},
        {0xf0,0x80,0x3e,0,2,1},{0xf0,0x0f,0xab,0xc0},{0xf0,0xf7,0x36,0,2}};
    for(bool native:{false,true}) {
        for(const auto &code:invalid) {
            CPU c;c.ip=0x100;c.r[AX]=0x1234;c.s[DS].base=0xa0000;
            std::copy(code.begin(),code.end(),c.mem.begin()+0x100);
            c.videoRead=[](uint32_t)->uint8_t{throw std::runtime_error("invalid LOCK read memory");};
            unsigned vector=255;c.observeFault=[&](const Fault &f){vector=f.vector;throw Stop{};};
            Jit jit(c,native,0,65536);
            try {if(native)jit.run();else c.step();}catch(const Stop &){}
            check(vector==6&&c.ip==0x100&&c.r[AX]==0x1234,"invalid LOCK form faults before operand access");
        }
        CPU c;c.ip=0x100;c.r[AX]=3;c.mem[0x200]=4;
        const uint8_t code[]={0xf0,0x01,0x06,0,2,0xf4};
        std::copy(std::begin(code),std::end(code),c.mem.begin()+0x100);
        Jit jit(c,native,0,65536);if(native)jit.run();else c.step();
        check(c.read(0x200,2)==7,"valid LOCK memory ADD executes normally");
    }
}
// DIV/IDIV overflow and zero divisors, plus AAM base zero, are restartable
// #DE faults rather than host errors. Hardware vectors exercise these paths.
static void divide_faults() {
    struct Stop {};
    struct Case { std::vector<uint8_t> code;uint32_t ax,dx,bx; };
    const std::vector<Case> cases={{{0xf6,0xf3},0x1234,0,0},{{0xf6,0xf3},0x100,0,1},
        {{0xf7,0xf3},0,1,1},{{0xf7,0xfb},0x8000,0xffff,0xffff},
        {{0x66,0xf7,0xfb},0x80000000,0xffffffff,0xffffffff},{{0xd4,0},0x1234,0,0}};
    for(bool native:{false,true})for(const auto &test:cases) {
        CPU c;c.ip=0x100;c.r[AX]=test.ax;c.r[DX]=test.dx;c.r[BX]=test.bx;
        std::copy(test.code.begin(),test.code.end(),c.mem.begin()+0x100);
        unsigned vector=255;c.observeFault=[&](const Fault &f){vector=f.vector;throw Stop{};};
        Jit jit(c,native,0,65536);
        try {if(native)jit.run();else c.step();}catch(const Stop &){}
        check(vector==0&&c.ip==0x100&&c.r[AX]==test.ax&&c.r[DX]==test.dx,
              "divide fault preserves dividend and restart IP");
    }
}
// Real-mode full-width bounds precede reads/writes, including native memory
// accesses. Intel 80386 PRM 14.7, Table 14-2; LEA only computes an offset.
static void real_segment_bounds() {
    struct Stop {};
    struct Case { std::vector<uint8_t> code; unsigned segment,width,offset,vector; };
    const std::vector<Case> cases={
        {{0x67,0x01,0x03},DS,2,0x10000,13},
        {{0x01,0x07},DS,2,0xffff,13},
        {{0x66,0x01,0x07},DS,4,0xfffd,13},
        {{0x36,0x01,0x07},SS,2,0xffff,12},
        {{0x67,0x88,0x03},DS,1,0xffffffff,13},
        {{0xa1,0xff,0xff},DS,2,0xffff,13},
        {{0xc5,0x07},DS,4,0xfffd,13},
        {{0x36,0x0f,0xb5,0x07},SS,4,0xfffd,12}};
    for(bool native:{false,true})for(const auto &test:cases) {
        CPU c;c.ip=0x100;c.r[BX]=test.offset;c.r[AX]=0x1234;c.r[SP]=0x8000;
        c.s[test.segment].base=0xa0000;
        std::copy(test.code.begin(),test.code.end(),c.mem.begin()+0x100);
        c.videoRead=[](uint32_t)->uint8_t{throw std::runtime_error("faulting segment read device");};
        c.videoWrite=[](uint32_t,uint8_t){throw std::runtime_error("faulting segment wrote device");};
        unsigned vector=255;c.observeFault=[&](const Fault &f){vector=f.vector;throw Stop{};};
        Jit jit(c,native,0,65536);
        try {if(native)jit.run();else c.step();}catch(const Stop &){}
        check(vector==test.vector && c.ip==0x100 && c.r[AX]==0x1234 && c.r[SP]==0x8000,
              "segment bounds fault before memory side effects and preserve restart state");
    }
    for(bool native:{false,true}) {
        CPU c;c.ip=0x100;c.r[BX]=0xfffe;c.r[AX]=2;c.mem[0xfffe]=3;
        const uint8_t code[]={0x01,0x07,0xf4};std::copy(std::begin(code),std::end(code),c.mem.begin()+0x100);
        Jit jit(c,native,0,65536);if(native)jit.run();else c.step();
        check(c.read(0xfffe,2)==5,"last valid segment word executes");
        CPU lea;lea.ip=0x100;lea.r[BX]=0x12345678;
        const uint8_t address[]={0x67,0x66,0x8d,0x03,0xf4};
        std::copy(std::begin(address),std::end(address),lea.mem.begin()+0x100);
        Jit lj(lea,native,0,65536);if(native)lj.run();else lea.step();
        check(lea.r[AX]==0x12345678,"LEA does not validate a memory access");
    }
}
// Sequential fetches past FFFFh fault instead of wrapping. Interrupt
// entry must latch its vector before an overlapping stack frame overwrites it.
static void fetch_and_interrupt_boundaries() {
    struct Stop {};
    for(bool native:{false,true}) {
        for(const auto &bytes:std::vector<std::vector<uint8_t>>{{0xb8,0,0},{0xf6,0xe0},{0x66,0xb8,0,0,0,0}}) {
            CPU c;c.ip=0xffff;c.mem[0xffff]=bytes[0];
            std::copy(bytes.begin()+1,bytes.end(),c.mem.begin());
            unsigned vector=255;c.observeFault=[&](const Fault &f){vector=f.vector;throw Stop{};};
            Jit jit(c,native,0,65536);
            try {if(native)jit.run();else c.step();}catch(const Stop &){}
            check(vector==13 && c.ip==0xffff,"instruction fetch cannot wrap within instruction");
        }
        CPU overlong;overlong.ip=0x100;
        std::fill(overlong.mem.begin()+0x100,overlong.mem.begin()+0x10e,0x66);
        overlong.mem[0x10e]=0xb8;
        unsigned vector=255;overlong.observeFault=[&](const Fault &f){vector=f.vector;throw Stop{};};
        Jit longjit(overlong,native,0,65536);
        try {if(native)longjit.run();else overlong.step();}catch(const Stop &){}
        check(vector==13 && overlong.ip==0x100,"15-byte limit includes immediates");
        CPU alias;alias.ip=0x100;alias.r[SP]=8;alias.s[CS].selector=0x6b9b;
        alias.mem[0x100]=0xf6;alias.mem[0x101]=0xf3;alias.r[BX]=0;
        alias.write(0,0x71cf0d60,4);
        Jit aj(alias,native,0,65536);if(native)aj.run();else alias.step();
        check(alias.s[CS].selector==0x71cf && alias.ip==0xd60 && alias.r[SP]==2,
              "divide handler pointer is latched before aliased interrupt stack writes");
        check(alias.read(2,2)==0x100,"aliased interrupt frame contains faulting IP");
    }
}
// Invalid memory-only encodings produce #UD. Register-indexed bit strings
// wrap their adjusted offset to address size, as verified by 386EX captures.
static void encoded_operand_rules() {
    struct Stop {};
    for(bool native:{false,true}) {
        for(const auto &bytes:std::vector<std::vector<uint8_t>>{{0x8d,0xc0},{0xc5,0xc0},{0x0f,0xb5,0xc0},{0x8f,0xc8}}) {
            CPU c;c.ip=0x100;std::copy(bytes.begin(),bytes.end(),c.mem.begin()+0x100);
            unsigned vector=255;c.observeFault=[&](const Fault &f){vector=f.vector;throw Stop{};};
            Jit jit(c,native,0,65536);
            try {if(native)jit.run();else c.step();}catch(const Stop &){}
            check(vector==6 && c.ip==0x100,"invalid memory-only form delivers restartable UD");
        }
        CPU bit;bit.ip=0x100;bit.r[BP]=0x4a8f;
        const uint8_t code[]={0x0f,0xab,0x2e,0x41,0xfa,0xf4};
        std::copy(std::begin(code),std::end(code),bit.mem.begin()+0x100);
        Jit bj(bit,native,0,65536);if(native)bj.run();else bit.step();
        check(bit.read(0x391,2)==0x8000,"16-bit bit-string offset wraps after index adjustment");
    }
}
// Count-zero address checks, whole-ring carry, BOUND and control-transfer
// limits follow the 386 instruction entries; expected values are literal.
static void scalar_boundary_rules() {
    struct Stop {};
    for(bool native:{false,true}) {
        CPU rotate;rotate.ip=0x100;rotate.r[AX]=0x81;rotate.r[CX]=8;
        const uint8_t ror[]={0xd2,0xc8,0xf4};
        std::copy(std::begin(ror),std::end(ror),rotate.mem.begin()+0x100);
        Jit rj(rotate,native,0,65536);if(native)rj.run();else rotate.step();
        check(rotate.r[AX]==0x81 && (rotate.flags&CF),"whole-ring ROR updates carry without changing data");
        for(unsigned kind=0;kind<3;++kind) {
            CPU c;c.ip=0x100;c.r[SP]=0x8000;c.r[BX]=0xffff;c.r[AX]=10;
            std::vector<uint8_t> bytes;
            if(kind==0)bytes={0x0f,0xa4,0x07,0}; // SHLD word [BX],AX,0.
            if(kind==1){bytes={0x66,0xc3};c.write(0x8000,0x10000,4);}
            if(kind==2){bytes={0x62,0x06,0,2};c.write(0x200,0,2);c.write(0x202,9,2);}
            std::copy(bytes.begin(),bytes.end(),c.mem.begin()+0x100);
            unsigned vector=255;c.observeFault=[&](const Fault &f){vector=f.vector;throw Stop{};};
            Jit j(c,native,0,65536);
            try {if(native)j.run();else c.step();}catch(const Stop &){}
            check(vector==(kind==2?5:13) && c.ip==0x100 && c.r[SP]==0x8000,
                  "zero-count span, RET target and BOUND faults retain restart state");
        }
        CPU push;push.ip=0x100;push.r[SP]=0x8000;push.flags=0xfffc0246;
        const uint8_t pf[]={0x66,0x9c,0xf4};std::copy(std::begin(pf),std::end(pf),push.mem.begin()+0x100);
        Jit pj(push,native,0,65536);if(native)pj.run();else push.step();
        check(push.read(0x7ffc,4)==0x246,"386 PUSHFD clears reserved high bits and RF/VM");
    }
}
int main() {
    try {
        reject_mov_cs(); lock_prefix_forms(); divide_faults(); real_segment_bounds(); fetch_and_interrupt_boundaries(); encoded_operand_rules(); scalar_boundary_rules();
        task_transfers(); task_rejections(); task_paging(); stack_and_comparisons();
        register_arithmetic(); integer_conversions(); environment_images(); operand_faults();
        std::cout << "Passed " << checks << " 386 task/x87 specification checks\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
