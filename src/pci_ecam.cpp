// PCI Express Enhanced Configuration Access Mechanism (ECAM) maps configuration
// registers into memory: address bits choose bus, device, function and byte offset.
// This router exposes bus 0 with 32 device slots and function 0 only. Endpoints
// own configuration contents and BAR relocation/enable state. Every BAR access
// asks their live address predicate, so a relocated/disabled window takes effect
// immediately. No cached memory map needs rebuilding after configuration writes.
//
// IMPLEMENTATION NOTE: Registration validates host callbacks and rejects duplicate
// slots. Overlapping enabled BARs select the last registered endpoint, retaining
// board routing precedence; this is host policy, not PCI arbitration behavior.
// The caller owns access-width/alignment validation. No bridges, transactions,
// link state, bus errors or ordering model are implemented by this router.
// Address mapping reference: AMD PG055 v2.9, Enhanced Configuration Access,
// Table 2-24 (ECAM Addressing). Only its standard address layout is used here;
// this model is not an emulation of the AMD AXI bridge.
// https://docs.amd.com/r/en-US/pg055-axi-bridge-pcie/Enhanced-Configuration-Access

#include "pci_ecam.hpp"
#include <stdexcept>
#include <utility>
namespace pc {
namespace {
// One bus occupies 1 MiB: 32 devices * 8 functions * 4096 configuration bytes.
constexpr uint64_t BusConfigurationBytes = 0x100000;
// Device number occupies ECAM address bits [19:15].
constexpr unsigned DeviceNumberShift = 15;
constexpr unsigned DeviceSlotsPerBus = 32;
// Function number occupies bits [14:12]; nonzero functions are not attached.
constexpr uint64_t FunctionNumberMask = 0x7000;
// Bits [11:0] select a byte in one function's 4 KiB configuration space.
constexpr uint64_t ConfigurationOffsetMask = 0xfff;
// An absent function reads ones. The MMIO caller narrows this host result.
constexpr uint64_t AbsentConfigurationValue = UINT64_MAX;
} // namespace
// Detach all host endpoints; subsequent configuration reads report absence.
// IMPLEMENTATION NOTE: Host routing/wiring around the ECAM interface.
// https://docs.amd.com/r/en-US/pg055-axi-bridge-pcie/Enhanced-Configuration-Access
void PciEcamBus::clear() {
    endpoints.clear();
}
// Register function zero in a slot; reject missing callbacks or duplicate slots.
// IMPLEMENTATION NOTE: Host routing/wiring around the ECAM interface.
// https://docs.amd.com/r/en-US/pg055-axi-bridge-pcie/Enhanced-Configuration-Access
void PciEcamBus::attach(std::string name, unsigned slot, Reader cr, Writer cw, Contains contains,
                        Reader read, Writer write) {
    if (slot >= DeviceSlotsPerBus || !cr || !cw || !contains || !read || !write)
        throw std::invalid_argument("Invalid PCI endpoint: " + name);
    for (const auto &endpoint : endpoints)
        if (endpoint.slot == slot)
            throw std::invalid_argument("Occupied PCI slot: " + name);
    endpoints.push_back({std::move(name), slot, std::move(cr), std::move(cw), std::move(contains),
                         std::move(read), std::move(write)});
}
// Decode the ECAM bus/device/function fields and find an attached function zero.
// AMD PG055 v2.9, Table 2-24: ECAM address mapping.
// https://docs.amd.com/r/en-US/pg055-axi-bridge-pcie/Enhanced-Configuration-Access
const PciEcamBus::Endpoint *PciEcamBus::configuration_endpoint(uint64_t offset) const {
    if (offset >= BusConfigurationBytes || (offset & FunctionNumberMask))
        return nullptr;
    for (const auto &endpoint : endpoints)
        if (endpoint.slot == (offset >> DeviceNumberShift))
            return &endpoint;
    return nullptr;
}
// Find the last registered endpoint currently decoding this address and width.
// IMPLEMENTATION NOTE: Host routing/wiring around the ECAM interface.
// https://docs.amd.com/r/en-US/pg055-axi-bridge-pcie/Enhanced-Configuration-Access
const PciEcamBus::Endpoint *PciEcamBus::bar_endpoint(uint64_t address, unsigned width) const {
    // Last registered BAR wins, preserving platform precedence if BARs overlap.
    for (auto e = endpoints.rbegin(); e != endpoints.rend(); ++e)
        if (e->contains(address, width))
            return &*e;
    return nullptr;
}
// Read endpoint configuration bytes, or all ones for an unattached function.
// AMD PG055 v2.9, Table 2-24: ECAM address mapping.
// https://docs.amd.com/r/en-US/pg055-axi-bridge-pcie/Enhanced-Configuration-Access
uint64_t PciEcamBus::read_configuration(uint64_t offset, unsigned width) const {
    auto *endpoint = configuration_endpoint(offset);
    return endpoint ? endpoint->configRead(offset & ConfigurationOffsetMask, width)
                    : AbsentConfigurationValue;
}
// Forward configuration bytes to an attached function; ignore an absent target.
// AMD PG055 v2.9, Table 2-24: ECAM address mapping.
// https://docs.amd.com/r/en-US/pg055-axi-bridge-pcie/Enhanced-Configuration-Access
void PciEcamBus::write_configuration(uint64_t offset, uint64_t value, unsigned width) const {
    if (auto *endpoint = configuration_endpoint(offset))
        endpoint->configWrite(offset & ConfigurationOffsetMask, value, width);
}
// Report whether any endpoint currently decodes the complete BAR access.
// IMPLEMENTATION NOTE: Host routing/wiring around the ECAM interface.
// https://docs.amd.com/r/en-US/pg055-axi-bridge-pcie/Enhanced-Configuration-Access
bool PciEcamBus::contains(uint64_t address, unsigned width) const {
    return bar_endpoint(address, width) != nullptr;
}
// Read a live BAR window, returning no result when no endpoint decodes it.
// IMPLEMENTATION NOTE: Host routing/wiring around the ECAM interface.
// https://docs.amd.com/r/en-US/pg055-axi-bridge-pcie/Enhanced-Configuration-Access
std::optional<uint64_t> PciEcamBus::read_bar(uint64_t address, unsigned width) const {
    if (auto *endpoint = bar_endpoint(address, width))
        return endpoint->read(address, width);
    return std::nullopt;
}
// Write a live BAR window and report whether an endpoint handled the access.
// IMPLEMENTATION NOTE: Host routing/wiring around the ECAM interface.
// https://docs.amd.com/r/en-US/pg055-axi-bridge-pcie/Enhanced-Configuration-Access
bool PciEcamBus::write_bar(uint64_t address, uint64_t value, unsigned width) const {
    if (auto *endpoint = bar_endpoint(address, width)) {
        endpoint->write(address, value, width);
        return true;
    }
    return false;
}
} // namespace pc
