#include "pci_ecam.hpp"
#include <stdexcept>
#include <utility>
namespace pc {
void PciEcamBus::clear() { endpoints.clear(); }
void PciEcamBus::attach(std::string name, unsigned slot, Reader cr, Writer cw,
                        Contains contains, Reader read, Writer write) {
    if (slot >= 32 || !cr || !cw || !contains || !read || !write)
        throw std::invalid_argument("Invalid PCI endpoint: " + name);
    for (const auto &endpoint : endpoints)
        if (endpoint.slot == slot) throw std::invalid_argument("Occupied PCI slot: " + name);
    endpoints.push_back({std::move(name), slot, std::move(cr), std::move(cw),
                         std::move(contains), std::move(read), std::move(write)});
}
const PciEcamBus::Endpoint *PciEcamBus::configEndpoint(uint64_t offset) const {
    if (offset >= 0x100000 || (offset & 0x7000)) return nullptr;
    for (const auto &endpoint : endpoints)
        if (endpoint.slot == (offset >> 15)) return &endpoint;
    return nullptr;
}
const PciEcamBus::Endpoint *PciEcamBus::barEndpoint(uint64_t address, unsigned width) const {
    // Last registered BAR wins, preserving platform precedence if BARs overlap.
    for (auto e = endpoints.rbegin(); e != endpoints.rend(); ++e)
        if (e->contains(address, width)) return &*e;
    return nullptr;
}
uint64_t PciEcamBus::configRead(uint64_t offset, unsigned width) const {
    auto *endpoint = configEndpoint(offset);
    return endpoint ? endpoint->configRead(offset & 0xfff, width) : UINT64_MAX;
}
void PciEcamBus::configWrite(uint64_t offset, uint64_t value, unsigned width) const {
    if (auto *endpoint = configEndpoint(offset)) endpoint->configWrite(offset & 0xfff, value, width);
}
bool PciEcamBus::contains(uint64_t address, unsigned width) const {
    return barEndpoint(address, width) != nullptr;
}
std::optional<uint64_t> PciEcamBus::read(uint64_t address, unsigned width) const {
    if (auto *endpoint = barEndpoint(address, width)) return endpoint->read(address, width);
    return std::nullopt;
}
bool PciEcamBus::write(uint64_t address, uint64_t value, unsigned width) const {
    if (auto *endpoint = barEndpoint(address, width)) { endpoint->write(address, value, width); return true; }
    return false;
}
} // namespace pc
