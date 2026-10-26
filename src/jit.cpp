#include "jit.hpp"
#include "jit_ir.hpp"
#include "profile.hpp"
#include <utility>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <unistd.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
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
    debugState->stream << "{\"event\":\"header\",\"version\":2,\"pid\":" << getpid()
             << ",\"clock\":\"unix-ns\",\"arenaBytes\":" << capacity
             << ",\"stubArenaBytes\":" << stubCapacity << "}\n";
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
                     const std::vector<DebugRegion> &regions, uintptr_t address) {
    if (!debugState || !debugState->stream.is_open()) return;
    debugState->records.push_back({debugTimeNs(), debugState->generation,
        address ? address : reinterpret_cast<uintptr_t>(block.fn), cpu.s[CS].selector, block.csBase, cpu.ip,
        block.cr3, block.page, block.generation, code, regions});
}
void Jit::flushDebugMap() {
    if (!debugState || !debugState->stream.is_open()) return;
    // Serialize after sampling stops. Collection only copies code/metadata;
    // formatting and file IO must not dominate cold-compilation profiles.
    for (const auto &record : debugState->records) {
        if (record.patch) {
            debugState->stream << "{\"event\":\"patch\",\"timeNs\":" << record.time
                << ",\"generation\":" << record.generation << ",\"start\":" << record.start
                << ",\"end\":" << record.start + 4 << ",\"arm64\":\"";
            static constexpr char hex[] = "0123456789abcdef";
            const auto *bytes = reinterpret_cast<const uint8_t *>(record.code.data());
            for (unsigned i = 0; i < 4; ++i)
                debugState->stream << hex[bytes[i] >> 4] << hex[bytes[i] & 15];
            debugState->stream << "\"}\n";
            continue;
        }
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
    stubCapacity = std::min(capacity, size_t(1024*1024));
    if (capacity > std::numeric_limits<size_t>::max() - stubCapacity)
        throw std::invalid_argument("JIT arena allocation size overflow");
#if defined(__APPLE__) && defined(__aarch64__)
    if (en) {
        arena = static_cast<uint8_t *>(mmap(nullptr, capacity + stubCapacity, PROT_READ | PROT_WRITE | PROT_EXEC,
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
        munmap(arena, capacity + stubCapacity);
#endif
}
void Jit::clear() {
    debugReset();
    incomingLinks.clear();
    directLinkTransitions = directLinkPromotions = directLinkBlocks = 0;
    blocks.clear();
    fast = {};
    used = stubUsed = 0;
    nativeInstructions = compiledBlocks = cacheHits = cacheRecycles = 0;
}
void Jit::setDirectLinks(bool enabled, unsigned hotEntries) {
    // Stub/body edges use ARM64 B immediates in both directions. Keep the
    // entire allocation aligned and within the conservative signed range.
    if (enabled && ((capacity & 15) || capacity + stubCapacity >= (size_t(1) << 27)))
        throw std::invalid_argument("Direct links require an aligned arena smaller than 128 MiB");
    if (directLinks != enabled || linkWarmup != hotEntries) {
        clear();
        directLinks = enabled;
        linkWarmup = hotEntries;
    }
}
void Jit::setLinkTarget(Block &source, unsigned slot, uintptr_t target) {
    auto &link = *source.link();
    if (link.target[slot] == target) return;
    const auto base = reinterpret_cast<uintptr_t>(source.fn);
    const auto address = base + link.branchOffsets[slot];
    const auto destination = target ? target : base + link.fallbackOffset;
    const auto words = (int64_t(destination) - int64_t(address)) / 4;
    if (words < -(1 << 25) || words >= (1 << 25))
        throw std::runtime_error("Direct-link branch outside ARM64 range");
    const uint32_t instruction = 0x14000000u | (uint32_t(words) & 0x03ffffffu);
#if defined(__APPLE__) && defined(__aarch64__)
    // Publication/invalidation happens only after all native frames returned.
    pthread_jit_write_protect_np(0);
    *reinterpret_cast<uint32_t *>(address) = instruction;
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(reinterpret_cast<void *>(address), 4);
#endif
    link.target[slot] = target;
    debugPatch(address, instruction);
}
void Jit::debugPatch(uintptr_t address, uint32_t instruction) {
    if (debugState) {
        DebugRecord record{};
        record.time = debugTimeNs();
        record.generation = debugState->generation;
        record.start = address;
        record.code = {instruction};
        record.patch = true;
        debugState->records.push_back(std::move(record));
    }
}
void Jit::patchIncoming(uint64_t key) {
    auto pending = incomingLinks.find(key);
    if (pending == incomingLinks.end()) return;
    auto target = blocks.find(key);
    auto &list = pending->second;
    std::erase_if(list, [&](const IncomingLink &edge) {
        auto source = blocks.find(edge.source);
        if (source == blocks.end() || source->second.fn != edge.sourceEntry || !source->second.link())
            return true;
        auto &a = source->second;
        if (target == blocks.end()) { setLinkTarget(a, edge.slot, 0); return false; }
        uintptr_t destinationEntry = 0;
        const auto &b = target->second;
        const auto destination = a.link()->ips[edge.slot];
        if (a.fn && b.fn && b.link() && b.link()->promoted && b.link()->entry && a.link()->saveMask == b.link()->saveMask &&
            (uint32_t(a.csBase + a.link()->startIP) >> 12) == (uint32_t(a.csBase + destination) >> 12) &&
            a.page == b.page && a.generation == b.generation && a.cr0 == b.cr0 &&
            a.cr3 == b.cr3 && a.csBase == b.csBase && a.a20 == b.a20 && a.vm == b.vm &&
            a.executionEpoch == b.executionEpoch && a.watched == b.watched &&
            a.stack32 == b.stack32 && a.ram == b.ram && a.ramSize == b.ramSize &&
            a.codeBits == b.codeBits && a.link()->generationData == b.link()->generationData)
            destinationEntry = reinterpret_cast<uintptr_t>(b.fn) +
                (&a == &b ? b.link()->loopEntry :
                 (b.link()->usedRegisters & ~a.link()->usedRegisters) == 0 ? b.link()->loadedEntry : b.link()->entry);
        setLinkTarget(a, edge.slot, destinationEntry);
        return false;
    });
}
void Jit::publishLinks(uint64_t key, Block &block) {
    for (unsigned slot = 0; slot < block.link()->count; ++slot) {
        auto destination = (key & 0xffffffff00000000ull) | block.link()->ips[slot];
        incomingLinks[destination].push_back({key, slot, block.fn});
        patchIncoming(destination);
    }
    patchIncoming(key);
}
Jit::Block *Jit::installBlock(uint64_t key, Block block) {
    if (!directLinks) return &blocks.insert_or_assign(key, std::move(block)).first->second;
    // No native frame is live here. Remove incoming references before releasing
    // helper metadata embedded in the old code, then publish compatible edges.
    if (auto it = incomingLinks.find(key); it != incomingLinks.end())
        for (auto &edge : it->second) {
            auto source = blocks.find(edge.source);
            if (source != blocks.end() && source->second.fn == edge.sourceEntry && source->second.link())
                setLinkTarget(source->second, edge.slot, 0);
        }
    auto &stored = blocks.insert_or_assign(key, std::move(block)).first->second;
    if (stored.link() && stored.link()->promoted) publishLinks(key, stored);
    patchIncoming(key);
    return &stored;
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
          cached->codeBits != cpu.codeUsed.data() ||
          (directLinks && cached->link() && cached->link()->generationData != cpu.generation.data()))) ||
        (checkedPage && cached->page != currentPage)) {
        try {
            profile::event(profile::MetadataRebuild);
            auto metadata = compile(false);
            cached = installBlock(key, std::move(metadata));
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
            cached = installBlock(key, std::move(translated));
        } catch (const Fault &) {
            cpu.step();
            return 1;
        }
    }
    if (directLinks && cached->fn && cached->link() && !cached->linkRequested &&
        ++cached->visits >= linkWarmup) {
        cached->linkRequested = true;
        ++directLinkPromotions;
        if (promoteBlock(*cached)) publishLinks(key, *cached);
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
    auto n = uint32_t(b.fn(&cpu, 0));
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
    return directLinks ? runCachedImpl<true>(budget) : runCachedImpl<false>(budget);
}
template <bool Linking> Jit::CachedBatch Jit::runCachedImpl(unsigned budget) noexcept {
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
            b->codeBits != cpu.codeUsed.data() ||
            (Linking && b->link() && b->link()->generationData != cpu.generation.data()))
            return result;
        if (Linking && b->link() && !b->linkRequested && ++b->visits >= linkWarmup)
            return result; // Promotion/patching is only allowed outside native frames.
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
        auto packed = b->fn(&cpu, Linking ? budget - result.count.instructions : 0);
        auto n = uint32_t(packed);
        unsigned dispatches = 1;
        bool stoppedZero = false;
        if constexpr (Linking) {
            dispatches = unsigned(packed >> 32);
            stoppedZero = dispatches & 0x8000;
            dispatches &= 0x7fff;
        }
        assert(n <= 127 && dispatches <= 64);
        if (!dispatches) dispatches = 1;
        result.count.dispatches += dispatches;
        cacheHits += dispatches;
        if constexpr (Linking) directLinkTransitions += dispatches - 1;
        profile::event(profile::Dispatch, dispatches);
        profile::event(profile::FastHit, dispatches);
        profile::event(profile::NativeCall, dispatches);
        result.count.instructions += n;
        if (!n || stoppedZero) {
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
                      !ir.back().nearReturn && !ir.back().indirectJump &&
                      !ir.front().stringMove))
        return block;
    return emitArm64(std::move(block), decoded);
#endif
    return block;
}
#if !defined(__APPLE__) || !defined(__aarch64__)
bool Jit::promoteBlock(Block &) { return false; }
#endif
} // namespace pc
