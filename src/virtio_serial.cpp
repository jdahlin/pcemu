// Virtio console provides byte streams over paired receive/transmit virtqueues.
// This model uses its MULTIPORT feature to expose named generic serial ports,
// including guest-agent channels. It carries bytes; it does not parse the guest
// agent's application protocol. Receive/transmit are named from the guest's view:
// host input fills receive buffers and guest output arrives on transmit queues.
// The shared Virtio core owns transport registers, split descriptors and interrupts.
//
// After DEVICE_READY the device announces each port with DEVICE_ADD. PORT_READY
// elicits its UTF-8 PORT_NAME and host PORT_OPEN state. The guest also sends
// PORT_OPEN; host input is accepted only while both ends are open and the port is
// ready. Independent bounded byte queues isolate ports. When output fills, the
// transmit chain is deferred until a later poll; receive chains wait for input.
// Control receive buffers likewise wait until an announcement is queued.
//
// IMPLEMENTATION NOTE: Port definitions are fixed at construction (one to eight
// unique names). Names and pending control messages have host resource bounds;
// no resize, console designation, emergency write or hot-unplug is implemented.
// Host connection transitions discard queued bytes for that port. Device reset
// clears guest handshake/data state but retains host connection state and names.
// COMPATIBILITY: Transmit buffers received before a host connection/PORT_READY
// are consumed and discarded; they are still counted in transmitted. Generic
// ports require the multiport handshake; a single-port legacy console is absent.
// Virtio 1.2, 5.3 Console Device, especially 5.3.6.2 Multiport Device Operation.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2900003
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3000002

