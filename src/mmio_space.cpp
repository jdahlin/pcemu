#include "mmio_space.hpp"
#include <limits>
#include <stdexcept>
#include <utility>
namespace pc {
void MmioSpace::clear() { regions.clear(); }
void MmioSpace::map(std::string name, uint64_t base, uint64_t size, Handler read,
                    Handler write, unsigned widths) {
    if (!size || size - 1 > std::numeric_limits<uint64_t>::max() - base ||
        (!read && !write) || !widths || (widths & ~15u))
        throw std::invalid_argument("Invalid MMIO region: " + name);
    for (const auto &region : regions)
        if (region.size && (base >= region.base ? base - region.base < region.size
                                               : region.base - base < size))
            throw std::invalid_argument("Overlapping MMIO regions: " + name + " and " + region.name);
    auto matches = [=](const MmioAccess &a) {
        return a.address >= base && a.address - base < size && (widths & a.width) &&
               (a.write ? bool(write) : bool(read));
    };
    auto handler = [read = std::move(read), write = std::move(write)](MmioAccess &a, uint64_t offset) {
        if (a.write) write(a, offset); else read(a, offset);
    };
    regions.push_back({std::move(name), base, size, std::move(matches), std::move(handler)});
}
void MmioSpace::mapDynamic(std::string name, Matcher matches, Handler access) {
    if (!matches || !access) throw std::invalid_argument("Invalid dynamic MMIO region: " + name);
    regions.push_back({std::move(name), 0, 0, std::move(matches), std::move(access)});
}
bool MmioSpace::access(MmioAccess &a) const {
    if (a.width != 1 && a.width != 2 && a.width != 4 && a.width != 8)
        throw std::invalid_argument("Invalid MMIO transaction width");
    for (const auto &region : regions)
        if (region.matches(a)) {
            region.access(a, a.address - region.base);
            return true;
        }
    return false;
}
} // namespace pc
