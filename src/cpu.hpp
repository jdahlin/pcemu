#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace pc {
struct Fault {
    uint8_t vector;
    uint32_t error;
    bool hasError;
    bool partialRepeat = false; // Successful REP elements remain committed on a later fault.
    uint32_t repeatFlags = 0;
};
enum Reg { AX, CX, DX, BX, SP, BP, SI, DI };
enum Seg { ES, CS, SS, DS, FS, GS };
enum Flag : uint32_t {
    CF = 1,
    PF = 4,
    AF = 16,
    ZF = 64,
    SF = 128,
    TF = 256,
    IF = 512,
    DF = 1024,
    OF = 2048
};
struct Segment {
    uint32_t base = 0, limit = 0xffff;
    uint16_t selector = 0;
    bool big = false, unusable = false;
};
// Shared descriptor interpretation; callers choose how descriptor bytes are read.
Segment decodeSegmentDescriptor(int segment, uint16_t selector, uint32_t low, uint32_t high);
struct CPU {
    // Fixed layout is also the AArch64 JIT ABI. No host pointers in guest registers.
    uint32_t r[8]{};
    uint32_t ip = 0, flags = 2;
    Segment s[6]{};
    uint32_t cr[4]{0x10, 0, 0, 0};
    uint32_t dr[8]{0, 0, 0, 0, 0, 0, 0xffff0ff0, 0x400};
    uint32_t gdtBase = 0, idtBase = 0, ldtBase = 0;
    uint16_t gdtLimit = 0, idtLimit = 0x3ff, ldtLimit = 0, ldtSelector = 0, trSelector = 0;
    uint32_t trBase = 0, trLimit = 0;
    bool tr32 = false;
    // 387 register stack: fp[] holds physical R0..R7 as binary64 bit patterns;
    // fpStatus.TOP maps logical ST(i) to R[(TOP+i)&7]. fpTag has physical tags.
    // IMPLEMENTATION NOTE: binary64 is the current arithmetic backend, not an
    // 80-bit register representation. See cpu_x87.cpp for conformance gaps.
    // Intel 253665-081US, 8.1.2/8.1.3/8.1.7 (September 2023):
    // https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf#page=203
    uint64_t fp[8]{};
    uint16_t fpControl = 0x37f, fpStatus = 0, fpTag = 0xffff;
    uint32_t fpIP = 0, fpData = 0;
    uint16_t fpCS = 0, fpDS = 0, fpOpcode = 0;
    bool halted = false, a20 = true;
    // CR0.PE entry starts at CPL0 while CS still contains a real-mode value.
    // Its low bits become privilege bits only after a protected CS reload.
    bool protectedEntry = false;
    bool nativeFallback = false;
    uint32_t nativeValue = 0;
    uint64_t nativeMemoryCalls = 0; // Diagnostic: slow RAM guards entered by native code.
    uint64_t executionEpoch = 0;    // Host cache identity; never rewound by snapshot restore.
    uint64_t instructions = 0;
    uint64_t interruptBlockedThrough = 0;
    unsigned systemAccess = 0;
    uint32_t watchStart = 0, watchLength = 0;
    std::function<void(uint32_t, uint32_t, int)> watchWrite;
    // Diagnostic observer receives the restarted instruction state before the
    // guest exception handler runs. It must not change architectural state.
    std::function<void(const Fault &)> observeFault;
    std::vector<uint8_t> mem;
    std::vector<uint32_t> generation;
    std::vector<uint64_t> codeUsed;
    void markCode(uint32_t physical);
    void written(uint32_t physical, unsigned width);
    std::function<bool(uint8_t)> bios;
    std::function<uint8_t(uint32_t)> videoRead;
    std::function<void(uint32_t, uint8_t)> videoWrite;
    std::function<uint32_t(uint16_t, int)> input;
    std::function<void(uint16_t, uint32_t, int)> output;
    explicit CPU(size_t ram = 16 * 1024 * 1024);
    // Keep cached translation out of the already large interpreter loop.
    [[gnu::noinline]] uint32_t physical(uint32_t linear, bool write = false);
    void flushTLB();
    uint32_t read(uint32_t linear, int width);
    uint32_t systemRead(uint32_t linear, int width);
    void write(uint32_t linear, uint32_t value, int width);
    uint32_t get(int reg, int width) const;
    void set(int reg, uint32_t value, int width);
    uint32_t address(int seg, uint32_t offset, unsigned width = 1) const;
    void segment(int seg, uint16_t selector);
    uint32_t fetch(int width);
    // Compiled pre-access #UD: registers/flags/IP already hold restart state.
    void stepInvalidOpcode();
    void push(uint32_t v, int width);
    uint32_t pop(int width);
    bool condition(int cc) const;
    uint32_t alu(int op, uint32_t a, uint32_t b, int width);
    void interrupt(uint8_t n, bool software = false);
    unsigned cpl() const {
        return (flags & 0x20000) ? 3 : (cr[0] & 1) ? (protectedEntry ? 0 : s[CS].selector & 3) : 0;
    }
    void farTransfer(uint16_t selector, uint32_t offset, bool call, int width);
    void farReturn(int width, uint16_t extra, bool iret);
    // Load an available 32-bit TSS, or a busy TSS for nested IRET. Saves the
    // outgoing dynamic state, updates busy/backlink/NT/TS, reloads descriptors
    // and invalidates translations. Caller checks the transfer gate's DPL.
    // Early Faults precede task replacement; late-fault sequencing is incomplete.
    // Intel 253668-085US, 9.3/9.4:
    // https://cdrdv2-public.intel.com/835754/253668-sdm-vol-3a.pdf#page=274
    void switch_task(uint16_t selector, bool nested, bool returning = false);
    // Decoded x87 operation shared by translated execution and the reference
    // decoder. Address calculation remains dynamic; no guest byte fetch occurs.
    struct DecodedX87 {
        uint8_t opcode=0, modrm=0;
        int base=-1,index=-1,scale=0,segment=DS;
        uint32_t displacement=0,end=0;
        bool address32=false,operand32=false;
    };
    void stepX87(const DecodedX87 &operation);
    void step();
    // Decoded scalar I/O, invoked only after leaving generated host frames.
    void stepPortIO(uint16_t port, int width, bool output, uint32_t end);
    std::string location() const;

