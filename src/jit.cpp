#include "jit.hpp"
#include "jit_ir.hpp"
#include "profile.hpp"
#include <utility>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <unistd.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include <sys/mman.h>
#endif
namespace pc {
using jit_detail::Kind;
static int64_t debugTimeNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
void Jit::setDebugMap(const std::string &path) {
    if (debugState || std::filesystem::exists(path))
        throw std::runtime_error("JIT debug map already exists: " + path);
    debugState = std::make_unique<DebugState>();
    debugState->stream.open(path);
    if (!debugState->stream) throw std::runtime_error("Cannot create JIT debug map: " + path);
    debugState->stream << "{\"event\":\"header\",\"version\":1,\"pid\":" << getpid()
             << ",\"clock\":\"unix-ns\",\"arenaBytes\":" << capacity << "}\n";
    debugReset();
}
void Jit::debugReset() {
    if (!debugState || !debugState->stream.is_open()) return;
    DebugRecord record{};
    record.time = debugTimeNs();
    record.generation = ++debugState->generation;
    debugState->records.push_back(std::move(record));
}
void Jit::debugBlock(const Block &block, const std::vector<uint32_t> &code,
                     const std::vector<DebugRegion> &regions) {
    if (!debugState || !debugState->stream.is_open()) return;
    debugState->records.push_back({debugTimeNs(), debugState->generation,
        reinterpret_cast<uintptr_t>(block.fn), cpu.s[CS].selector, block.csBase, cpu.ip,
        block.cr3, block.page, block.generation, code, regions});
}
void Jit::flushDebugMap() {
    if (!debugState || !debugState->stream.is_open()) return;
    // Serialize after sampling stops. Collection only copies code/metadata;
    // formatting and file IO must not dominate cold-compilation profiles.
    for (const auto &record : debugState->records) {
        if (record.code.empty()) {
            debugState->stream << "{\"event\":\"reset\",\"timeNs\":" << record.time
                     << ",\"generation\":" << record.generation << "}\n";
            continue;
        }
        debugState->stream << "{\"event\":\"load\",\"timeNs\":" << record.time
                 << ",\"generation\":" << record.generation << ",\"start\":" << record.start
                 << ",\"end\":" << record.start + record.code.size()*4 << ",\"cs\":" << record.cs
                 << ",\"csBase\":" << record.csBase << ",\"ip\":" << record.ip
                 << ",\"cr3\":" << record.cr3 << ",\"physicalPage\":" << record.page
                 << ",\"codeGeneration\":" << record.codeGeneration << ",\"arm64\":\"";
        const auto *bytes = reinterpret_cast<const uint8_t *>(record.code.data());
        static constexpr char hex[] = "0123456789abcdef";
        std::string encoded(record.code.size()*8, '\0');
        for (size_t i = 0; i < record.code.size()*4; ++i) {
            encoded[i*2] = hex[bytes[i] >> 4];
            encoded[i*2+1] = hex[bytes[i] & 15];
        }
        debugState->stream << encoded << "\",\"regions\":[";
        bool first = true;
        for (const auto &region : record.regions) {
            debugState->stream << (first ? "" : ",") << "{\"offset\":" << region.offset
                     << ",\"kind\":" << std::quoted(region.kind)
                     << ",\"guestIP\":" << region.guestIP << '}';
            first = false;
        }
        debugState->stream << "]}\n";
    }
    debugState->stream.flush();
    if (!debugState->stream) throw std::runtime_error("Cannot write JIT debug map");
    debugState->records.clear();
}
// The dispatcher asks for this on every cached paged block entry. Reuse a
// current permitted read translation without the address()/physical() call
// pair; misses and segment faults retain the reference CPU's slow path.
static inline uint32_t instructionPage(CPU &cpu) {
    const auto &cs = cpu.s[CS];
    if (!(cpu.cr[0] & 1) || (cpu.flags & 0x20000) || !cs.unusable) {
        const uint32_t linear = cs.base + cpu.ip;
        const uint32_t page = linear >> 12;
        const auto &entry = cpu.tlb[(page ^ (page >> 10)) & (cpu.tlb.size() - 1)];
        if (cpu.tlbCR3 == cpu.cr[3] && entry.epoch == cpu.tlbEpoch && entry.tag == page &&
            ((entry.permissions & 4) || cpu.cpl() != 3 || cpu.systemAccess)) {
            uint32_t physical = entry.base | (linear & 4095);
            if (!cpu.a20) physical &= ~(1u << 20);
            return physical >> 12;
        }
    }
    return cpu.physical(cpu.address(CS, cpu.ip)) >> 12;
}
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
    try { flushDebugMap(); } catch (const std::exception &e) { std::cerr << e.what() << '\n'; }
#if defined(__APPLE__) && defined(__aarch64__)
    if (arena)
        munmap(arena, capacity);
#endif
}
void Jit::clear() {
    debugReset();
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
            currentPage = instructionPage(cpu);
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
Jit::CachedBatch Jit::runCached(unsigned budget) noexcept {
    // Only already-native fast-cache hits run here. Keep these context guards
    // synchronized with run(); misses, compilation and replay stay outside.
    CachedBatch result;
    while (result.count.instructions < budget) {
        if (cpu.nativeFallback || cpu.halted || (cpu.flags & TF) ||
            (cpu.interruptBlockedThrough && cpu.instructions <= cpu.interruptBlockedThrough))
            return result;
        uint64_t key = uint64_t(cpu.s[CS].selector) << 48 | uint64_t(cpu.s[CS].big) << 47 | cpu.ip;
        auto &entry = fast[(cpu.ip ^ (cpu.ip >> 12) ^ (cpu.s[CS].selector * 31u)) & 4095];
        Block *b = entry.key == key ? entry.block : nullptr;
        if (!b || !b->fn || b->executionEpoch != cpu.executionEpoch ||
            b->page >= cpu.generation.size() || b->generation != cpu.generation[b->page] ||
            b->cr0 != cpu.cr[0] || b->cr3 != cpu.cr[3] || b->csBase != cpu.s[CS].base ||
            b->a20 != cpu.a20 || b->vm != bool(cpu.flags & 0x20000) ||
            b->watched != bool(cpu.watchWrite) || (b->usesStack && b->stack32 != cpu.s[SS].big) ||
            b->ram != cpu.mem.data() || b->ramSize != cpu.mem.size() ||
            b->codeBits != cpu.codeUsed.data())
            return result;
        auto oldCR2 = cpu.cr[2];
        try {
            if ((cpu.cr[0] & 0x80000000u) && instructionPage(cpu) != b->page)
                return result;
            if (b->generation != cpu.generation[b->page])
                return result;
        } catch (...) {
            cpu.cr[2] = oldCR2;
            return result;
        }
        ++result.count.dispatches;
        ++cacheHits;
        profile::event(profile::Dispatch);
        profile::event(profile::FastHit);
        profile::event(profile::NativeCall);
        auto n = b->fn(&cpu);
        result.count.instructions += n;
        if (!n) {
            result.stoppedZero = !cpu.nativeFallback;
            break;
        }
    }
    return result;
}
Jit::BatchResult Jit::runBatch(unsigned budget) {
    if (budget > 64)
        budget = 64;
    BatchResult result;
    const auto before = nativeInstructions;
    while (result.instructions < budget && !cpu.halted) {
        auto n = run();
        ++result.dispatches;
        result.instructions += n;
        if (!n || nativeInstructions - before != result.instructions)
            break;
        if (result.instructions < budget) {
            auto tail = runCached(budget - result.instructions);
            cpu.instructions += tail.count.instructions;
            nativeInstructions += tail.count.instructions;
            result.instructions += tail.count.instructions;
            result.dispatches += tail.count.dispatches;
            if (cpu.nativeFallback) {
                profile::event(profile::NativeReplay);
                cpu.nativeFallback = false;
                cpu.step();
                ++result.instructions;
                break;
            }
            if (tail.stoppedZero)
                break;
        }
    }
    return result;
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
    // Keep cheap fragments interpreted, but admit measured expensive pairs.
    const bool flagBranchPair =
        ir.size() == 2 && ir[1].kind == Kind::Branch && ir[1].dst >= 0 &&
        (ir[0].memoryAlu || ir[0].kind == Kind::Test || ir[0].kind == Kind::Alu);
    const bool doubleShiftPair = ir.size() == 2 &&
        ir[0].kind == Kind::DoubleShift && ir[1].kind == Kind::DoubleShift;
    if (ir.empty() || (warmup && ir.size() < 3 && !flagBranchPair && !doubleShiftPair &&
                      ir.back().kind != Kind::CounterBranch && !ir.back().nearCall &&
                      !ir.back().nearReturn && !ir.back().indirectJump))
        return block;
    return emitArm64(std::move(block), decoded);
#endif
    return block;
}
} // namespace pc