#include "virtio_serial.hpp"
#include <algorithm>
#include <stdexcept>
namespace pc {
namespace {
// Virtio 1.2, 5.3.1-5.3.3: console device identity and MULTIPORT feature bit.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2900003
constexpr uint32_t ConsoleDeviceId = 3;
constexpr unsigned MultiportFeatureBit = 1;
constexpr uint64_t OfferedFeatures = 1ull << MultiportFeatureBit;
// Queue numbering, 5.3.2: port 0 uses 0/1, control uses 2/3, port n>0 uses
// 2+2*n and 3+2*n. Even data queues receive, odd data queues transmit.
enum QueueIndex : unsigned {
    FirstPortReceiveQueue = 0,
    FirstPortTransmitQueue = 1,
    ControlReceiveQueue = 2,
    ControlTransmitQueue = 3,
};
constexpr unsigned QueuesPerPort = 2, ControlQueueCount = 2;
// max_nr_ports is a little-endian u32 at byte offset 4 in virtio_console_config;
// unoffered cols/rows/emergency-write fields read zero.
constexpr unsigned MaximumPortsConfigurationOffset = 4;
// Packed virtio_console_control: port id:u32, event:u16, value:u16, then optional text.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3000002
enum ControlField : unsigned { PortId = 0, Event = 4, Value = 6 };
constexpr unsigned ControlHeaderBytes = 8, BitsPerByte = 8;
enum ControlEvent : uint16_t {
    DeviceReady = 0, // Driver can receive control messages; value 1 means success.
    DeviceAdd = 1,   // Device announces a port; value is unused.
    PortReady = 3,   // Driver initialized the announced port; value 1 means success.
    PortOpen = 6,    // Sent by either end; value 0/1 means closed/open.
    PortName = 7,    // Device supplies a UTF-8 tag after the header, without a NUL.
};
// IMPLEMENTATION NOTE: Bound port/name storage and pending control announcements.
constexpr size_t MaximumPorts = 8, MaximumPortNameBytes = 128, MaximumPendingControls = 128;
} // namespace

// Construct fixed named generic ports and their data/control queues.
// Virtio 1.2, 5.3; buffering limits and host connection lifecycle are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2900003
VirtioSerial::VirtioSerial(std::span<uint8_t> ram, uint64_t base, Interrupt irq,
                           std::string portName)
    : VirtioSerial(ram, base, std::move(irq), std::vector<std::string>{std::move(portName)}) {}
// Construct fixed named generic ports and their data/control queues.
// Virtio 1.2, 5.3; buffering limits and host connection lifecycle are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2900003
VirtioSerial::VirtioSerial(std::span<uint8_t> ram, uint64_t base, Interrupt irq,
                           std::vector<std::string> names)
    : VirtioDevice(ram, base, ConsoleDeviceId,
                   unsigned(ControlQueueCount + names.size() * QueuesPerPort), OfferedFeatures,
                   std::move(irq)) {
    if (names.empty() || names.size() > MaximumPorts)
        throw std::runtime_error("Invalid virtio serial port count");
    for (auto &name : names) {
        if (name.empty() || name.size() > MaximumPortNameBytes ||
            name.find('\0') != std::string::npos ||
            std::any_of(ports.begin(), ports.end(), [&](const Port &p) { return p.name == name; }))
            throw std::runtime_error("Invalid or duplicate virtio serial port name");
        ports.push_back(Port{std::move(name), false, false, false, {}, {}});
    }
}
// Read max_nr_ports bytes; unoffered size/emergency fields remain zero.
// Virtio 1.2, 5.3; buffering limits and host connection lifecycle are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2940004
uint64_t VirtioSerial::read_device_configuration(unsigned offset, unsigned width) const {
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        if (offset + i == MaximumPortsConfigurationOffset)
            value |= uint64_t(ports.size()) << (i * BitsPerByte);
    return value; // size and emergency output are not advertised.
}
// Clear guest handshake, control and data queues; preserve names and host connection state.
// Virtio 1.2, 5.3; buffering limits and host connection lifecycle are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2960005
void VirtioSerial::reset_device() {
    deviceReady = false;
    controls.clear();
    for (auto &port : ports) {
        port.guestOpen = port.ready = false;
        port.incoming.clear();
        port.outgoing.clear();
    }
}
// Queue a little-endian control header and optional UTF-8 name; reject host backlog overflow.
// Virtio 1.2, 5.3; buffering limits and host connection lifecycle are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3000002
void VirtioSerial::queue_control_message(uint16_t event, uint16_t value, const std::string &text,
                                         unsigned port) {
    if (controls.size() >= MaximumPendingControls)
        throw std::runtime_error("Virtio serial control queue overflow");
    std::vector<uint8_t> bytes(ControlHeaderBytes, 0);
    bytes[ControlField::PortId] = uint8_t(port);
    bytes[ControlField::Event] = uint8_t(event);
    bytes[ControlField::Event + 1] = uint8_t(event >> BitsPerByte);
    bytes[ControlField::Value] = uint8_t(value);
    bytes[ControlField::Value + 1] = uint8_t(value >> BitsPerByte);
    bytes.insert(bytes.end(), text.begin(), text.end());
    controls.push_back(std::move(bytes));
}
// Change host connection state, discard stale bytes and announce PORT_OPEN when ready.
// Virtio 1.2, 5.3; buffering limits and host connection lifecycle are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3000002
void VirtioSerial::set_host_port_open(bool connected, unsigned id) {
    auto &port = ports.at(id);
    if (port.hostOpen == connected)
        return;
    port.hostOpen = connected;
    port.incoming.clear();
    port.outgoing.clear();
    if (port.ready)
        queue_control_message(ControlEvent::PortOpen, connected, {}, id);
    poll();
}
// Report bounded host-input capacity only while the port is ready and both ends are open.
// Virtio 1.2, 5.3; buffering limits and host connection lifecycle are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2980006
size_t VirtioSerial::host_receive_capacity(unsigned id) const {
    const auto &port = ports.at(id);
    return port.ready && port.guestOpen && port.hostOpen ? PortBufferBytes - port.incoming.size()
                                                         : 0;
}
// Enqueue host bytes up to capacity, then service guest queues; return accepted byte count.
// Virtio 1.2, 5.3; buffering limits and host connection lifecycle are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2980006
size_t VirtioSerial::receive_from_host(std::span<const uint8_t> bytes, unsigned id) {
    auto &port = ports.at(id);
    auto size = std::min(bytes.size(), host_receive_capacity(id));
    port.incoming.insert(port.incoming.end(), bytes.begin(), bytes.begin() + size);
    poll();
    return size;
}
// Remove at most maximum bytes from the selected port’s guest-output backlog.
// Virtio 1.2, 5.3; buffering limits and host connection lifecycle are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2980006
std::vector<uint8_t> VirtioSerial::take_guest_output(size_t maximum, unsigned id) {
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
// Service control then per-port data queues, retaining chains blocked on bytes/backpressure.
// Virtio 1.2, 5.3; buffering limits and host connection lifecycle are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-2980006
void VirtioSerial::poll() {
    process_queue(QueueIndex::ControlTransmitQueue);
    if (!controls.empty())
        process_queue(QueueIndex::ControlReceiveQueue);
    for (unsigned id = 0; id < ports.size(); ++id) {
        auto &port = ports[id];
        unsigned rx =
            id ? ControlQueueCount + id * QueuesPerPort : QueueIndex::FirstPortReceiveQueue;
        process_queue(rx + 1);
        if (port.ready && port.guestOpen && port.hostOpen && !port.incoming.empty())
            process_queue(rx);
    }
}
// Decode control handshakes or transfer a data chain, returning no completion when deferred.
// Virtio 1.2, 5.3; buffering limits and host connection lifecycle are host policy.
// https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3000002
std::optional<uint32_t> VirtioSerial::handle(unsigned queue, const Chain &chain) {
    if (queue == QueueIndex::ControlTransmitQueue) {
        if (!negotiated(MultiportFeatureBit) || chain.writable ||
            chain.readable != ControlHeaderBytes)
            throw std::runtime_error("Invalid virtio serial control request");
        auto data = chain.input(ControlHeaderBytes);
        unsigned id = unsigned(data[ControlField::PortId]) |
                      (unsigned(data[ControlField::PortId + 1]) << 8) |
                      (unsigned(data[ControlField::PortId + 2]) << 16) |
                      (unsigned(data[ControlField::PortId + 3]) << 24);
        unsigned event = data[ControlField::Event] | (unsigned(data[ControlField::Event + 1]) << 8),
                 value = data[ControlField::Value] | (unsigned(data[ControlField::Value + 1]) << 8);
        if (event == ControlEvent::DeviceReady) {
            if (value == 1 && !deviceReady) {
                deviceReady = true;
                for (unsigned port = 0; port < ports.size(); ++port)
                    queue_control_message(ControlEvent::DeviceAdd, 1, {}, port);
            }
        } else if (id < ports.size() && deviceReady && event == ControlEvent::PortReady) {
            auto &port = ports[id];
            if (value == 1 && !port.ready) {
                port.ready = true;
                queue_control_message(ControlEvent::PortName, 1, port.name, id);
                queue_control_message(ControlEvent::PortOpen, port.hostOpen, {}, id);
            }
        } else if (id < ports.size() && ports[id].ready && event == ControlEvent::PortOpen) {
            auto &port = ports[id];
            port.guestOpen = value != 0;
            if (!port.guestOpen)
                port.incoming.clear();
        }
        return 0;
    }
    if (queue == QueueIndex::ControlReceiveQueue) {
        if (chain.readable || !chain.writable)
            throw std::runtime_error("Invalid virtio serial control buffer");
        if (controls.empty())
            return std::nullopt;
        auto length = controls.front().size();
        chain.output(controls.front());
        controls.pop_front();
        return uint32_t(length);
    }
    auto &port =
        ports.at(queue < ControlQueueCount ? 0 : (queue - ControlQueueCount) / QueuesPerPort);
    if (queue & 1) {
        if (chain.writable)
            throw std::runtime_error("Virtio serial TX must be readable");
        if (port.hostOpen && port.ready && chain.readable > PortBufferBytes - port.outgoing.size())
            return std::nullopt;
        auto bytes = chain.input(PortBufferBytes);
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
