#include "arm_hyperv.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <utility>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace pc {
namespace {
constexpr uint32_t uid = 0x8600ff01, function = 0x46000001;
constexpr uint16_t invalidCode = 2, invalidInput = 3, invalidAlignment = 4,
                   invalidParameter = 5, accessDenied = 6, invalidVp = 14;
constexpr uint64_t privileges = (1ull << 1) | (1ull << 5) | (1ull << 6) | (1ull << 49) | (1ull << 51) | (1ull << 52);
uint64_t get(std::span<const uint8_t> b, size_t at, size_t size) {
    uint64_t v = 0;
    for (size_t i = 0; i < size; ++i) v |= uint64_t(b[at + i]) << (8 * i);
    return v;
}
void put(std::span<uint8_t> b, size_t at, uint64_t v, size_t size) {
    for (size_t i = 0; i < size; ++i) b[at + i] = uint8_t(v >> (8 * i));
}
}
ArmHyperV::ArmHyperV(bool on, std::span<uint8_t> memory, uint64_t start, unsigned count, Clock source)
    : enabled(on), synic(memory, start, count), ram(memory), base(start), cpus(count), clock(std::move(source)) {
    if (!clock) clock = [] {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) / 100;
    };
    clockOrigin = clock();
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
    if (name >= 0xa0000 && name <= 0xa0015) {
        if (!guestId) return accessDenied;
        return synic.read(name, vp, value[0]);
    }
    if (name >= 0x210 && name <= 0x214) {
        value[0] = crashParameters[vp][name - 0x210]; return 0;
    }
    if (name == 0x215) { value[0] = (1ull << 63) | (1ull << 62); return 0; }
    switch (name) {
    case 0x100: value[0] = 1ull | (1ull << 48); return 0; // PCEmu interface version 1.0, build 1
    case 0x200: value[0] = privileges; value[1] = 1ull << 8; return 0; // ARM64 bit 72: crash registers
    case 0x201: value[0] = 0xffffffff00000000ull | (interruptSender ? 4 : 0); return 0; // Recommend IPIs only with a real interrupt backend
    case 0x202: value[0] = cpus | (uint64_t(cpus) << 32); return 0;
    case 0x203: value[0] = 2; return 0; // Stage-2 translation, no synthetic interrupt remapping
    case 0x204: case 0x205: case 0x206: case 0x207: case 0x208: case 0x209: return 0;
    case 0x90002: value[0] = guestId; return 0;
    case 0x90004: {
        // Shared by all VPs, including early reads before GuestOsId. Dispatch
        // is serialized with all vCPUs stopped. Clamp equal-resolution samples
        // to maintain the TLFS strictly increasing access order.
        auto now = clock();
        auto elapsed = now >= clockOrigin ? now - clockOrigin : 0;
        referenceTime = std::max(referenceTime + 1, elapsed);
        value[0] = referenceTime; return 0;
    }
    case 0x90006: value[0] = 0x31237648; return 0; // ARM64 interface version: Hv#1
    case 0x90003:
        if (!guestId) return accessDenied;
        value[0] = vp; return 0;
    case 0x90015:
        if (!guestId) return accessDenied;
        value[0] = partitionInfo; return 0;
    case 0x90016:
        if (!guestId) return accessDenied;
        value[0] = tlbiControl[vp]; return 0;
    case 0x40400: value[0] = syntheticVectors[vp]; return 0;
    case 0x40401: return 0; // No synthetic exception has been injected.
    default: return invalidParameter;
    }
}
uint16_t ArmHyperV::writeRegister(uint32_t name, unsigned vp, std::array<uint64_t, 2> value) {
    if (name >= 0xa0000 && name <= 0xa0015) {
        if (!guestId) return accessDenied;
        if (value[1]) return invalidParameter;
        return synic.write(name, vp, value[0]);
    }
    if (name >= 0x210 && name <= 0x214) {
        if (value[1]) return invalidParameter;
        crashParameters[vp][name - 0x210] = value[0]; return 0;
    }
    if (name == 0x215) {
        constexpr uint64_t notify = 1ull << 63, message = 1ull << 62;
        if (value[1] || (value[0] & ((1ull << 58) - 1)) ||
            (value[0] && !(value[0] & notify))) return invalidParameter;
        if (!value[0]) return 0;
        const auto &parameters = crashParameters[vp];
        std::string hex;
        if (value[0] & message) {
            auto address = parameters[3], size = parameters[4];
            if (size > 4096 || (size && (address < base || address - base > ram.size() ||
                size > ram.size() - (address - base)))) return invalidParameter;
            constexpr char digits[] = "0123456789abcdef";
            hex.reserve(size_t(size) * 2);
            for (size_t i = 0; i < size; ++i) {
                auto byte = ram[size_t(address - base) + i];
                hex += digits[byte >> 4]; hex += digits[byte & 15];
            }
        }
        // Copy before returning to the guest: later register/RAM writes must
        // not change already notified diagnostics. No guest shutdown is implied.
        notifiedParameters = parameters;
        crashControl = value[0]; crashVp = vp;
        crashMessageHex = std::move(hex);
        ++crashNotifications; crashPending = true;
        return 0;
    }
    if (name == 0x40400) {
        if (value[1] || (value[0] & 0x7ff)) return invalidParameter;
        // A guest VA, separate from the architectural VBAR_EL1. Optional
        // synthetic interrupt/exception producers remain disabled.
        syntheticVectors[vp] = value[0];
        return 0;
    }
    if (name == 0x90016) {
        if (!guestId) return accessDenied;
        if (value[1] || (value[0] & ~uint64_t(1))) return invalidParameter;
        // Per-VP TLBI opt-in. Architectural TLBI still executes through HVF;
        // this backend does not queue additional asynchronous flush actions.
        tlbiControl[vp] = value[0];
        return 0;
    }
    if (name == 0x90015) {
        if (!guestId) return accessDenied;
        if (value[1] || (value[0] & 0xffe)) return invalidParameter;
        if (value[0] & 1) {
            uint64_t address = value[0] & ~uint64_t(4095);
            if (address < base || address - base > ram.size() ||
                4096 > ram.size() - (address - base)) return invalidParameter;
            // Windows supplies this shared page. No asynchronous address-space
            // flushes are queued by this backend: its pending-flush word is zero.
            // All remaining fields are reserved/zero in this initial interface.
            std::fill_n(ram.begin() + size_t(address - base), 4096, uint8_t(0));
        }
        partitionInfo = value[0];
        return 0;
    }
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
    if (uint16_t(lastResult)) {
        ++failures;
        if (calls.contains(code)) ++callFailures[code];
    }
    r[0] = lastResult;
    return true;
}
uint64_t ArmHyperV::invoke(Registers &r, bool smccc, unsigned vp) {
    auto control = r[smccc ? 1 : 0];
    auto code = uint16_t(control);
    if (code == 0xd6) {
        // ARM enlightened TLB drain: Windows supplies no input or output
        // parameters, after consulting its partition-info pending-flush word.
        if (!guestId) return accessDenied;
        if ((control & ~uint64_t(0x1ffff)) || vp >= cpus) return invalidInput;
        if (!translationFlusher) return invalidCode;
        const uint64_t targets = 1ull << vp;
        if (!translationFlusher(targets)) return 0x71;
        ++tlbFlushRequests; tlbFlushTargetMask = targets;
        tlbFlushAddressSpace = 0; tlbFlushFlags = 0;
        return 0;
    }
    if (code == 0x2 || code == 0x13 || code == 0x19 || code == 0x1a || code == 0xb || code == 0x15) {
        if (!guestId) return accessDenied;
        const bool flush = code == 0x2 || code == 0x13;
        const bool ipi = code == 0xb || code == 0x15;
        const bool extended = code == 0x13 || code == 0x1a || code == 0x15;
        const uint64_t allowed = extended ? 0x7ffffff : 0x1ffff;
        if ((control & ~allowed) || vp >= cpus) return invalidInput;
        size_t banks = extended ? (control >> 17) & 0x3ff : 0;
        if (banks > 64) return invalidInput;
        size_t prefix = flush ? 16 : 8;
        size_t size = prefix + (extended ? 16 + banks * 8 : 8);
        unsigned in = smccc ? 2 : 1;
        std::vector<uint8_t> input(size);
        if (control & 0x10000) {
            if (size > 128) return invalidInput;
            for (size_t i = 0; i < size; ++i) input[i] = uint8_t(r[in + i / 8] >> ((i % 8) * 8));
        } else {
            auto address = r[in];
            if ((address & 7) || size > 4096 - (address & 4095) || address < base ||
                address - base > ram.size() || size > ram.size() - (address - base)) return invalidAlignment;
            std::copy_n(ram.begin() + size_t(address - base), size, input.begin());
        }
        auto flags = get(input, prefix - 8, 8);
        if (flush && (flags & ~uint64_t(7))) return invalidParameter;
        // ARM64 IPI vectors are GIC SGI IDs. Accept current VTL0 or explicit
        // VTL0 (UseTargetVtl bit), with all remaining flag/reserved bytes zero.
        if (ipi && ((flags & ~0x1fffffffffull) || uint32_t(flags) >= 16 ||
            ((flags & (0x10ull << 32)) && (flags & (0xfull << 32))))) return invalidParameter;
        const bool allProcessors = flush && (flags & 1);
        uint64_t targets = 0;
        const uint64_t valid = (1ull << cpus) - 1;
        if (!extended) {
            targets = get(input, prefix, 8);
            if (!flush && !ipi && (flags > 1 || (flags && targets))) return invalidParameter;
            if (allProcessors || (!flush && !ipi && flags)) targets = valid;
        } else {
            // HV_VP_SET follows the operation's fixed prefix. Only its bank
            // contents belong to the variable input header.
            if (!flush && !ipi && flags) return invalidParameter;
            auto format = get(input, prefix, 8), validBanks = get(input, prefix + 8, 8);
            if (banks != size_t(std::popcount(validBanks))) return invalidInput;
            if (format == 1) {
                if (validBanks) return invalidParameter;
                targets = valid;
            } else if (!format) {
                size_t at = prefix + 16;
                for (unsigned bank = 0; bank < 64; ++bank) if (validBanks & (1ull << bank)) {
                    auto bits = get(input, at, 8); at += 8;
                    // Empty banks are legal even beyond the configured VP range.
                    if (bank && bits && !allProcessors) return invalidVp;
                    if (!bank) targets = bits;
                }
            } else return invalidParameter;
        }
        if (allProcessors) targets = valid; // The supplied processor mask is ignored.
        if (targets & ~valid) return invalidVp;
        if (ipi) {
            if (!interruptSender) return invalidCode;
            if (!interruptSender(targets, uint32_t(flags))) return 0x71;
            ++ipiRequests; ipiTargetMask = targets; ipiVector = uint32_t(flags);
            for (unsigned cpu = 0; cpu < cpus; ++cpu) if (targets & (1ull << cpu)) ++ipiTargetRequests[cpu];
            return 0;
        }
        if (flush) {
            if (!translationFlusher) return invalidCode;
            // The native backend conservatively flushes all ASIDs/global
            // mappings, which also covers any requested address-space subset.
            if (!translationFlusher(targets)) return 0x71;
            ++tlbFlushRequests; tlbFlushTargetMask = targets;
            tlbFlushAddressSpace = get(input, 0, 8); tlbFlushFlags = flags;
            return 0;
        }
        if (!contextSynchronizer) return invalidCode;
        if (!contextSynchronizer(targets)) return 0x71; // HV_STATUS_OPERATION_FAILED
        ++contextSyncRequests; contextSyncTargetMask = targets;
        return 0;
    }
    if (code == 0x8001) {
        if (!guestId) return accessDenied;
        if ((control & ~uint64_t(0x1ffff)) || vp >= cpus) return invalidInput;
        if (control & 0x10000) {
            // No input; one 16-byte output pair. Only its low 8 bytes are
            // defined. Clear the padding as for GetVpRegisters fast output.
            unsigned out = smccc ? 2 : 15;
            r[out] = r[out + 1] = 0;
        } else {
            auto address = r[smccc ? 3 : 2];
            if ((address & 7) || address < base || address - base > ram.size() ||
                8 > ram.size() - (address - base)) return invalidAlignment;
            // No 0x8002+ service is currently provided. A zero capability
            // bitmap is the truthful supported-set response, not a stub action.
            std::fill_n(ram.begin() + size_t(address - base), 8, uint8_t(0));
        }
        return 0;
    }
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
std::string ArmHyperV::crashReport() const {
    std::ostringstream s;
    s << "{\"notifications\":" << crashNotifications << ",\"vp\":" << crashVp
      << ",\"control\":" << crashControl << ",\"parameters\":[";
    for (unsigned i = 0; i < 5; ++i) { if (i) s << ','; s << notifiedParameters[i]; }
    s << "],\"messageHex\":\"" << crashMessageHex << "\"}";
    return s.str();
}
std::string ArmHyperV::takeCrashReport() {
    if (!crashPending) return {};
    crashPending = false;
    return crashReport();
}
std::string ArmHyperV::report() const {
    std::ostringstream s;
    s << "{\"schema\":1,\"mode\":\"" << (enabled ? "basic" : "off") << "\",\"guestOsId\":" << guestId
      << ",\"partitionInfoPage\":" << partitionInfo
      << ",\"referenceTime100ns\":" << referenceTime
      << ",\"ipiRequests\":" << ipiRequests << ",\"ipiTargetMask\":" << ipiTargetMask
      << ",\"ipiVector\":" << ipiVector
      << ",\"crash\":" << crashReport()
      << ",\"tlbFlushRequests\":" << tlbFlushRequests
      << ",\"tlbFlushTargetMask\":" << tlbFlushTargetMask
      << ",\"tlbFlushAddressSpace\":" << tlbFlushAddressSpace
      << ",\"tlbFlushFlags\":" << tlbFlushFlags
      << ",\"contextSyncRequests\":" << contextSyncRequests
      << ",\"contextSyncTargetMask\":" << contextSyncTargetMask
      << ",\"uidQueries\":" << uidQueries << ",\"smcccVersionQueries\":" << versionQueries
      << ",\"smcccCalls\":" << smcccCalls << ",\"legacyCalls\":" << legacyCalls
      << ",\"disabledCalls\":" << disabledCalls << ",\"failures\":" << failures
      << ",\"lastControl\":" << lastControl << ",\"lastResult\":" << lastResult
      << ",\"lastRegister\":" << lastRegister << ",\"omittedCounters\":" << omittedCounters << ",\"calls\":[";
    bool comma = false;
    for (auto [code, n] : calls) { if (comma) s << ','; comma = true; s << "{\"code\":" << code << ",\"count\":" << n
          << ",\"failures\":" << (callFailures.contains(code) ? callFailures.at(code) : 0) << '}'; }
    s << "],\"ipiTargetRequests\":[";
    for (unsigned cpu = 0; cpu < cpus; ++cpu) { if (cpu) s << ','; s << ipiTargetRequests[cpu]; }
    s << "],\"registers\":["; comma = false;
    for (auto [name, n] : registers) {
        if (comma) s << ','; comma = true;
        s << "{\"name\":" << name << ",\"reads\":" << n.reads << ",\"writes\":" << n.writes << ",\"failures\":" << n.failures << '}';
    }
    s << "],\"vps\":[";
    for (unsigned vp = 0; vp < cpus; ++vp) {
        if (vp) s << ',';
        s << "{\"index\":" << vp << ",\"tlbiControl\":" << tlbiControl[vp]
          << ",\"syntheticVector\":" << syntheticVectors[vp] << '}';
    }
    return s.str() + "]}\n";
}
} // namespace pc
