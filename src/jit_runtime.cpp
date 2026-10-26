#include "jit_runtime.hpp"
#include <cstring>
namespace pc::jit_detail {
void nativeFinish(CPU *cpu, int op, uint32_t a, uint32_t b, int w, int condition,
                         uint32_t target) {
    if (op >= 8) {
        auto carry = cpu->flags & CF;
        cpu->alu(op == 8 ? 0 : 5, a, b, w);
        cpu->flags = (cpu->flags & ~CF) | carry;
    } else if (op >= 0)
        cpu->alu(op, a, b, w);
    if (condition >= 0 && cpu->condition(condition))
        cpu->ip = target;
}
// No C++ exception can unwind through generated ARM64 frames. Decline before
// touching devices or guest data; the dispatcher replays that instruction in
// the interpreter with committed GPRs, flags and its original instruction IP.
uint32_t nativeMemory(CPU *cpu, const NativeMemoryMove *op, uint32_t offset, uint32_t value,
                             uint32_t codePage, uint32_t codeGeneration) {
    cpu->nativeMemoryCalls++;
    auto previousCR2 = cpu->cr[2];
    try {
        if (op->width != 1 && op->width != 2 && op->width != 4) {
            cpu->nativeFallback = true;
            return 0;
        }
        if (op->store && cpu->watchWrite) {
            cpu->nativeFallback = true;
            return 0;
        }
        auto linear = cpu->address(op->segment, offset);
        if ((linear & 4095) + op->width > 4096) {
            cpu->nativeFallback = true;
            return 0;
        }
        // RMW probes read translation first. A declined device access must
        // replay its read before dirty-bit updates or a write callback.
        const bool rmw = op->delta || op->alu >= 0 || op->exchange || op->bit > 4;
        auto physical = cpu->physical(linear, op->store && !rmw);
        if (uint64_t(physical) + op->width > cpu->mem.size() ||
            (physical < 0xb0000 && uint64_t(physical) + op->width > 0xa0000)) {
            cpu->nativeFallback = true;
            return 0;
        }
        if (op->store) {
            if (rmw) {
                uint32_t original = 0;
                std::memcpy(&original, cpu->mem.data() + physical, op->width);
                // The operand can alias its own PTE. Capture it before the
                // write probe updates D in that same guest word.
                physical = cpu->physical(linear, true);
                cpu->nativeValue = original;
                if (op->bit > 4) {
                    auto mask = 1u << (value & (op->width * 8 - 1));
                    value = op->bit == 5 ? original | mask : op->bit == 6 ? original & ~mask : original ^ mask;
                } else if (op->delta) value = original + uint32_t(op->delta);
                else if (!op->exchange) switch (op->alu) {
                    case 0: value = original + value; break;
                    case 1: value = original | value; break;
                    case 2: value = original + value + (cpu->flags & CF); break;
                    case 3: value = original - value - (cpu->flags & CF); break;
                    case 4: value = original & value; break;
                    case 5: value = original - value; break;
                    case 6: value = original ^ value; break;
                    default: cpu->nativeFallback = true; return 0;
                }
            }
            if (op->width == 4)
                std::memcpy(cpu->mem.data() + physical, &value, 4);
            else if (op->width == 2)
                std::memcpy(cpu->mem.data() + physical, &value, 2);
            else
                cpu->mem[physical] = uint8_t(value);
            cpu->written(physical, op->width);
        } else {
            value = 0;
            if (op->width == 4)
                std::memcpy(&value, cpu->mem.data() + physical, 4);
            else if (op->width == 2)
                std::memcpy(&value, cpu->mem.data() + physical, 2);
            else
                value = cpu->mem[physical];
            cpu->nativeValue = value;
        }
        return cpu->generation[codePage] == codeGeneration ? 1 : 2;
    } catch (...) {
        cpu->cr[2] = previousCR2;
        cpu->nativeFallback = true;
        return 0;
    }
}
} // namespace pc::jit_detail
