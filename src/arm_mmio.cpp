#include "arm_mmio.hpp"
#include <stdexcept>
namespace pc {
uint64_t armMmioLoad(uint64_t value, unsigned width, bool signExtend, bool wideRegister) {
    if (width != 1 && width != 2 && width != 4 && width != 8)
        throw std::runtime_error("Invalid ARM MMIO load width");
    if (width < 8) {
        value &= (1ull << (width * 8)) - 1;
        if (signExtend) {
            const auto sign = 1ull << (width * 8 - 1);
            value = (value ^ sign) - sign;
        }
    }
    return wideRegister ? value : uint32_t(value);
}
} // namespace pc