    struct TLBEntry {
        uint32_t tag = 0, epoch = 0, base = 0, pteAddress = 0;
        uint8_t permissions = 0;
        bool dirty = false;
    };
    std::array<TLBEntry, 1024> tlb{};
    uint32_t tlbEpoch = 1, tlbCR3 = 0xffffffffu;

    // Internal continuation across host scheduling batches, not a guest snapshot.
    struct RepeatResume {
        bool valid = false;
        uint16_t selector = 0;
        uint32_t base = 0, ip = 0, initialFlags = 0, lastFlags = 0;
        uint32_t ax = 0, cx = 0, si = 0, di = 0;
    } repeatResume;

  private:
    uint32_t physicalSlow(uint32_t linear, bool write);
    void stepImpl();
    // Execute D8..DF after prefixes/opcode fetch; decode ModR/M, enforce EM/TS,
    // update stack/environment and use ordinary guest paging for memory operands.
    // Unsupported encodings raise #UD; see the implementation's explicit limits.
    // Intel 253665-081US, 8.1:
    // https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf#page=203
    void execute_x87_instruction(uint8_t opcode, bool address32, int overrideSeg, bool operand32);
    void ioPermission(uint16_t port, int width);
    void portIO(uint16_t port, int width, bool output);
    bool descriptorVisible(uint16_t selector, uint32_t high) const;
    struct Operand {
        bool reg;
        uint8_t index;
        uint32_t linear;
        int segment = -1;
    };
    void execute_x87_decoded(uint8_t opcode, uint8_t modrm, Operand operand, bool operand32);
    Operand decode(uint8_t modrm, bool address32, int overrideSeg);
    uint32_t load(Operand o, int w);
    void store(Operand o, uint32_t v, int w);
    uint32_t shift(int op, uint32_t value, unsigned count, int w);
};
} // namespace pc
