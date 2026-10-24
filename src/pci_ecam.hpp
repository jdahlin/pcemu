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
    void clear();
    void attach(std::string name, unsigned slot, Reader configRead, Writer configWrite,
                Contains contains, Reader read, Writer write);
    uint64_t configRead(uint64_t offset, unsigned width) const;
    void configWrite(uint64_t offset, uint64_t value, unsigned width) const;
    bool contains(uint64_t address, unsigned width) const;
    std::optional<uint64_t> read(uint64_t address, unsigned width) const;
    bool write(uint64_t address, uint64_t value, unsigned width) const;
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
    const Endpoint *configEndpoint(uint64_t offset) const;
    const Endpoint *barEndpoint(uint64_t address, unsigned width) const;
};
} // namespace pc
