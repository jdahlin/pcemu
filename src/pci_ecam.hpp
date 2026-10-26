#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
namespace pc {
// One ECAM bus (32 slots, function zero). Endpoints own configuration/BAR state;
// the bus owns routing only, so BAR relocation and command decoding stay live.
class PciEcamBus {
  public:
    using Reader = std::function<uint64_t(uint64_t, unsigned)>;
    using Writer = std::function<void(uint64_t, uint64_t, unsigned)>;
    using Contains = std::function<bool(uint64_t, unsigned)>;
    // Detach all registered endpoints.
    void clear();
    // Attach function zero; reject invalid callbacks and occupied/out-of-range slots.
    void attach(std::string name, unsigned slot, Reader configRead, Writer configWrite,
                Contains contains, Reader read, Writer write);
    // Read configuration bytes or ones for an absent function.
    uint64_t read_configuration(uint64_t offset, unsigned width) const;
    // Write configuration bytes, ignoring absent functions.
    void write_configuration(uint64_t offset, uint64_t value, unsigned width) const;
    // Test live BAR decoding for the whole requested access.
    bool contains(uint64_t address, unsigned width) const;
    // Read a decoded BAR, or return no result.
    std::optional<uint64_t> read_bar(uint64_t address, unsigned width) const;
    // Write a decoded BAR; return false when unhandled.
    bool write_bar(uint64_t address, uint64_t value, unsigned width) const;

  private:
    struct Endpoint {
        std::string name;
        unsigned slot;
        Reader configRead;
        Writer configWrite;
        Contains contains;
        Reader read;
        Writer write;
    };
    std::vector<Endpoint> endpoints;
    const Endpoint *configuration_endpoint(uint64_t offset) const;
    const Endpoint *bar_endpoint(uint64_t address, unsigned width) const;
};
} // namespace pc
