#include "guest_agent_socket.hpp"
#include "virtio_pci.hpp"
#include "virtio_serial.hpp"
#include "virtio_tool_fixture.hpp"
#include <filesystem>
#include <iostream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
using namespace pc;
using namespace tooltest;
int main() {
    Memory m;
    bool irq = false;
    unsigned interruptQueue = 0;
    VirtioSerial serial(m.ram, m.base, [&](bool v, unsigned q) {
        irq = v;
        interruptQueue = q;
    });
    auto setup = [&] { m.configure(serial, 4, 2); };
    auto control = [&](unsigned event, unsigned value) {
        m.put(0x20000, 0, 4);
        m.put(0x20004, event, 2);
        m.put(0x20006, value, 2);
        m.descriptor(3, 0, 0x20000, 8, 0);
        m.submit(serial, 3);
    };
    auto rxControl = [&] {
        m.descriptor(2, 0, 0x21000, 256, 2);
        m.submit(serial, 2);
        return unsigned(m.get(0x21004, 2));
    };
    auto handshake = [&] {
        control(0, 1);
        check(rxControl() == 1);
        control(3, 1);
        check(rxControl() == 7);
        std::string name(reinterpret_cast<char *>(m.ram.data() + 0x21008), 22);
        check(name == "org.qemu.guest_agent.0");
        check(rxControl() == 6);
        control(6, 1);
        check(serial.port_ready() && serial.guest_port_open());
    };
    setup();
    check(serial.read(8) == 3 && serial.read(0x104) == 1);
    serial.set_host_port_open(true);
    handshake();
    check(irq && interruptQueue == 3);
    serial.write(0x64, 3);
    check(!irq);
    std::string fromHost = "{\"execute\":\"guest-ping\"}\n";
    m.descriptor(0, 0, 0x22000, 8, 3, 1);
    m.descriptor(0, 1, 0x23000, 256, 2);
    m.submit(serial, 0);
    check(m.get(m.used(0) + 2, 2) == 0);
    check(serial.receive_from_host(std::span(reinterpret_cast<const uint8_t *>(fromHost.data()),
                                   fromHost.size())) == fromHost.size());
    check(m.get(m.used(0) + 2, 2) == 1 && m.get(m.used(0) + 8, 4) == fromHost.size());
    check(std::equal(fromHost.begin(), fromHost.begin() + 8, m.ram.begin() + 0x22000));
    check(std::equal(fromHost.begin() + 8, fromHost.end(), m.ram.begin() + 0x23000));
    m.put(0x24000, 0x0a7d7b, 3);
    m.descriptor(1, 0, 0x24000, 3, 0);
    m.submit(serial, 1);
    auto bytes = serial.take_guest_output(2);
    check(bytes == std::vector<uint8_t>({'{', '}'}));
    check(serial.take_guest_output(9) == std::vector<uint8_t>({'\n'}));
    check(m.get(m.used(1) + 8, 4) == 0);
    // Bounded TX preserves the unconsumed descriptor until the host drains it.
    m.descriptor(1, 0, 0x40000, 65536, 0);
    for (unsigned i = 0; i < 16; ++i)
        m.submit(serial, 1);
    auto count = m.get(m.used(1) + 2, 2);
    m.submit(serial, 1);
    check(m.get(m.used(1) + 2, 2) == count);
    check(serial.take_guest_output(65536).size() == 65536);
    serial.poll();
    check(m.get(m.used(1) + 2, 2) == count + 1);
    // Closing Windows ports can detach unused RX buffers before device reset.
    // Do not walk RX descriptors without input or after the port has closed.
    control(6, 0);
    m.descriptor(0, 0, 0x22000, 1, 3, 0);
    m.submit(serial, 0);
    serial.poll();
    check(serial.error().empty());
    serial.set_host_port_open(false);
    check(rxControl() == 6 && m.get(0x21006, 2) == 0);
    check(serial.receive_from_host(std::array<uint8_t, 1>{1}) == 0);
    // Bad trailing DMA is rejected before touching the first receive buffer.
    setup();
    serial.set_host_port_open(true);
    handshake();
    serial.receive_from_host(std::array<uint8_t, 2>{1, 2});
    m.descriptor(0, 0, 0x22000, 1, 3, 1);
    m.descriptor(0, 1, m.ram.size() + 1, 1, 2);
    m.submit(serial, 0);
    check(!serial.error().empty() && m.ram[0x22000] == 0 && m.get(m.used(0) + 2, 2) == 0);
    setup();
    m.descriptor(1, 0, 0x22000, 1, 1, 0);
    m.submit(serial, 1);
    check(!serial.error().empty());
    setup();
    m.descriptor(1, 0, 0x22000, 1, 4);
    m.submit(serial, 1);
    check(!serial.error().empty());
    setup();
    m.put(m.avail(1) + 2, 9, 2);
    serial.write(0x50, 1);
    check(!serial.error().empty());
    setup();
    serial.write(0x38, 16);
    check(!serial.error().empty());
    setup();
    control(0, 1);
    m.descriptor(2, 0, 0x21000, 4, 2);
    m.submit(serial, 2);
    check(!serial.error().empty());
    // Wrap both split-ring indices without replaying a descriptor.
    setup();
    m.descriptor(1, 0, 0x22000, 0, 0);
    for (unsigned i = 0; i < 65540; ++i)
        m.submit(serial, 1);
    check(serial.error().empty() && m.get(m.used(1) + 2, 2) == 4);
    // Real PCI register translation and multiple queue vectors.
    VirtioPciDevice pci([&](unsigned o, unsigned w) { return serial.read(o, w); },
                        [&](unsigned o, uint64_t v, unsigned w) { serial.write(o, v, w); }, 3,
                        0x07800001, 4, [](bool) {});
    check(pci.configRead(0, 4) == 0x10431af4);
    // Private socket creation, collision refusal and identity-safe cleanup.
    auto path = (std::filesystem::temp_directory_path() /
                 ("pcemu-agent-" + std::to_string(getpid()) + ".sock"))
                    .string();
    {
        GuestAgentSocket socket(path);
        struct stat info{};
        check(!stat(path.c_str(), &info) && (info.st_mode & 0777) == 0600);
        bool refused = false;
        try {
            GuestAgentSocket duplicate(path);
        } catch (const std::runtime_error &) {
            refused = true;
        }
        check(refused && std::filesystem::exists(path));
        setup();
        serial.set_host_port_open(false);
        handshake();
        int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
        check(client >= 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::copy(path.begin(), path.end(), address.sun_path);
        check(!connect(client, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
        socket.pump(serial);
        check(rxControl() == 6 && m.get(0x21006, 2) == 1);
        m.descriptor(0, 0, 0x22000, 256, 2);
        m.submit(serial, 0);
        check(send(client, "ping", 4, 0) == 4);
        socket.pump(serial);
        check(m.get(m.used(0) + 8, 4) == 4);
        m.put(0x24000, 0x676e6f70, 4);
        m.descriptor(1, 0, 0x24000, 4, 0);
        m.submit(serial, 1);
        socket.pump(serial);
        char reply[4];
        check(recv(client, reply, 4, 0) == 4 && std::string(reply, 4) == "pong");
        close(client);
        socket.pump(serial);
        check(rxControl() == 6 && m.get(0x21006, 2) == 0);
    }
    check(!std::filesystem::exists(path));
    // SPICE and QGA share one controller, but never data or open states.
    {
        Memory n;
        VirtioSerial multi(
            n.ram, n.base, [](bool, unsigned) {},
            std::vector<std::string>{"org.qemu.guest_agent.0", "com.redhat.spice.0"});
        n.configure(multi, 6, 2);
        check(multi.read(0x104) == 2);
        auto ctl = [&](unsigned id, unsigned event, unsigned value) {
            n.put(0x20000, id, 4);
            n.put(0x20004, event, 2);
            n.put(0x20006, value, 2);
            n.descriptor(3, 0, 0x20000, 8, 0);
            n.submit(multi, 3);
        };
        auto reply = [&](unsigned id, unsigned event) {
            n.descriptor(2, 0, 0x21000, 256, 2);
            n.submit(multi, 2);
            check(n.get(0x21000, 4) == id && n.get(0x21004, 2) == event);
        };
        multi.set_host_port_open(true, 1);
        ctl(0, 0, 1);
        reply(0, 1);
        reply(1, 1);
        for (unsigned id = 0; id < 2; ++id) {
            ctl(id, 3, 1);
            reply(id, 7);
            reply(id, 6);
            ctl(id, 6, 1);
        }
        check(multi.host_receive_capacity(0) == 0 && multi.host_receive_capacity(1) > 0);
        n.descriptor(4, 0, 0x22000, 8, 2);
        n.submit(multi, 4);
        check(multi.receive_from_host(std::array<uint8_t, 3>{1, 2, 3}, 1) == 3);
        check(n.get(0x22000, 3) == 0x030201 && n.get(n.used(0) + 2, 2) == 0);
        n.put(0x23000, 0x060504, 3);
        n.descriptor(5, 0, 0x23000, 3, 0);
        n.submit(multi, 5);
        check(multi.take_guest_output(10).empty());
        check(multi.take_guest_output(10, 1) == std::vector<uint8_t>({4, 5, 6}));
        multi.set_host_port_open(true, 0);
        reply(0, 6);
        multi.set_host_port_open(false, 1);
        reply(1, 6);
        check(multi.host_receive_capacity(1) == 0 && multi.host_receive_capacity(0) > 0);
        multi.write(0x70, 0);
        check(!multi.port_ready(0) && !multi.port_ready(1) && !multi.guest_port_open(1));
        check(multi.error().empty());
    }
    std::cout << checks << " virtio serial checks passed\n";
}
