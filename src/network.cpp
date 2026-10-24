#include "network.hpp"
#include "ne2000.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
namespace pc {
namespace {
using Bytes = std::vector<uint8_t>;
uint16_t u16(const uint8_t *p) {
    return uint16_t(p[0]) * 256 + p[1];
}
uint32_t u32(const uint8_t *p) {
    return uint32_t(u16(p)) * 65536 + u16(p + 2);
}
void put16(uint8_t *p, uint16_t v) {
    p[0] = v >> 8;
    p[1] = v;
}
void put32(uint8_t *p, uint32_t v) {
    put16(p, v >> 16);
    put16(p + 2, v);
}
uint16_t checksum(const uint8_t *p, size_t n, uint32_t sum = 0) {
    for (; n >= 2; n -= 2, p += 2)
        sum += u16(p);
    if (n)
        sum += unsigned(*p) << 8;
    while (sum >> 16)
        sum = (sum & 65535) + (sum >> 16);
    return ~sum;
}
uint64_t nowMS() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
constexpr uint32_t guestIP = 0x0a00020f, gatewayIP = 0x0a000202, dnsIP = 0x0a000203;
constexpr uint8_t gatewayMAC[] = {0x02, 0x50, 0x43, 0x45, 0x4d, 0x02};
int socketFor(int type) {
    int fd = socket(AF_INET, type, 0);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    }
    return fd;
}
struct Flow {
    int fd = -1;
    uint32_t source = 0, destination = 0;
    uint16_t sourcePort = 0, destinationPort = 0;
    uint64_t last = 0, sentAt = 0;
    uint32_t receiveNext = 0, sendNext = 0, unacknowledged = 0;
    uint16_t window = 65535;
    bool tcp = false, connecting = false, established = false, local = false;
    bool peerFin = false, hostFin = false, finSent = false, incoming = false;
    uint8_t pendingFlags = 0;
    Bytes pending, toHost;
    ~Flow() {
        if (fd >= 0)
            close(fd);
    }
};
} // namespace
struct Network::State {
    std::vector<std::unique_ptr<Flow>> flows;
    std::array<uint8_t, 6> guestMAC{};
    uint64_t epoch = UINT64_MAX, resets = UINT64_MAX, nextPoll = 0;
    uint32_t dns = 0x01010101, serial = 0x50430000;
    uint16_t ipID = 0;
    Network::Receiver receive;
    int listener = -1;
    uint16_t forwardedPort = 22, incomingPort = 40000;
    ~State() { if (listener >= 0) close(listener); }
    uint16_t forwardTCP(uint16_t hostPort, uint16_t guestPort) {
        if (listener >= 0 || !guestPort) throw std::runtime_error("Invalid or duplicate TCP forward");
        int fd = socketFor(SOCK_STREAM);
        if (fd < 0) throw std::runtime_error("Cannot create TCP forward socket");
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(hostPort);
        if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) || listen(fd, 16)) {
            close(fd); throw std::runtime_error("Cannot bind loopback TCP forward");
        }
        socklen_t size = sizeof(address);
        if (getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size)) {
            close(fd); throw std::runtime_error("Cannot inspect TCP forward port");
        }
        listener = fd; forwardedPort = guestPort;
        return ntohs(address.sin_port);
    }
    std::ofstream capture;
    explicit State(const std::string &path) {
        std::ifstream resolvers("/etc/resolv.conf");
        std::string line;
        while (std::getline(resolvers, line)) {
            if (line.starts_with("nameserver ")) {
                in_addr a{};
                auto value = line.substr(11);
                value = value.substr(0, value.find_first_of(" \t\r"));
                if (inet_pton(AF_INET, value.c_str(), &a) == 1) {
                    dns = ntohl(a.s_addr);
                    break;
                }
            }
        }
        if (!path.empty()) {
            capture.open(path, std::ios::binary);
            if (!capture)
                throw std::runtime_error("Cannot open network capture " + path);
            const uint32_t header[] = {0xa1b2c3d4, 0x00040002, 0, 0, 65535, 1};
            capture.write(reinterpret_cast<const char *>(header), sizeof(header));
        }
    }
    void trace(const Bytes &p) {
        if (!capture)
            return;
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
        uint32_t h[] = {uint32_t(us / 1000000), uint32_t(us % 1000000), uint32_t(p.size()),
                        uint32_t(p.size())};
        capture.write(reinterpret_cast<char *>(h), sizeof(h));
        capture.write(reinterpret_cast<const char *>(p.data()), p.size());
        capture.flush();
    }
    void deliver(Bytes p) {
        p.resize(std::max<size_t>(60, p.size()), 0);
        trace(p);
        receive(p.data(), p.size());
    }
    Bytes ethernet(uint16_t type, size_t size, bool broadcast = false) {
        Bytes p(14 + size);
        if (broadcast)
            std::fill_n(p.data(), 6, 255);
        else
            std::copy(guestMAC.begin(), guestMAC.end(), p.begin());
        std::copy_n(gatewayMAC, 6, p.data() + 6);
        put16(p.data() + 12, type);
        return p;
    }
    void ip(uint32_t source, uint32_t dest, uint8_t protocol, const Bytes &payload,
            bool broadcast = false) {
        auto p = ethernet(0x800, 20 + payload.size(), broadcast);
        auto *h = p.data() + 14;
        h[0] = 0x45;
        put16(h + 2, 20 + payload.size());
        put16(h + 4, ++ipID);
        h[8] = 64;
        h[9] = protocol;
        put32(h + 12, source);
        put32(h + 16, dest);
        put16(h + 10, checksum(h, 20));
        std::copy(payload.begin(), payload.end(), p.begin() + 34);
        deliver(std::move(p));
    }
    uint16_t transportChecksum(uint32_t src, uint32_t dst, uint8_t proto, const Bytes &p) {
        return checksum(p.data(), p.size(),
                        (src >> 16) + (src & 65535) + (dst >> 16) + (dst & 65535) + proto +
                            uint32_t(p.size()));
    }
    void udp(uint32_t source, uint32_t dest, uint16_t sport, uint16_t dport, const Bytes &data,
             bool broadcast = false) {
        Bytes p(8 + data.size());
        put16(p.data(), sport);
        put16(p.data() + 2, dport);
        put16(p.data() + 4, p.size());
        std::copy(data.begin(), data.end(), p.begin() + 8);
        uint16_t c = transportChecksum(source, dest, 17, p);
        put16(p.data() + 6, c ? c : 65535);
        ip(source, dest, 17, p, broadcast);
    }
    void tcp(Flow &f, uint8_t flags, const Bytes &data = {}, bool remember = false) {
        if (data.size() > 1460) {
            // Keep one bounded transmit window, emitted as MTU-sized packets.
            // A guest can ACK every other segment without stop-and-wait stalls.
            uint32_t first = remember ? f.sendNext : f.unacknowledged;
            if (remember) {
                f.pending = data; f.pendingFlags = flags;
                f.sendNext += data.size();
            }
            for (size_t offset = 0; offset < data.size(); offset += 1460) {
                f.unacknowledged = first + offset;
                auto end = std::min(data.size(), offset + 1460);
                tcp(f, flags, Bytes(data.begin() + offset, data.begin() + end));
            }
            f.unacknowledged = first;
            return;
        }
        bool syn = flags & 2;
        Bytes p((syn ? 24 : 20) + data.size());
        put16(p.data(), f.destinationPort);
        put16(p.data() + 2, f.sourcePort);
        put32(p.data() + 4, remember
                                ? f.sendNext
                                : ((flags & 3) || !data.empty() ? f.unacknowledged : f.sendNext));
        put32(p.data() + 8, f.receiveNext);
        p[12] = syn ? 0x60 : 0x50;
        p[13] = flags;
        put16(p.data() + 14, uint16_t(32768 - std::min<size_t>(32768, f.toHost.size())));
        if (syn) {
            p[20] = 2;
            p[21] = 4;
            put16(p.data() + 22, 1460);
        }
        std::copy(data.begin(), data.end(), p.begin() + (syn ? 24 : 20));
        put16(p.data() + 16, transportChecksum(f.destination, f.source, 6, p));
        if (remember) {
            f.unacknowledged = f.sendNext;
            f.sendNext += data.size() + (syn ? 1 : 0) + ((flags & 1) ? 1 : 0);
            f.pending = data;
            f.pendingFlags = flags;
        }
        f.sentAt = nowMS();
        ip(f.destination, f.source, 6, p);
    }
    void dhcp(const uint8_t *p, size_t n) {
        if (n < 240 || p[0] != 1 || p[1] != 1 || p[2] != 6 || u32(p + 236) != 0x63825363)
            return;
        uint8_t type = 0;
        for (size_t i = 240; i < n;) {
            unsigned code = p[i++];
            if (code == 255)
                break;
            if (code == 0)
                continue;
            if (i >= n)
                return;
            size_t len = p[i++];
            if (i + len > n)
                return;
            if (code == 53 && len == 1)
                type = p[i];
            i += len;
        }
        if (type != 1 && type != 3 && type != 8)
            return;
        Bytes r(240, 0);
        std::copy_n(p, 44, r.data());
        r[0] = 2;
        put32(r.data() + 16, type == 8 ? 0 : guestIP);
        put32(r.data() + 20, gatewayIP);
        put32(r.data() + 236, 0x63825363);
        auto option = [&](uint8_t code, uint32_t value) {
            r.push_back(code);
            r.push_back(4);
            size_t i = r.size();
            r.resize(i + 4);
            put32(r.data() + i, value);
        };
        r.insert(r.end(), {53, 1, uint8_t(type == 1 ? 2 : 5)});
        option(54, gatewayIP);
        option(1, 0xffffff00);
        option(3, gatewayIP);
        option(6, dnsIP);
        option(51, 86400);
        option(58, 43200);
        option(59, 75600);
        r.push_back(255);
        r.resize(std::max<size_t>(300, r.size()), 0);
        udp(gatewayIP, 0xffffffff, 67, 68, r, true);
    }
    Flow *find(uint32_t src, uint32_t dst, uint16_t sp, uint16_t dp, bool isTCP) {
        for (auto &f : flows)
            if (f->last && f->source == src && f->destination == dst && f->sourcePort == sp &&
                f->destinationPort == dp && f->tcp == isTCP)
                return f.get();
        return nullptr;
    }
    Flow *open(uint32_t src, uint32_t dst, uint16_t sp, uint16_t dp, bool isTCP) {
        if (flows.size() >= 128)
            return nullptr;
        auto f = std::make_unique<Flow>();
        f->source = src;
        f->destination = dst;
        f->sourcePort = sp;
        f->destinationPort = dp;
        f->tcp = isTCP;
        f->last = nowMS();
        f->local = isTCP && dst == gatewayIP && dp == 80;
        if (!f->local) {
            f->fd = socketFor(isTCP ? SOCK_STREAM : SOCK_DGRAM);
            if (f->fd < 0)
                return nullptr;
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(dp);
            address.sin_addr.s_addr =
                htonl(dst == dnsIP && dp == 53 ? dns : (dst == gatewayIP ? 0x7f000001 : dst));
            int result = connect(f->fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
            if (result < 0 && errno != EINPROGRESS)
                return nullptr;
            f->connecting = isTCP && result < 0;
        }
        flows.push_back(std::move(f));
        return flows.back().get();
    }
    void packet(const Bytes &p) {
        trace(p);
        if (p.size() < 14)
            return;
        std::copy_n(p.data() + 6, 6, guestMAC.begin());
        auto *h = p.data() + 14;
        size_t n = p.size() - 14;
        if (u16(p.data() + 12) == 0x806) {
            if (n < 28 || u16(h) != 1 || u16(h + 2) != 0x800 || h[4] != 6 || h[5] != 4 ||
                u16(h + 6) != 1)
                return;
            uint32_t target = u32(h + 24);
            if (target != gatewayIP && target != dnsIP)
                return;
            auto r = ethernet(0x806, 28);
            auto *a = r.data() + 14;
            std::copy_n(h, 6, a);
            put16(a + 6, 2);
            std::copy_n(gatewayMAC, 6, a + 8);
            put32(a + 14, target);
            std::copy_n(h + 8, 10, a + 18);
            deliver(std::move(r));
            return;
        }
        if (u16(p.data() + 12) != 0x800 || n < 20 || h[0] >> 4 != 4)
            return;
        size_t ihl = (h[0] & 15) * 4, total = u16(h + 2);
        if (ihl < 20 || total < ihl || total > n || checksum(h, ihl) || (u16(h + 6) & 0x3fff))
            return;
        uint32_t src = u32(h + 12), dst = u32(h + 16);
        uint8_t proto = h[9];
        auto *q = h + ihl;
        size_t len = total - ihl;
        if (proto == 1) {
            if (len >= 8 && q[0] == 8 && !checksum(q, len) && dst == gatewayIP) {
                Bytes reply(q, q + len);
                reply[0] = 0;
                reply[2] = reply[3] = 0;
                put16(reply.data() + 2, checksum(reply.data(), reply.size()));
                ip(dst, src, 1, reply);
            }
            return;
        }
        if (len < 8 || (proto != 6 && proto != 17))
            return;
        uint16_t sp = u16(q), dp = u16(q + 2);
        if (proto == 17) {
            size_t ul = u16(q + 4);
            if (ul < 8 || ul > len)
                return;
            if (u16(q + 6) && transportChecksum(src, dst, 17, Bytes(q, q + ul)))
                return;
            if (sp == 68 && dp == 67) {
                dhcp(q + 8, ul - 8);
                return;
            }
            if (src != guestIP || dst == 0xffffffff || (dst & 0xf0000000) == 0xe0000000)
                return;
            auto *f = find(src, dst, sp, dp, false);
            if (!f)
                f = open(src, dst, sp, dp, false);
            if (f) {
                f->last = nowMS();
                send(f->fd, q + 8, ul - 8, 0);
            }
            return;
        }
        if (src != guestIP || len < 20 || transportChecksum(src, dst, 6, Bytes(q, q + len)))
            return;
        unsigned thl = (q[12] >> 4) * 4;
        if (thl < 20 || thl > len)
            return;
        uint8_t flags = q[13];
        uint32_t seq = u32(q + 4), ack = u32(q + 8);
        auto *f = find(src, dst, sp, dp, true);
        if (!f && (flags & 0x12) == 2) {
            f = open(src, dst, sp, dp, true);
            if (f) {
                f->receiveNext = seq + 1;
                f->sendNext = serial += 65537;
                f->unacknowledged = f->sendNext;
            }
        }
        if (!f) {
            if (!(flags & 4)) {
                Flow closed;
                closed.source = src;
                closed.destination = dst;
                closed.sourcePort = sp;
                closed.destinationPort = dp;
                closed.receiveNext =
                    seq + (len - thl) + ((flags & 2) ? 1 : 0) + ((flags & 1) ? 1 : 0);
                closed.sendNext = ack;
                tcp(closed, (flags & 16) ? 4 : 0x14);
            }
            return;
        }
        f->last = nowMS();
        f->window = u16(q + 14);
        if (flags & 4) {
            f->last = 0;
            return;
        }
        if (flags & 2) {
            if (f->incoming && (flags & 16) && ack == f->sendNext && f->pendingFlags == 2) {
                f->receiveNext = seq + 1;
                f->pendingFlags = 0; f->pending.clear();
                f->unacknowledged = ack; f->established = true;
                tcp(*f, 0x10);
                return;
            }
            if (!f->connecting && !f->pendingFlags && !f->established)
                tcp(*f, 0x12, {}, true);
            else if (!f->connecting && f->pendingFlags == 0x12)
                tcp(*f, 0x12);
            return;
        }
        if (!(flags & 16) || int32_t(ack - f->sendNext) > 0)
            return;
        if (f->pendingFlags == 0x18 && int32_t(ack - f->unacknowledged) > 0 &&
            int32_t(f->sendNext - ack) > 0 && ack - f->unacknowledged <= f->pending.size()) {
            auto count = ack - f->unacknowledged;
            f->pending.erase(f->pending.begin(), f->pending.begin() + count);
            f->unacknowledged = ack;
            f->sentAt = nowMS();
        }
        if (ack == f->sendNext && f->pendingFlags) {
            f->pending.clear();
            f->pendingFlags = 0;
            f->unacknowledged = ack;
            f->established = true;
        }
        if (seq != f->receiveNext) {
            tcp(*f, 0x10);
            return;
        }
        size_t payload = len - thl;
        if (payload && f->toHost.size() + payload <= 32768 && !f->peerFin) {
            f->toHost.insert(f->toHost.end(), q + thl, q + len);
            f->receiveNext += payload;
            tcp(*f, 0x10);
        } else if (payload) {
            tcp(*f, 0x10);
            return;
        }
        if ((flags & 1) && !f->peerFin) {
            f->peerFin = true;
            ++f->receiveNext;
            tcp(*f, 0x10);
        }
    }
    void service() {
        uint64_t now = nowMS();
        if (now < nextPoll)
            return;
        nextPoll = now + 2;
        if (listener >= 0 && flows.size() < 128 &&
            std::any_of(guestMAC.begin(), guestMAC.end(), [](uint8_t b) { return b != 0; })) {
            int fd = accept(listener, nullptr, nullptr);
            if (fd >= 0) {
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
                int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
                auto f = std::make_unique<Flow>();
                f->fd = fd; f->source = guestIP; f->destination = gatewayIP;
                f->sourcePort = forwardedPort;
                do { if (++incomingPort < 40000) incomingPort = 40000; }
                while (find(guestIP, gatewayIP, forwardedPort, incomingPort, true));
                f->destinationPort = incomingPort;
                f->tcp = f->incoming = true; f->last = now;
                f->sendNext = serial += 65537; f->unacknowledged = f->sendNext;
                tcp(*f, 2, {}, true);
                flows.push_back(std::move(f));
            }
        }
        for (auto &ptr : flows) {
            auto &f = *ptr;
            if (!f.last)
                continue;
            if (!f.tcp) {
                for (int i = 0; i < 8; ++i) {
                    uint8_t buffer[1472];
                    ssize_t n = recv(f.fd, buffer, sizeof(buffer), 0);
                    if (n < 0)
                        break;
                    udp(f.destination, f.source, f.destinationPort, f.sourcePort,
                        Bytes(buffer, buffer + n));
                }
                continue;
            }
            if (f.connecting) {
                pollfd p{f.fd, POLLOUT, 0};
                if (::poll(&p, 1, 0) > 0) {
                    int error = 0;
                    socklen_t size = sizeof(error);
                    if (getsockopt(f.fd, SOL_SOCKET, SO_ERROR, &error, &size) || error) {
                        tcp(f, 0x14);
                        f.last = 0;
                        continue;
                    }
                    f.connecting = false;
                    tcp(f, 0x12, {}, true);
                }
                continue;
            }
            if (f.pendingFlags && now - f.sentAt > 500)
                tcp(f, f.pendingFlags, f.pending);
            if (!f.established)
                continue;
            if (f.local) {
                if (!f.toHost.empty() && !f.pendingFlags && !f.hostFin) {
                    std::string request(f.toHost.begin(), f.toHost.end());
                    if (request.find("\r\n\r\n") != std::string::npos) {
                        std::string body =
                            "<html><head><title>PCEMU network test</title></head><body>"
                            "<h1>NE2000 works</h1><p>Windows 95 TCP/IP, Ethernet and HTTP are "
                            "working.</p>"
                            "<p><a href=\"http://example.com/\">External HTTP "
                            "test</a></p></body></html>";
                        std::string response =
                            "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                        tcp(f, 0x18, Bytes(response.begin(), response.end()), true);
                        f.toHost.clear();
                        f.hostFin = true;
                    }
                } else if (f.hostFin && !f.pendingFlags && !f.finSent) {
                    tcp(f, 0x11, {}, true);
                    f.finSent = true;
                }
                if (f.peerFin && f.finSent && !f.pendingFlags)
                    f.last = 0;
                continue;
            }
            if (!f.toHost.empty()) {
                ssize_t sent = send(f.fd, f.toHost.data(), f.toHost.size(), 0);
                if (sent > 0) {
                    f.toHost.erase(f.toHost.begin(), f.toHost.begin() + sent);
                    tcp(f, 0x10); // Reopen the receive window after socket backpressure.
                } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    tcp(f, 0x14);
                    f.last = 0;
                    continue;
                }
            }
            if (f.peerFin && f.toHost.empty())
                shutdown(f.fd, SHUT_WR);
            if (!f.pendingFlags && !f.hostFin && f.window) {
                uint8_t buffer[16384];
                ssize_t n = recv(f.fd, buffer, std::min<unsigned>(sizeof(buffer), f.window), 0);
                if (n > 0)
                    tcp(f, 0x18, Bytes(buffer, buffer + n), true);
                else if (!n) {
                    f.hostFin = f.finSent = true;
                    tcp(f, 0x11, {}, true);
                } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    tcp(f, 0x14);
                    f.last = 0;
                }
            }
            if (f.peerFin && f.hostFin && !f.pendingFlags)
                f.last = 0;
        }
        std::erase_if(flows, [now](const auto &f) {
            return !f->last || now - f->last > (f->tcp ? 120000u : 30000u);
        });
    }
};
Network::Network(const std::string &capture) : state(std::make_unique<State>(capture)) {}
Network::~Network() = default;
uint16_t Network::forwardTCP(uint16_t hostPort, uint16_t guestPort) {
    return state->forwardTCP(hostPort, guestPort);
}
void Network::pump(NE2000 &nic, uint64_t epoch, uint64_t resets) {
    pump(nic.outgoing, [&](const uint8_t *data, size_t length) { nic.receive(data, length); }, epoch, resets);
}
void Network::pump(std::deque<std::vector<uint8_t>> &outgoing, Receiver receive,
                   uint64_t epoch, uint64_t resets) {
    auto &s = *state;
    s.receive = std::move(receive);
    if (s.epoch != epoch || s.resets != resets) {
        s.flows.clear(); s.epoch = epoch; s.resets = resets;
    }
    while (!outgoing.empty()) {
        Bytes p = std::move(outgoing.front()); outgoing.pop_front(); s.packet(p);
    }
    s.service();
}
} // namespace pc
