#pragma once
#include <cstdint>
#ifdef PCEMU_PROFILE
#include <array>
#include <fstream>
#include <exception>
#include <limits>
#include <time.h>
#include <iomanip>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#endif

// Diagnostic hooks are compiled away in the normal executable. Counts from the
// instrumented executable explain coverage; its timings are not benchmarks.
namespace pc::profile {
enum Event {
    Dispatch,
    FastHit,
    MapHit,
    LookupMiss,
    MetadataRebuild,
    Translate,
    InterpretedDispatch,
    NegativeHit,
    NativeCall,
    NativeReplay,
    DevicePoll,
    DiskReadSectors,
    DiskWriteSectors,
    CowDecompress,
    CowCacheHit,
    ShortFragmentHit,
    ShortMemoryFragmentHit,
    ShortPromote,
    NativeInvalidOpcode,
    CachedContextValidation,
    CachedContextChange,
    CachedContextHit,
    CachedContextMiss,
    Count
};
enum class InterpretationReason { OutsideJit, Forced, Warming, ShortFragment, Declined, Replay, Count };
#ifdef PCEMU_PROFILE
inline constexpr const char *interpretationReasonNames[]={"outsideJit","forced","warming","shortFragment","declined","replay"};
inline thread_local InterpretationReason interpretationReason = InterpretationReason::OutsideJit;
struct InterpretationScope {
    InterpretationReason previous;
    explicit InterpretationScope(InterpretationReason reason):previous(interpretationReason) { interpretationReason=reason; }
    ~InterpretationScope() { interpretationReason=previous; }
    void set(InterpretationReason reason) { interpretationReason=reason; }
};
inline bool active = false;
inline unsigned currentOpcode = 0;
struct Data {
    uint64_t startInstructions = 0;
    std::array<uint64_t, Count> events{};
    std::array<uint64_t, 512> opcodes{}, lockedOpcodes{};
    std::array<std::array<uint64_t, unsigned(InterpretationReason::Count)>,512> interpretationReasons{};
    std::array<std::array<uint64_t, 2>, 512> operands{};
    std::array<std::array<uint64_t, 8>, 512> groups{};
    std::map<std::tuple<uint32_t, uint32_t, uint16_t, uint32_t, unsigned, unsigned>, uint64_t> sampledIPs;
    uint64_t attempts = 0;
    uint32_t random = 0x731be5;
    unsigned untilSample = 1;
    struct CpuTiming {
        uint64_t attempts = 0, samples = 0, rawNs = 0;
        uint64_t faults = 0, exceptions = 0;
    };
    std::array<CpuTiming, 512> cpuTiming{};
    CpuTiming unknownTiming{};
    uint32_t cpuRandom = 0x9e3779b9;
    unsigned untilCpuSample = 1;
    uint64_t clockPairMeanNs = 0, clockFailures = 0, nestedScopes = 0;
    bool cpuClockAvailable = false;
};
inline Data data;
inline uint64_t profileGeneration = 0;
inline uint64_t threadCpuNs() noexcept {
#if defined(__APPLE__) || defined(__linux__)
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0)
        return uint64_t(value.tv_sec) * 1000000000 + uint64_t(value.tv_nsec);
#endif
    return std::numeric_limits<uint64_t>::max();
}
// Replaceable only for deterministic accounting tests; normal diagnostics use
// the executing thread's user+system CPU clock, never elapsed wall time.
inline uint64_t (*instructionClockNs)() noexcept = threadCpuNs;
struct InstructionScope;
inline thread_local InstructionScope *currentScope = nullptr;
struct InstructionScope {
    InstructionScope *parent = nullptr;
    uint64_t generation = 0, begin = 0;
    int op = -1, uncaught = 0;
    bool registered = false, sampled = false, fault = false, nested = false, lockPrefix = false;
    InstructionScope(const InstructionScope &) = delete;
    InstructionScope &operator=(const InstructionScope &) = delete;
    InstructionScope() noexcept {
        if (!active) return;
        registered = true;
        generation = profileGeneration;
        parent = currentScope;
        currentScope = this;
        nested = parent && parent->generation == generation;
        if (nested) return; // Parent timing is inclusive; do not count it twice.
        if (--data.untilCpuSample) return;
        // Independent jitter avoids periodic guest-loop and IP-sampler bias.
        data.cpuRandom = data.cpuRandom * 1664525u + 1013904223u;
        data.untilCpuSample = 64 + (data.cpuRandom >> 25);
        uncaught = std::uncaught_exceptions();
        begin = instructionClockNs();
        sampled = begin != std::numeric_limits<uint64_t>::max();
        if (!sampled) ++data.clockFailures;
    }
    ~InstructionScope() noexcept {
        const auto end = sampled ? instructionClockNs() : 0;
        if (!registered) return;
        currentScope = parent;
        if (!active || generation != profileGeneration) return;
        if (nested) { ++data.nestedScopes; return; }
        auto &timing = op >= 0 ? data.cpuTiming[unsigned(op)] : data.unknownTiming;
        ++timing.attempts;
        if (!sampled) return;
        if (end == std::numeric_limits<uint64_t>::max() || end < begin) {
            ++data.clockFailures;
            return;
        }
        data.cpuClockAvailable = true;
        const auto elapsed = end - begin;
        ++timing.samples;
        timing.rawNs += elapsed;
        timing.faults += fault;
        timing.exceptions += std::uncaught_exceptions() > uncaught;
    }
};
inline void instructionFault() noexcept {
    if (currentScope) currentScope->fault = true;
}
inline uint64_t adjustedCpuNs(const Data::CpuTiming &timing) noexcept {
    const auto overhead = timing.samples * data.clockPairMeanNs;
    // Subtract after aggregation, not per sample: clipping quantized short
    // samples individually would bias fast opcodes upwards.
    return timing.rawNs > overhead ? timing.rawNs - overhead : 0;
}
inline void calibrateCpuClock() noexcept {
    uint64_t total = 0, pairs = 0;
    for (unsigned n = 0; n < 64; ++n) {
        auto begin = instructionClockNs(), end = instructionClockNs();
        if (begin == std::numeric_limits<uint64_t>::max() || end == std::numeric_limits<uint64_t>::max() || end < begin) continue;
        total += end - begin;
        ++pairs;
    }
    data.cpuClockAvailable = pairs != 0;
    data.clockPairMeanNs = pairs ? total / pairs : 0;
}
inline void event(Event e, uint64_t n = 1) {
    if (active)
        data.events[e] += n;
}
inline void locked() { if(currentScope)currentScope->lockPrefix=true; }
inline void opcode(unsigned op, uint16_t cs, uint32_t base, uint32_t cr3, uint32_t ip) {
    if (!active)
        return;
    currentOpcode = op;
    if (currentScope) currentScope->op = int(op);
    ++data.opcodes[op];
    if(currentScope && currentScope->lockPrefix)++data.lockedOpcodes[op];
    ++data.interpretationReasons[op][unsigned(interpretationReason)];
    ++data.attempts;
    if (--data.untilSample == 0) {
        ++data.sampledIPs[{cr3, base, cs, ip, op, unsigned(interpretationReason)}];
        // Jitter the interval to avoid locking onto a short guest loop.
        data.random = data.random * 1664525u + 1013904223u;
        data.untilSample = 128 + (data.random >> 24);
    }
}
inline void extended(unsigned op) {
    if (!active)
        return;
    currentOpcode = 256 + op;
    if (currentScope) currentScope->op = int(currentOpcode);
    ++data.opcodes[currentOpcode];
    if(currentScope && currentScope->lockPrefix)++data.lockedOpcodes[currentOpcode];
    ++data.interpretationReasons[currentOpcode][unsigned(interpretationReason)];
}
inline void operand(uint8_t modrm) {
    if (!active)
        return;
    ++data.operands[currentOpcode][(modrm >> 6) == 3 ? 0 : 1];
    ++data.groups[currentOpcode][(modrm >> 3) & 7];
}
inline void start(uint64_t instructions) {
    data = {};
    data.startInstructions = instructions;
    ++profileGeneration;
    calibrateCpuClock();
    active = true;
}
inline void stop(const std::string &path, uint64_t instructions) {
    active = false;
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("Cannot write guest profile");
    const char *names[] = {"dispatch",
                           "fastHit",
                           "mapHit",
                           "lookupMiss",
                           "metadataRebuild",
                           "translate",
                           "interpretedDispatch",
                           "negativeHit",
                           "nativeCall",
                           "nativeReplay",
                           "devicePoll",
                           "diskReadSectorsIncludingParents",
                           "diskWriteSectors",
                           "cowDecompress",
                           "cowCacheHit",
                           "shortFragmentHit",
                           "shortMemoryFragmentHit",
                           "shortPromote", "nativeInvalidOpcode", "cachedContextValidation",
                           "cachedContextChange", "cachedContextHit", "cachedContextMiss"};
    out << "{\"startInstructions\":" << data.startInstructions
        << ",\"stopInstructions\":" << instructions << ",\"interpretedAttempts\":" << data.attempts
        << ",\"events\":{";
    for (unsigned e = 0; e < Count; ++e)
        out << (e ? "," : "") << std::quoted(names[e]) << ':' << data.events[e];
    out << "},\"cpuTiming\":{\"version\":1,\"clock\":\"thread-cpu-ns\",\"available\":"
        << (data.cpuClockAvailable ? "true" : "false")
        << ",\"sampleIntervalMinimum\":64,\"sampleIntervalMaximum\":191,\"clockPairMeanNs\":"
        << data.clockPairMeanNs << ",\"clockFailures\":" << data.clockFailures
        << ",\"nestedScopesExcluded\":" << data.nestedScopes
        << ",\"unknownAttempts\":" << data.unknownTiming.attempts
        << ",\"unknownSamples\":" << data.unknownTiming.samples
        << ",\"unknownRawNs\":" << data.unknownTiming.rawNs
        << ",\"unknownAdjustedNs\":" << adjustedCpuNs(data.unknownTiming) << "},\"opcodes\":[";
    bool first = true;
    for (unsigned op = 0; op < 512; ++op) {
        if (!data.opcodes[op])
            continue;
        out << (first ? "" : ",") << "{\"opcode\":" << op << ",\"attempts\":" << data.opcodes[op]
            << ",\"lockPrefixAttempts\":" << data.lockedOpcodes[op]
            << ",\"registerOperands\":" << data.operands[op][0]
            << ",\"memoryOperands\":" << data.operands[op][1] << ",\"groups\":[";
        first = false;
        for (unsigned g = 0; g < 8; ++g)
            out << (g ? "," : "") << data.groups[op][g];
        const auto &timing = data.cpuTiming[op];
        out << "],\"cpuLeafAttempts\":" << timing.attempts
            << ",\"cpuSamples\":" << timing.samples << ",\"cpuRawNs\":" << timing.rawNs
            << ",\"cpuAdjustedNs\":" << adjustedCpuNs(timing)
            << ",\"cpuFaultSamples\":" << timing.faults << ",\"cpuExceptionSamples\":" << timing.exceptions;
        out << ",\"interpretationReasons\":{";
        for(unsigned r=0;r<unsigned(InterpretationReason::Count);++r)
            out << (r?",":"") << std::quoted(interpretationReasonNames[r]) << ':' << data.interpretationReasons[op][r];
        out << "}}";
    }
    out << "],\"sampledIPs\":[";
    first = true;
    for (auto [key, n] : data.sampledIPs) {
        auto [cr3, base, cs, ip, opcode, reason] = key;
        out << (first ? "" : ",") << "{\"cr3\":" << cr3 << ",\"base\":" << base << ",\"cs\":" << cs
            << ",\"ip\":" << ip << ",\"primaryOpcode\":" << opcode
            << ",\"interpretationReason\":" << std::quoted(interpretationReasonNames[reason]) << ",\"samples\":" << n << '}';
        first = false;
    }
    out << "]}\n";
}
#else
struct InterpretationScope {
    explicit InterpretationScope(InterpretationReason) {}
    void set(InterpretationReason) {}
};
inline void event(Event, uint64_t = 1) {}
inline void locked() {}
inline void opcode(unsigned, uint16_t, uint32_t, uint32_t, uint32_t) {}
inline void extended(unsigned) {}
inline void operand(uint8_t) {}
#endif
} // namespace pc::profile
