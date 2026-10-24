#include "machine.hpp"
#include "network.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
using namespace pc;
using Bytes = std::vector<uint8_t>;
static unsigned checks;
static void check(bool v, const char *s) {
    ++checks;
    if (!v)
        throw std::runtime_error(s);
}
static void put16(uint8_t *p, unsigned v) {
    p[0] = v >> 8;
    p[1] = v;
}
static void put32(uint8_t *p, uint32_t v) {
    put16(p, v >> 16);
    put16(p + 2, v);
}
static unsigned get16(const uint8_t *p) {
    return unsigned(p[0]) * 256 + p[1];
}
static uint32_t get32(const uint8_t *p) {
    return uint32_t(get16(p)) * 65536 + get16(p + 2);
}
static uint16_t sum(const uint8_t *p, size_t n, uint32_t s = 0) {
    for (; n >= 2; n -= 2, p += 2)
        s += get16(p);
    if (n)
        s += *p << 8;
    while (s >> 16)
        s = (s & 65535) + (s >> 16);
    return ~s;
}
constexpr uint32_t guest = 0x0a00020f, gateway = 0x0a000202;
static void configure(NE2000 &n) {
    n.out(0x300, 0x21, 1);
    n.out(0x301, 0x46, 1);
    n.out(0x302, 0x80, 1);
    n.out(0x303, 0x46, 1);
    n.out(0x30c, 0x14, 1);
    n.out(0x30e, 0x49, 1);
    n.out(0x300, 0x61, 1);
    n.out(0x307, 0x47, 1);
    n.out(0x300, 0x22, 1);
    n.out(0x307, 0x7f, 1);
}
static Bytes take(NE2000 &n, uint8_t &page) {
    if (page == n.current)
        return {};
    unsigned a = unsigned(page) * 256;
    unsigned len = get16(std::array<uint8_t, 2>{n.memory[a + 3], n.memory[a + 2]}.data());
    check(len >= 64 && len <= 1522, "ring count includes Ethernet FCS");
    Bytes p;
    unsigned pos = a + 4;
    for (unsigned i = 0; i < len - 4; ++i) {
        p.push_back(n.memory[pos++]);
        if (pos == 0x8000)
            pos = 0x4600;
    }
    page = n.memory[a + 1];
    n.boundary = page == n.start ? n.stop - 1 : page - 1;
    return p;
}
static Bytes ipv4(uint8_t proto, uint32_t dst, Bytes payload) {
    Bytes p(34 + payload.size());
    std::fill_n(p.data(), 6, 255);
    const uint8_t mac[] = {2, 0x50, 0x43, 0x45, 0x4d, 1};
    std::copy_n(mac, 6, p.data() + 6);
    put16(p.data() + 12, 0x800);
    auto *h = p.data() + 14;
    h[0] = 0x45;
    h[8] = 64;
    h[9] = proto;
    put16(h + 2, 20 + payload.size());
    put32(h + 12, guest);
    put32(h + 16, dst);
    put16(h + 10, sum(h, 20));
    std::copy(payload.begin(), payload.end(), p.begin() + 34);
    return p;
}
static Bytes udp(uint32_t dst, uint16_t sp, uint16_t dp, const Bytes &data) {
    Bytes p(8 + data.size());
    put16(p.data(), sp);
    put16(p.data() + 2, dp);
    put16(p.data() + 4, p.size());
    std::copy(data.begin(), data.end(), p.begin() + 8);
    return ipv4(17, dst, p);
}
static Bytes tcp(uint16_t dp, uint32_t seq, uint32_t ack, uint8_t flags, const Bytes &data = {}) {
    Bytes p(20 + data.size());
    put16(p.data(), 12345);
    put16(p.data() + 2, dp);
    put32(p.data() + 4, seq);
    put32(p.data() + 8, ack);
    p[12] = 0x50;
    p[13] = flags;
    put16(p.data() + 14, 32768);
    std::copy(data.begin(), data.end(), p.begin() + 20);
    put16(p.data() + 16, sum(p.data(), p.size(),
                             (guest >> 16) + (guest & 65535) + (gateway >> 16) + (gateway & 65535) +
                                 6 + p.size()));
    return ipv4(6, gateway, p);
}
int main() {
    auto path =
        std::filesystem::temp_directory_path() / ("pcemu-network-" + std::to_string(getpid()));
    try {
        NE2000 n;
        check(n.in(0x307, 1) == 0x80, "power-on reset status");
        n.out(0x308, 0, 1);
        n.out(0x309, 0, 1);
        n.out(0x30a, 32, 1);
        n.out(0x30b, 0, 1);
        n.out(0x300, 0x0a, 1);
        check(n.in(0x310, 2) == 0x0202, "duplicated station PROM");
        for (unsigned i = 2; i < 32; ++i)
            n.in(0x310, 1);
        check(n.remoteCount == 0 && (n.isr & 0x40), "remote DMA completion");
        check(n.in(0x310, 2) == 65535, "completed DMA does not advance");
        n.out(0x307, 0x40, 1);
        check(!(n.isr & 0x40), "ISR write-one-clear");
        n.out(0x308, 0, 1);
        n.out(0x309, 0x40, 1);
        n.out(0x30a, 3, 1);
        n.out(0x300, 0x12, 1);
        n.out(0x310, 0x44332211, 4);
        check(n.memory[0x4000] == 0x11 && n.memory[0x4002] == 0x33 && n.remoteCount == 0,
              "odd DMA count cannot overrun");
        n.reset();
        n.dcr = 1;
        n.remoteCount = 12;
        n.command = 0x0a;
        for (uint8_t expected : n.physical)
            check(n.in(0x310, 1) == expected, "WTS byte reads consume full PROM words");
        check(n.remote == 12 && !n.remoteCount, "WTS word count and address");
        configure(n);
        Bytes frame(60, 0x41);
        std::copy(n.physical.begin(), n.physical.end(), frame.begin());
        check(n.receive(frame.data(), frame.size()), "unicast packet accepted");
        check(n.memory[0x4700] == 1 && n.memory[0x4701] == 0x48 && n.memory[0x4702] == 64,
              "receive ring header");
        check(n.isr & 1, "receive IRQ status");
        n.out(0x30f, 1, 1);
        check(n.irq(), "interrupt mask enables pending IRQ");
        n.out(0x307, 1, 1);
        check(!n.irq(), "acknowledged interrupt deasserts");
        frame[0] ^= 2;
        n.rcr = 0;
        check(!n.receive(frame.data(), frame.size()), "unicast address filtering");
        std::fill_n(frame.data(), 6, 255);
        check(!n.receive(frame.data(), frame.size()), "broadcast filter disabled");
        n.rcr = 4;
        check(n.receive(frame.data(), frame.size()), "broadcast filter enabled");
        n.rcr = 0x10;
        n.current = 0x7f;
        n.boundary = 0x49;
        frame.resize(600);
        check(n.receive(frame.data(), frame.size()) && n.current == 0x48 - 0 /* next 48 */,
              "ring wraps at PSTOP");
        // The protected boundary must reject an incoming packet before writes.
        n.current = 0x47;
        n.boundary = 0x48;
        auto before = n.memory;
        check(!n.receive(frame.data(), frame.size()) && n.memory == before && (n.isr & 0x10),
              "overflow preserves unread ring");
        configure(n);
        n.remote = 0x7fff;
        n.remoteCount = 2;
        n.command = 0x0a;
        n.memory[0x7fff] = 0xab;
        n.memory[0x4600] = 0xcd;
        check(n.in(0x310, 2) == 0xcdab && n.remote == 0x4601, "remote DMA ring wrap");
        n.command = 0x22;
        n.transmitPage = 0x40;
        n.transmitCount = 60;
        std::copy_n(frame.data(), 60, n.memory.data() + 0x4000);
        n.out(0x300, 0x26, 1);
        check(n.outgoing.size() == 1 && (n.isr & 2) && n.transmitStatus == 1,
              "transmit DMA and PTX");
        n.outgoing.clear();
        Machine m("", "", 2 * 1024 * 1024);
        m.nic = n;
        m.nic.imr = 2;
        m.nicLine = false;
        m.poll();
        check(m.pic[1].irr & 4, "NE2000 routed to IRQ10");
        m.nic.remote = 0x4567;
        m.nic.remoteCount = 17;
        m.nic.outgoing.push_back(Bytes(60, 0x55));
        m.saveSnapshot(path.string());
        m.nic.reset();
        m.loadSnapshot(path.string());
        check(m.nic.remote == 0x4567 && m.nic.remoteCount == 17 && m.nic.memory == n.memory &&
                  m.nic.outgoing == std::deque<Bytes>({Bytes(60, 0x55)}),
              "NIC checkpoint roundtrip including wire queue");
        configure(n);
        Network net;
        uint8_t page = n.current;
        auto pump = [&](Bytes p) {
            n.outgoing.push_back(std::move(p));
            net.pump(n, 1, 0);
        };
        Bytes arp(42);
        std::fill_n(arp.data(), 6, 255);
        std::copy(n.physical.begin(), n.physical.end(), arp.begin() + 6);
        put16(arp.data() + 12, 0x806);
        auto *a = arp.data() + 14;
        put16(a, 1);
        put16(a + 2, 0x800);
        a[4] = 6;
        a[5] = 4;
        put16(a + 6, 1);
        std::copy(n.physical.begin(), n.physical.end(), arp.begin() + 22);
        put32(a + 14, guest);
        put32(a + 24, gateway);
        pump(arp);
        auto reply = take(n, page);
        check(reply.size() >= 42 && get16(reply.data() + 20) == 2 &&
                  get32(reply.data() + 28) == gateway,
              "ARP gateway reply");
        Bytes echo(12);
        echo[0] = 8;
        echo[8] = 42;
        put16(echo.data() + 2, sum(echo.data(), echo.size()));
        pump(ipv4(1, gateway, echo));
        reply = take(n, page);
        check(reply.size() >= 46 && reply[34] == 0 && reply[42] == 42 &&
                  !sum(reply.data() + 34, 12),
              "ICMP gateway echo checksum");
        Bytes discover(244);
        discover[0] = 1;
        discover[1] = 1;
        discover[2] = 6;
        put32(discover.data() + 4, 0x12345678);
        std::copy(n.physical.begin(), n.physical.end(), discover.begin() + 28);
        put32(discover.data() + 236, 0x63825363);
        discover[240] = 53;
        discover[241] = 1;
        discover[242] = 1;
        discover[243] = 255;
        pump(udp(0xffffffff, 68, 67, discover));
        reply = take(n, page);
        check(reply.size() >= 342 && get32(reply.data() + 58) == guest && reply[284] == 2,
              "DHCP OFFER with guest address");
        discover[242] = 3;
        pump(udp(0xffffffff, 68, 67, discover));
        reply = take(n, page);
        check(reply.size() >= 342 && reply[284] == 5, "DHCP ACK");
        uint32_t seq = 1000;
        pump(tcp(80, seq, 0, 2));
        reply = take(n, page);
        check(reply.size() >= 58 && reply[47] == 0x12 && get32(reply.data() + 42) == seq + 1,
              "TCP SYN ACK");
        uint32_t ack = get32(reply.data() + 38) + 1;
        ++seq;
        pump(tcp(80, seq, ack, 16));
        std::string req = "GET / HTTP/1.0\r\n\r\n";
        pump(tcp(80, seq, ack, 24, Bytes(req.begin(), req.end())));
        seq += req.size();
        reply = take(n, page);
        check(!reply.empty() && reply[47] == 16, "TCP request acknowledged");
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        net.pump(n, 1, 0);
        reply = take(n, page);
        check(reply.size() > 54 &&
                  std::string(reply.begin() + 54, reply.end()).find("NE2000 works") !=
                      std::string::npos,
              "HTTP gateway response");
        auto response = reply;
        std::this_thread::sleep_for(std::chrono::milliseconds(510));
        net.pump(n, 1, 0);
        reply = take(n, page);
        check(reply.size() == response.size() && get32(reply.data() + 38) == ack,
              "unacknowledged TCP data retransmitted");
        ack += response.size() - 54;
        pump(tcp(80, seq, ack, 16));
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        net.pump(n, 1, 0);
        reply = take(n, page);
        check(!reply.empty() && (reply[47] & 1), "HTTP closes with FIN after response ACK");
        ++ack;
        pump(tcp(80, seq, ack, 17));
        reply = take(n, page);
        check(!reply.empty() && get32(reply.data() + 42) == seq + 1, "peer FIN acknowledged");
        // Exercise actual host UDP and TCP sockets, without depending on Internet services.
        int listener = socket(AF_INET, SOCK_STREAM, 0), datagram = socket(AF_INET, SOCK_DGRAM, 0);
        check(listener >= 0 && datagram >= 0, "host sockets created");
        timeval timeout{2, 0};
        setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(datagram, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 &&
                  listen(listener, 1) == 0,
              "host TCP listener");
        socklen_t alen = sizeof(address);
        getsockname(listener, reinterpret_cast<sockaddr *>(&address), &alen);
        uint16_t port = ntohs(address.sin_port);
        pump(tcp(port, 2000, 0, 2));
        int client = accept(listener, nullptr, nullptr);
        check(client >= 0, "gateway opens host TCP connection");
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        net.pump(n, 1, 0);
        reply = take(n, page);
        check(reply.size() >= 58 && reply[47] == 0x12, "host connect completes guest handshake");
        uint32_t hostAck = get32(reply.data() + 38) + 1;
        pump(tcp(port, 2001, hostAck, 16));
        Bytes payload = {'p', 'i', 'n', 'g'};
        pump(tcp(port, 2001, hostAck, 24, payload));
        take(n, page);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        net.pump(n, 1, 0);
        take(n, page);
        char received[4];
        check(recv(client, received, 4, 0) == 4 && std::string(received, 4) == "ping",
              "guest payload forwarded to host socket");
        check(send(client, "pong", 4, 0) == 4, "host response sent");
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        net.pump(n, 1, 0);
        reply = take(n, page);
        check(reply.size() >= 58 && std::string(reply.begin() + 54, reply.begin() + 58) == "pong",
              "host response returned over guest TCP");
        pump(tcp(port, 2005, hostAck + 4, 16));
        shutdown(client, SHUT_WR);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        net.pump(n, 1, 0);
        reply = take(n, page);
        check(!reply.empty() && (reply[47] & 1), "host EOF translated to guest FIN");
        pump(tcp(port, 2005, hostAck + 5, 17));
        take(n, page);
        close(client);
        close(listener);
        address.sin_port = 0;
        check(bind(datagram, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
              "host UDP listener");
        getsockname(datagram, reinterpret_cast<sockaddr *>(&address), &alen);
        port = ntohs(address.sin_port);
        pump(udp(gateway, 12346, port, payload));
        sockaddr_in remote{};
        socklen_t rlen = sizeof(remote);
        check(recvfrom(datagram, received, 4, 0, reinterpret_cast<sockaddr *>(&remote), &rlen) == 4,
              "guest UDP forwarded");
        check(sendto(datagram, "pong", 4, 0, reinterpret_cast<sockaddr *>(&remote), rlen) == 4,
              "host UDP reply sent");
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        net.pump(n, 1, 0);
        reply = take(n, page);
        check(reply.size() >= 46 && std::string(reply.begin() + 42, reply.begin() + 46) == "pong",
              "host UDP reply returned");
        close(datagram);
        net.pump(n, 2, 0);
        pump(tcp(80, seq + 1, ack, 16));
        reply = take(n, page);
        check(!reply.empty() && (reply[47] & 4), "stale TCP after restore receives reset");
        // Malformed IP/transport inputs must not generate packets or consume ring memory.
        auto count = n.received;
        for (unsigned len = 0; len < 60; ++len)
            pump(Bytes(len, 0xff));
        auto bad = ipv4(1, gateway, echo);
        bad[24] ^= 1;
        pump(bad);
        check(n.received == count, "malformed/truncated traffic rejected");
        std::filesystem::remove_all(path);
        std::cout << checks << " network checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << "Network test: " << e.what() << '\n';
        std::filesystem::remove_all(path);
        return 1;
    }
}
