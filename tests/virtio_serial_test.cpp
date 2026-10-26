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
        check(serial.portReady() && serial.portOpen());
    };
    setup();
    check(serial.read(8) == 3 && serial.read(0x104) == 1);
    serial.hostConnected(true);
    handshake();
    check(irq && interruptQueue == 3);
    serial.write(0x64, 3);
    check(!irq);
    std::string fromHost = "{\"execute\":\"guest-ping\"}\n";
    m.descriptor(0, 0, 0x22000, 8, 3, 1);
    m.descriptor(0, 1, 0x23000, 256, 2);
    m.submit(serial, 0);
    check(m.get(m.used(0) + 2, 2) == 0);
    check(serial.receive(std::span(reinterpret_cast<const uint8_t *>(fromHost.data()),
                                   fromHost.size())) == fromHost.size());
    check(m.get(m.used(0) + 2, 2) == 1 && m.get(m.used(0) + 8, 4) == fromHost.size());
    check(std::equal(fromHost.begin(), fromHost.begin() + 8, m.ram.begin() + 0x22000));
    check(std::equal(fromHost.begin() + 8, fromHost.end(), m.ram.begin() + 0x23000));
    m.put(0x24000, 0x0a7d7b, 3);
    m.descriptor(1, 0, 0x24000, 3, 0);
    m.submit(serial, 1);
    auto bytes = serial.takeOutput(2);
    check(bytes == std::vector<uint8_t>({'{', '}'}));
    check(serial.takeOutput(9) == std::vector<uint8_t>({'\n'}));
    check(m.get(m.used(1) + 8, 4) == 0);
    // Bounded TX preserves the unconsumed descriptor until the host drains it.
    m.descriptor(1, 0, 0x40000, 65536, 0);
    for (unsigned i = 0; i < 16; ++i)
        m.submit(serial, 1);
    auto count = m.get(m.used(1) + 2, 2);
    m.submit(serial, 1);
    check(m.get(m.used(1) + 2, 2) == count);
    check(serial.takeOutput(65536).size() == 65536);
    serial.poll();
    check(m.get(m.used(1) + 2, 2) == count + 1);
    // Closing Windows ports can detach unused RX buffers before device reset.
    // Do not walk RX descriptors without input or after the port has closed.
    control(6, 0);
    m.descriptor(0, 0, 0x22000, 1, 3, 0);
    m.submit(serial, 0);
    serial.poll();
    check(serial.error().empty());
    serial.hostConnected(false);
    check(rxControl() == 6 && m.get(0x21006, 2) == 0);
    check(serial.receive(std::array<uint8_t, 1>{1}) == 0);
    // Bad trailing DMA is rejected before touching the first receive buffer.
    setup();
    serial.hostConnected(true);
    handshake();
    serial.receive(std::array<uint8_t, 2>{1, 2});
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
        serial.hostConnected(false);
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
    std::cout << checks << " virtio serial checks passed\n";
}
