#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pc {
struct MmioAccess {
    uint64_t address, value;
    unsigned width;
    bool write;
    // Some hardware adapters suppress a discarded read; FIFO reads normally
    // still execute. The CPU boundary owns destination-register semantics.
    bool discard = false;
};
class MmioSpace {
  public:
    using Handler = std::function<void(MmioAccess &, uint64_t)>;
    using Matcher = std::function<bool(const MmioAccess &)>;
    void clear();
    // Callbacks receive the offset, with the original transaction width intact.
    // Width bits are 1|2|4|8; selection uses the starting address, while each
    // device validates any transfer crossing its register/window boundaries.
    void map(std::string name, uint64_t base, uint64_t size, Handler read,
             Handler write, unsigned widths = 1 | 2 | 4 | 8);
    // Dynamic BAR decoding retains registration precedence with fixed regions.
    void mapDynamic(std::string name, Matcher matches, Handler access);
    bool access(MmioAccess &access) const;
  private:
    struct Region {
        std::string name;
        uint64_t base, size;
        Matcher matches;
        Handler access;
    };
    std::vector<Region> regions;
};
} // namespace pc
