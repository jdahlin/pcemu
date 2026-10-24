#include "jit.hpp"
#include "jit_ir.hpp"
#include "profile.hpp"
#include <utility>
#if defined(__APPLE__) && defined(__aarch64__)
#include <sys/mman.h>
#endif
namespace pc {
using jit_detail::Kind;
Jit::Jit(CPU &c, bool en, unsigned heat, size_t arenaBytes, BlockMode mode)
    : cpu(c), capacity(arenaBytes), warmup(heat), blockMode(mode) {
    if (capacity < 65536)
        throw std::invalid_argument("JIT arena must be at least 64 KiB");
#if defined(__APPLE__) && defined(__aarch64__)
    if (en) {
        arena = static_cast<uint8_t *>(mmap(nullptr, capacity, PROT_READ | PROT_WRITE | PROT_EXEC,
                                            MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0));
        if (arena == MAP_FAILED) {
            arena = nullptr;
            throw std::runtime_error("MAP_JIT failed");
        }
        active = true;
    }
#else
    (void)en;
#endif
}
Jit::~Jit() {
#if defined(__APPLE__) && defined(__aarch64__)
    if (arena)
        munmap(arena, capacity);
#endif
}
void Jit::clear() {
    blocks.clear();
    fast = {};
    used = 0;
    nativeInstructions = compiledBlocks = cacheHits = cacheRecycles = 0;
}
uint32_t Jit::run() {
    profile::event(profile::Dispatch);
    if (cpu.halted)
        return 0;
    if (!active ||
        (cpu.interruptBlockedThrough && cpu.instructions <= cpu.interruptBlockedThrough) ||
        (cpu.flags & TF)) {
        profile::event(profile::InterpretedDispatch);
        cpu.step();
        return 1;
    }
    // Include segment base and mode: real-mode aliases may share linear addresses
    // but have different IPs and operand sizes. Paged entries validate translation
    // and access permission before using cached native code.
    uint64_t key = uint64_t(cpu.s[CS].selector) << 48 | uint64_t(cpu.s[CS].big) << 47 | cpu.ip;
    auto &entry = fast[(cpu.ip ^ (cpu.ip >> 12) ^ (cpu.s[CS].selector * 31u)) & 4095];
    Block *cached = entry.key == key ? entry.block : nullptr;
    if (cached)
        profile::event(profile::FastHit);
    if (!cached) {
        auto it = blocks.find(key);
        if (it != blocks.end()) {
            cached = &it->second;
            profile::event(profile::MapHit);
        } else
            profile::event(profile::LookupMiss);
    }
    uint32_t currentPage = 0;
    bool checkedPage = false;
    if ((cpu.cr[0] & 0x80000000u) && cached && (cached->fn || cached->tlbEpoch != cpu.tlbEpoch)) {
        try {
            currentPage = cpu.physical(cpu.address(CS, cpu.ip)) >> 12;
            checkedPage = true;
        } catch (const Fault &) {
            cpu.step(); // Deliver #PF with the interpreter's restartable frame.
            return 1;
        }
    }
    if (!cached || cached->executionEpoch != cpu.executionEpoch ||
        cached->page >= cpu.generation.size() ||
        cached->generation != cpu.generation[cached->page] || cached->cr0 != cpu.cr[0] ||
        cached->cr3 != cpu.cr[3] || cached->csBase != cpu.s[CS].base || cached->a20 != cpu.a20 ||
        cached->vm != bool(cpu.flags & 0x20000) ||
        // These guards protect addresses and assumptions embedded in native
        // code. Negative and warming entries execute CPU::step(), which reads
        // the current buffers, watch callback and stack mode itself. Keep code
        // identity checks above immediate so changed code can be translated.
        (cached->fn &&
         (cached->watched != bool(cpu.watchWrite) ||
          (cached->usesStack && cached->stack32 != cpu.s[SS].big) ||
          cached->ram != cpu.mem.data() || cached->ramSize != cpu.mem.size() ||
          cached->codeBits != cpu.codeUsed.data())) ||
        (checkedPage && cached->page != currentPage)) {
        try {
            profile::event(profile::MetadataRebuild);
            auto metadata = compile(false);
            cached = &blocks.insert_or_assign(key, metadata).first->second;
        } catch (const Fault &) {
            cpu.step();
            return 1;
        }
    }
    // A TLB flush need not discard decoded code when fetch permissions were
    // rechecked and the same physical instruction page is still mapped.
    cached->tlbEpoch = cpu.tlbEpoch;
    if (!cached->attempted && ++cached->visits >= warmup) {
        try {
            profile::event(profile::Translate);
            auto translated = compile();
            // Compilation can recycle the arena and clear both lookup tables.
            cached = &blocks.insert_or_assign(key, translated).first->second;
        } catch (const Fault &) {
            cpu.step();
            return 1;
        }
    }
    entry = {key, cached};
    auto &b = *cached;
    if (!b.fn) {
        profile::event(profile::InterpretedDispatch);
        if (b.attempted)
            profile::event(profile::NegativeHit);
        cpu.step();
        return 1;
    }
    cacheHits++;
    profile::event(profile::NativeCall);
    cpu.nativeFallback = false;
    auto n = b.fn(&cpu);
    cpu.instructions += n;
    nativeInstructions += n;
    if (cpu.nativeFallback) {
        profile::event(profile::NativeReplay);
        cpu.nativeFallback = false;
        cpu.step();
        return n + 1;
    }
    return n;
}
Jit::Block Jit::compile(bool translate) {
    Block block;
    auto linear = cpu.address(CS, cpu.ip), physical = cpu.physical(linear);
    block.page = physical >> 12;
    block.generation = cpu.generation.at(block.page);
    block.cr0 = cpu.cr[0];
    block.cr3 = cpu.cr[3];
    block.csBase = cpu.s[CS].base;
    block.a20 = cpu.a20;
    block.tlbEpoch = cpu.tlbEpoch;
    block.executionEpoch = cpu.executionEpoch;
    block.vm = cpu.flags & 0x20000;
    block.watched = bool(cpu.watchWrite);
    block.stack32 = cpu.s[SS].big;
    block.ram = cpu.mem.data();
    block.ramSize = cpu.mem.size();
    block.codeBits = cpu.codeUsed.data();
    if (!translate)
        return block;
    block.attempted = true;
#if defined(__APPLE__) && defined(__aarch64__)
    auto decoded = decode(block);
    const auto &ir = decoded.instructions;
    // Native entry saves and restores the guest register set. Very short
    // blocks cost more than interpreting them unless explicitly testing lowering.
    const bool shortComparison =
        ir.size() == 2 && ir[1].kind == Kind::Branch && ir[1].dst >= 0 &&
        (ir[0].memoryAlu || ir[0].kind == Kind::Test ||
         (ir[0].kind == Kind::Alu && (ir[0].op == 7 || ir[0].op == 8 || ir[0].op == 9)));
    if (ir.empty() || (warmup && ir.size() < 3 && !shortComparison &&
                      ir.back().kind != Kind::CounterBranch && !ir.back().nearCall &&
                      !ir.back().nearReturn))
        return block;
    return emitArm64(std::move(block), decoded);
#endif
    return block;
}
} // namespace pc
