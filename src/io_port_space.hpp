#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pc {
// Port regions select transactions by starting port. A wide access is delivered
// intact: a word read of ATA data is not two reads of adjacent registers.
class IoPortSpace {
  public:
    using Reader = std::function<uint32_t(uint16_t, int)>;
    using Writer = std::function<void(uint16_t, uint32_t, int)>;

    void clear() { regions.clear(); }
    void map(std::string name, uint16_t base, uint32_t count, Reader read, Writer write) {
        if (!count || count > 65536u - base || !read || !write)
            throw std::invalid_argument("Invalid I/O region: " + name);
        const uint32_t end = uint32_t(base) + count;
        for (const auto &region : regions)
            if (base < region.end && region.base < end)
                throw std::invalid_argument("Overlapping I/O regions: " + name + " and " + region.name);
        regions.push_back({std::move(name), base, end, std::move(read), std::move(write)});
    }
    std::optional<uint32_t> read(uint16_t port, int width) const {
        for (const auto &region : regions)
            if (port >= region.base && port < region.end)
                return region.read(port, width);
        return std::nullopt;
    }
    bool write(uint16_t port, uint32_t value, int width) const {
        for (const auto &region : regions)
            if (port >= region.base && port < region.end) {
                region.write(port, value, width);
                return true;
            }
        return false;
    }

  private:
    struct Region {
        std::string name;
        uint32_t base, end;
        Reader read;
        Writer write;
    };
    std::vector<Region> regions;
};
} // namespace pc
