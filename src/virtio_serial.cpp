#include "virtio_serial.hpp"
#include <algorithm>
#include <stdexcept>
namespace pc {
VirtioSerial::VirtioSerial(std::span<uint8_t> ram, uint64_t base, Interrupt irq,
                           std::string portName)
    : VirtioDevice(ram, base, 3, 4, 2, std::move(irq)), name(std::move(portName)) {
    if (name.empty() || name.size() > 128 || name.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid virtio serial port name");
}
uint64_t VirtioSerial::configRead(unsigned offset, unsigned width) const {
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        if (offset + i == 4)
            value |= 1ull << (i * 8);
    return value; // max_nr_ports=1; size and emergency output are not advertised.
}
void VirtioSerial::deviceReset() {
    guestOpen = deviceReady = ready = false;
    controls.clear();
    incoming.clear();
    outgoing.clear();
}
void VirtioSerial::control(uint16_t event, uint16_t value, const std::string &text) {
    if (controls.size() >= 32)
        throw std::runtime_error("Virtio serial control queue overflow");
    std::vector<uint8_t> bytes(8, 0); // Port ID zero.
    bytes[4] = uint8_t(event);
    bytes[5] = uint8_t(event >> 8);
    bytes[6] = uint8_t(value);
    bytes[7] = uint8_t(value >> 8);
    bytes.insert(bytes.end(), text.begin(), text.end());
    controls.push_back(std::move(bytes));
}
void VirtioSerial::hostConnected(bool connected) {
    if (hostOpen == connected)
        return;
    hostOpen = connected;
    incoming.clear();
    outgoing.clear();
    if (ready)
        control(6, connected);
    poll();
}
size_t VirtioSerial::receiveCapacity() const {
    return ready && guestOpen && hostOpen ? limit - incoming.size() : 0;
}
size_t VirtioSerial::receive(std::span<const uint8_t> bytes) {
    auto size = std::min(bytes.size(), receiveCapacity());
    incoming.insert(incoming.end(), bytes.begin(), bytes.begin() + size);
    poll();
    return size;
}
std::vector<uint8_t> VirtioSerial::takeOutput(size_t maximum) {
    size_t count = std::min(maximum, outgoing.size());
    std::vector<uint8_t> bytes;
    bytes.reserve(count);
    while (count--) {
        bytes.push_back(outgoing.front());
        outgoing.pop_front();
    }
    return bytes;
}
void VirtioSerial::poll() {
    process(3);
    if (!controls.empty())
        process(2);
    process(1);
    if (ready && guestOpen && hostOpen && !incoming.empty())
        process(0);
}
std::optional<uint32_t> VirtioSerial::handle(unsigned queue, const Chain &chain) {
    if (queue == 3) {
        if (!negotiated(1) || chain.writable || chain.readable != 8)
            throw std::runtime_error("Invalid virtio serial control request");
        auto data = chain.input(8);
        unsigned id = unsigned(data[0]) | (unsigned(data[1]) << 8) | (unsigned(data[2]) << 16) |
                      (unsigned(data[3]) << 24);
        unsigned event = data[4] | (unsigned(data[5]) << 8),
                 value = data[6] | (unsigned(data[7]) << 8);
        if (event == 0) {
            if (value == 1 && !deviceReady) {
                deviceReady = true;
                control(1, 1);
            }
        } else if (!id && deviceReady && event == 3) {
            if (value == 1 && !ready) {
                ready = true;
                control(7, 1, name);
                control(6, hostOpen);
            }
        } else if (!id && ready && event == 6) {
            guestOpen = value != 0;
            if (!guestOpen)
                incoming.clear();
        }
        return 0;
    }
    if (queue == 2) {
        if (chain.readable || !chain.writable)
            throw std::runtime_error("Invalid virtio serial control buffer");
        if (controls.empty())
            return std::nullopt;
        auto length = controls.front().size();
        chain.output(controls.front());
        controls.pop_front();
        return uint32_t(length);
    }
    if (queue == 1) {
        if (chain.writable)
            throw std::runtime_error("Virtio serial TX must be readable");
        if (hostOpen && ready && chain.readable > limit - outgoing.size())
            return std::nullopt;
        auto bytes = chain.input(limit);
        if (hostOpen && ready)
            outgoing.insert(outgoing.end(), bytes.begin(), bytes.end());
        transmitted += bytes.size();
        return 0;
    }
    if (chain.readable || !chain.writable)
        throw std::runtime_error("Virtio serial RX must be writable");
    if (incoming.empty())
        return std::nullopt;
    size_t count = std::min(incoming.size(), chain.writable);
    std::vector<uint8_t> bytes;
    bytes.reserve(count);
    for (size_t i = 0; i < count; ++i)
        bytes.push_back(incoming[i]);
    chain.output(bytes);
    for (size_t i = 0; i < count; ++i)
        incoming.pop_front();
    received += count;
    return uint32_t(count);
}
} // namespace pc
