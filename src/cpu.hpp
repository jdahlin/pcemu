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
    bool halted = false, a20 = true;
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
    uint32_t physical(uint32_t linear, bool write = false);
    void flushTLB();
    uint32_t read(uint32_t linear, int width);
    uint32_t systemRead(uint32_t linear, int width);
    void write(uint32_t linear, uint32_t value, int width);
    uint32_t get(int reg, int width) const;
    void set(int reg, uint32_t value, int width);
    uint32_t address(int seg, uint32_t offset) const;
    void segment(int seg, uint16_t selector);
    uint32_t fetch(int width);
    void push(uint32_t v, int width);
    uint32_t pop(int width);
    bool condition(int cc) const;
    uint32_t alu(int op, uint32_t a, uint32_t b, int width);
    void interrupt(uint8_t n, bool software = false);
    unsigned cpl() const {
        return (flags & 0x20000) ? 3 : (cr[0] & 1) ? (s[CS].selector & 3) : 0;
    }
    void farTransfer(uint16_t selector, uint32_t offset, bool call, int width);
    void farReturn(int width, uint16_t extra, bool iret);
    void step();
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
    void stepImpl();
    void ioPermission(uint16_t port, int width);
    bool descriptorVisible(uint16_t selector, uint32_t high) const;
    struct Operand {
        bool reg;
        uint8_t index;
        uint32_t linear;
    };
    Operand decode(uint8_t modrm, bool address32, int overrideSeg);
    uint32_t load(Operand o, int w);
    void store(Operand o, uint32_t v, int w);
    uint32_t shift(int op, uint32_t value, unsigned count, int w);
};
} // namespace pc
