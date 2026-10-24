#include "arm_hyperv.hpp"
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace pc {
namespace {
constexpr uint32_t uid = 0x8600ff01, function = 0x46000001;
constexpr uint16_t invalidCode = 2, invalidInput = 3, invalidAlignment = 4,
                   invalidParameter = 5, accessDenied = 6, invalidVp = 14;
constexpr uint64_t privileges = (1ull << 5) | (1ull << 6) | (1ull << 49) | (1ull << 51);
uint64_t get(std::span<const uint8_t> b, size_t at, size_t size) {
    uint64_t v = 0;
    for (size_t i = 0; i < size; ++i) v |= uint64_t(b[at + i]) << (8 * i);
    return v;
}
void put(std::span<uint8_t> b, size_t at, uint64_t v, size_t size) {
    for (size_t i = 0; i < size; ++i) b[at + i] = uint8_t(v >> (8 * i));
}
}
ArmHyperV::ArmHyperV(bool on, std::span<uint8_t> memory, uint64_t start, unsigned count)
    : enabled(on), ram(memory), base(start), cpus(count) {
    if (!cpus || cpus > 8) throw std::runtime_error("Invalid Hyper-V VP count");
}
ArmHyperV::Counts *ArmHyperV::count(uint32_t name) {
    auto it = registers.find(name);
    if (it != registers.end()) return &it->second;
    if (registers.size() == 128) { ++omittedCounters; return nullptr; }
    return &registers[name];
}
uint16_t ArmHyperV::readRegister(uint32_t name, unsigned vp, std::array<uint64_t, 2> &value) {
    value = {};
    switch (name) {
    case 0x100: value[0] = 1ull | (1ull << 48); return 0; // PCEmu interface version 1.0, build 1
    case 0x200: value[0] = privileges; return 0;
    case 0x201: value[0] = 0xffffffff00000000ull; return 0; // No scheduling/flush recommendations
    case 0x202: value[0] = cpus | (uint64_t(cpus) << 32); return 0;
    case 0x203: value[0] = 2; return 0; // Stage-2 translation, no synthetic interrupt remapping
    case 0x204: case 0x205: case 0x206: case 0x207: case 0x208: case 0x209: return 0;
    case 0x90002: value[0] = guestId; return 0;
    case 0x90003:
        if (!guestId) return accessDenied;
        value[0] = vp; return 0;
    default: return invalidParameter;
    }
}
uint16_t ArmHyperV::writeRegister(uint32_t name, unsigned vp, std::array<uint64_t, 2> value) {
    if (name != 0x90002 || value[1]) return invalidParameter;
    if (vp != 0) return accessDenied;
    guestId = value[0];
    return 0;
}
bool ArmHyperV::call(unsigned immediate, Registers &r, unsigned vp) {
    if (immediate > 1) return false;
    if (!immediate && uint32_t(r[0]) == uid) {
        ++uidQueries;
        if (!enabled) return false;
        r[0] = 0x4d32ba58; r[1] = 0xcd244764; r[2] = 0x8eef6c75; r[3] = 0x16597024;
        return true;
    }
    if (!immediate && (uint32_t(r[0]) == 0x80000000 || uint32_t(r[0]) == 0x80000001)) {
        ++versionQueries;
        if (!enabled) return false;
        if (uint32_t(r[0]) == 0x80000000) r[0] = 0x10002; // SMCCC 1.2
        else r[0] = (uint32_t(r[1]) == uid || uint32_t(r[1]) == function ||
                     uint32_t(r[1]) == 0x80000000 || uint32_t(r[1]) == 0x80000001) ? 0 : uint64_t(-1);
        return true;
    }
    if (!immediate && uint32_t(r[0]) != function) return false;
    if (immediate) ++legacyCalls; else ++smcccCalls;
    lastControl = r[immediate ? 0 : 1];
    auto code = uint16_t(lastControl);
    if (calls.contains(code) || calls.size() < 128) ++calls[code]; else ++omittedCounters;
    if (!enabled) { ++disabledCalls; lastResult = uint64_t(-1); return false; }
    lastResult = invoke(r, !immediate, vp);
    if (uint16_t(lastResult)) ++failures;
    r[0] = lastResult;
    return true;
}
uint64_t ArmHyperV::invoke(Registers &r, bool smccc, unsigned vp) {
    auto control = r[smccc ? 1 : 0];
    auto code = uint16_t(control);
    if (code != 0x50 && code != 0x51) return guestId ? invalidCode : accessDenied;
    // Only Fast, Code, RepCount and RepStart are supported. No nested/variable headers.
    constexpr uint64_t allowed = 0x0fff0fff0001ffffull;
    if (control & ~allowed) return invalidInput;
    size_t reps = (control >> 32) & 0xfff, first = (control >> 48) & 0xfff;
    if (!reps || first >= reps || vp >= cpus) return invalidInput;
    bool fast = control & 0x10000, write = code == 0x51;
    size_t inputSize = 16 + reps * (write ? 32 : 4), outputSize = write ? 0 : reps * 16;
    if (inputSize > 4096 || outputSize > 4096) return invalidInput;
    std::vector<uint8_t> input(inputSize);
    std::span<uint8_t> output;
    unsigned inputStart = smccc ? 2 : 1, outputStart = 0;
    auto memory = [&](uint64_t address, size_t size) -> std::span<uint8_t> {
        if ((address & 7) || size > 4096 - (address & 4095) || address < base ||
            address - base > ram.size() || size > ram.size() - (address - base)) return {};
        return ram.subspan(size_t(address - base), size);
    };
    if (fast) {
        auto inputPairs = (inputSize + 15) / 16, outputPairs = outputSize / 16;
        if (inputPairs + outputPairs > 8) return invalidInput;
        for (size_t i = 0; i < inputSize; ++i) input[i] = uint8_t(r[inputStart + i / 8] >> ((i % 8) * 8));
        // OpenVMM ARM64 ABI: SMCCC output follows 16-byte input pairs;
        // legacy HVC #1 output occupies the final pairs ending at X16.
        outputStart = unsigned(smccc ? inputStart + inputPairs * 2 : 17 - outputPairs * 2);
    } else {
        auto in = memory(r[inputStart], inputSize);
        if (in.empty()) return invalidAlignment;
        std::copy(in.begin(), in.end(), input.begin());
        if (outputSize) {
            auto a = r[inputStart], b = r[inputStart + 1];
            output = memory(b, outputSize);
            if (output.empty() || (a < b + outputSize && b < a + inputSize)) return invalidAlignment;
        }
    }
    if (get(input, 0, 8) != uint64_t(-1)) return accessDenied;
    auto target = get(input, 8, 4);
    if (target != 0xfffffffe && target != vp) return invalidVp;
    if (get(input, 12, 4)) return invalidParameter; // VTL0 only, reserved bytes zero
    for (size_t i = first; i < reps; ++i) {
        size_t at = 16 + i * (write ? 32 : 4);
        auto name = uint32_t(get(input, at, 4)); lastRegister = name;
        auto counters = count(name);
        if (counters) { if (write) ++counters->writes; else ++counters->reads; }
        std::array<uint64_t, 2> value{};
        uint16_t status;
        if (write) {
            status = get(input, at + 4, 4) || get(input, at + 8, 8) ? invalidParameter :
                writeRegister(name, vp, {get(input, at + 16, 8), get(input, at + 24, 8)});
        } else status = readRegister(name, vp, value);
        if (status) { if (counters) ++counters->failures; return (uint64_t(i) << 32) | status; }
        if (!write) {
            if (fast) { r[outputStart + i * 2] = value[0]; r[outputStart + i * 2 + 1] = value[1]; }
            else { put(output, i * 16, value[0], 8); put(output, i * 16 + 8, value[1], 8); }
        }
    }
    return uint64_t(reps) << 32;
}
std::string ArmHyperV::report() const {
    std::ostringstream s;
    s << "{\"schema\":1,\"mode\":\"" << (enabled ? "basic" : "off") << "\",\"guestOsId\":" << guestId
      << ",\"uidQueries\":" << uidQueries << ",\"smcccVersionQueries\":" << versionQueries
      << ",\"smcccCalls\":" << smcccCalls << ",\"legacyCalls\":" << legacyCalls
      << ",\"disabledCalls\":" << disabledCalls << ",\"failures\":" << failures
      << ",\"lastControl\":" << lastControl << ",\"lastResult\":" << lastResult
      << ",\"lastRegister\":" << lastRegister << ",\"omittedCounters\":" << omittedCounters << ",\"calls\":[";
    bool comma = false;
    for (auto [code, n] : calls) { if (comma) s << ','; comma = true; s << "{\"code\":" << code << ",\"count\":" << n << '}'; }
    s << "],\"registers\":["; comma = false;
    for (auto [name, n] : registers) {
        if (comma) s << ','; comma = true;
        s << "{\"name\":" << name << ",\"reads\":" << n.reads << ",\"writes\":" << n.writes << ",\"failures\":" << n.failures << '}';
    }
    return s.str() + "]}\n";
}
} // namespace pc
