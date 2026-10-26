#include "virtio_serial.hpp"
#include <algorithm>
#include <stdexcept>
namespace pc {
VirtioSerial::VirtioSerial(std::span<uint8_t> ram, uint64_t base, Interrupt irq,
                           std::string portName)
    : VirtioSerial(ram, base, std::move(irq), std::vector<std::string>{std::move(portName)}) {}
VirtioSerial::VirtioSerial(std::span<uint8_t> ram, uint64_t base, Interrupt irq,
                           std::vector<std::string> names)
    : VirtioDevice(ram, base, 3, unsigned(2 + names.size() * 2), 2, std::move(irq)) {
    if (names.empty() || names.size() > 8)
        throw std::runtime_error("Invalid virtio serial port count");
    for (auto &name : names) {
        if (name.empty() || name.size() > 128 || name.find('\0') != std::string::npos ||
            std::any_of(ports.begin(), ports.end(), [&](const Port &p) { return p.name == name; }))
            throw std::runtime_error("Invalid or duplicate virtio serial port name");
        ports.push_back(Port{std::move(name), false, false, false, {}, {}});
    }
}
uint64_t VirtioSerial::read_device_configuration(unsigned offset, unsigned width) const {
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        if (offset + i == 4)
            value |= uint64_t(ports.size()) << (i * 8);
    return value; // size and emergency output are not advertised.
}
void VirtioSerial::reset_device() {
    deviceReady = false;
    controls.clear();
    for (auto &port : ports) {
        port.guestOpen = port.ready = false;
        port.incoming.clear();
        port.outgoing.clear();
    }
}
void VirtioSerial::control(uint16_t event, uint16_t value, const std::string &text, unsigned port) {
    if (controls.size() >= 128)
        throw std::runtime_error("Virtio serial control queue overflow");
    std::vector<uint8_t> bytes(8, 0);
    bytes[0] = uint8_t(port);
    bytes[4] = uint8_t(event);
    bytes[5] = uint8_t(event >> 8);
    bytes[6] = uint8_t(value);
    bytes[7] = uint8_t(value >> 8);
    bytes.insert(bytes.end(), text.begin(), text.end());
    controls.push_back(std::move(bytes));
}
void VirtioSerial::hostConnected(bool connected, unsigned id) {
    auto &port = ports.at(id);
    if (port.hostOpen == connected)
        return;
    port.hostOpen = connected;
    port.incoming.clear();
    port.outgoing.clear();
    if (port.ready)
        control(6, connected, {}, id);
    poll();
}
size_t VirtioSerial::receiveCapacity(unsigned id) const {
    const auto &port = ports.at(id);
    return port.ready && port.guestOpen && port.hostOpen ? limit - port.incoming.size() : 0;
}
size_t VirtioSerial::receive(std::span<const uint8_t> bytes, unsigned id) {
    auto &port = ports.at(id);
    auto size = std::min(bytes.size(), receiveCapacity(id));
    port.incoming.insert(port.incoming.end(), bytes.begin(), bytes.begin() + size);
    poll();
    return size;
}
std::vector<uint8_t> VirtioSerial::takeOutput(size_t maximum, unsigned id) {
    auto &port = ports.at(id);
    size_t count = std::min(maximum, port.outgoing.size());
    std::vector<uint8_t> bytes;
    bytes.reserve(count);
    while (count--) {
        bytes.push_back(port.outgoing.front());
        port.outgoing.pop_front();
    }
    return bytes;
}
void VirtioSerial::poll() {
    process_queue(3);
    if (!controls.empty())
        process_queue(2);
    for (unsigned id = 0; id < ports.size(); ++id) {
        auto &port = ports[id];
        unsigned rx = id ? 2 + id * 2 : 0;
        process_queue(rx + 1);
        if (port.ready && port.guestOpen && port.hostOpen && !port.incoming.empty())
            process_queue(rx);
    }
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
                for (unsigned port = 0; port < ports.size(); ++port)
                    control(1, 1, {}, port);
            }
        } else if (id < ports.size() && deviceReady && event == 3) {
            auto &port = ports[id];
            if (value == 1 && !port.ready) {
                port.ready = true;
                control(7, 1, port.name, id);
                control(6, port.hostOpen, {}, id);
            }
        } else if (id < ports.size() && ports[id].ready && event == 6) {
            auto &port = ports[id];
            port.guestOpen = value != 0;
            if (!port.guestOpen)
                port.incoming.clear();
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
    auto &port = ports.at(queue < 2 ? 0 : (queue - 2) / 2);
    if (queue & 1) {
        if (chain.writable)
            throw std::runtime_error("Virtio serial TX must be readable");
        if (port.hostOpen && port.ready && chain.readable > limit - port.outgoing.size())
            return std::nullopt;
        auto bytes = chain.input(limit);
        if (port.hostOpen && port.ready)
            port.outgoing.insert(port.outgoing.end(), bytes.begin(), bytes.end());
        transmitted += bytes.size();
        return 0;
    }
    if (chain.readable || !chain.writable)
        throw std::runtime_error("Virtio serial RX must be writable");
    if (port.incoming.empty())
        return std::nullopt;
    size_t count = std::min(port.incoming.size(), chain.writable);
    std::vector<uint8_t> bytes;
    bytes.reserve(count);
    for (size_t i = 0; i < count; ++i)
        bytes.push_back(port.incoming[i]);
    chain.output(bytes);
    for (size_t i = 0; i < count; ++i)
        port.incoming.pop_front();
    received += count;
    return uint32_t(count);
}
} // namespace pc
