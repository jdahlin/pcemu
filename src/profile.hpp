#pragma once
#include <cstdint>
#ifdef PCEMU_PROFILE
#include <array>
#include <fstream>
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
    Count
};
#ifdef PCEMU_PROFILE
inline bool active = false;
inline unsigned currentOpcode = 0;
struct Data {
    uint64_t startInstructions = 0;
    std::array<uint64_t, Count> events{};
    std::array<uint64_t, 512> opcodes{};
    std::array<std::array<uint64_t, 2>, 512> operands{};
    std::array<std::array<uint64_t, 8>, 512> groups{};
    std::map<std::tuple<uint32_t, uint32_t, uint16_t, uint32_t>, uint64_t> sampledIPs;
    uint64_t attempts = 0;
    uint32_t random = 0x731be5;
    unsigned untilSample = 1;
};
inline Data data;
inline void event(Event e, uint64_t n = 1) {
    if (active)
        data.events[e] += n;
}
inline void opcode(unsigned op, uint16_t cs, uint32_t base, uint32_t cr3, uint32_t ip) {
    if (!active)
        return;
    currentOpcode = op;
    ++data.opcodes[op];
    ++data.attempts;
    if (--data.untilSample == 0) {
        ++data.sampledIPs[{cr3, base, cs, ip}];
        // Jitter the interval to avoid locking onto a short guest loop.
        data.random = data.random * 1664525u + 1013904223u;
        data.untilSample = 128 + (data.random >> 24);
    }
}
inline void extended(unsigned op) {
    if (!active)
        return;
    currentOpcode = 256 + op;
    ++data.opcodes[currentOpcode];
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
                           "cowCacheHit"};
    out << "{\"startInstructions\":" << data.startInstructions
        << ",\"stopInstructions\":" << instructions << ",\"interpretedAttempts\":" << data.attempts
        << ",\"events\":{";
    for (unsigned e = 0; e < Count; ++e)
        out << (e ? "," : "") << std::quoted(names[e]) << ':' << data.events[e];
    out << "},\"opcodes\":[";
    bool first = true;
    for (unsigned op = 0; op < 512; ++op) {
        if (!data.opcodes[op])
            continue;
        out << (first ? "" : ",") << "{\"opcode\":" << op << ",\"attempts\":" << data.opcodes[op]
            << ",\"registerOperands\":" << data.operands[op][0]
            << ",\"memoryOperands\":" << data.operands[op][1] << ",\"groups\":[";
        first = false;
        for (unsigned g = 0; g < 8; ++g)
            out << (g ? "," : "") << data.groups[op][g];
        out << "]}";
    }
    out << "],\"sampledIPs\":[";
    first = true;
    for (auto [key, n] : data.sampledIPs) {
        auto [cr3, base, cs, ip] = key;
        out << (first ? "" : ",") << "{\"cr3\":" << cr3 << ",\"base\":" << base << ",\"cs\":" << cs
            << ",\"ip\":" << ip << ",\"samples\":" << n << '}';
        first = false;
    }
    out << "]}\n";
}
#else
inline void event(Event, uint64_t = 1) {}
inline void opcode(unsigned, uint16_t, uint32_t, uint32_t, uint32_t) {}
inline void extended(unsigned) {}
inline void operand(uint8_t) {}
#endif
} // namespace pc::profile
