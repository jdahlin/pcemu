#include "guest_agent_socket.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
namespace pc {
namespace {
void nonblocking(int fd) {
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
        throw std::runtime_error("Cannot configure guest-agent socket");
#ifdef SO_NOSIGPIPE
    int value = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &value, sizeof(value)) < 0)
        throw std::runtime_error("Cannot configure guest-agent socket SIGPIPE handling");
#endif
}
} // namespace
GuestAgentSocket::GuestAgentSocket(const std::string &name) : path(name) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(address.sun_path))
        throw std::runtime_error("Guest-agent socket path too long");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0)
        throw std::runtime_error("Cannot create guest-agent socket");
    try {
        nonblocking(listener);
        // Never unlink an existing socket: it may belong to another running VM.
        auto previousMask = umask(0077);
        int bound = bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address));
        int bindError = errno;
        umask(previousMask);
        if (bound < 0)
            throw std::runtime_error("Cannot bind guest-agent socket: " +
                                     std::string(std::strerror(bindError)));
        struct stat info{};
        if (lstat(path.c_str(), &info) < 0)
            throw std::runtime_error("Cannot stat guest-agent socket");
        device = info.st_dev;
        inode = info.st_ino;
        if (chmod(path.c_str(), 0600) < 0 || listen(listener, 1) < 0)
            throw std::runtime_error("Cannot listen on guest-agent socket");
    } catch (...) {
        close(listener);
        listener = -1;
        struct stat info{};
        if (inode && !lstat(path.c_str(), &info) && info.st_dev == device && info.st_ino == inode)
            unlink(path.c_str());
        throw;
    }
}
GuestAgentSocket::~GuestAgentSocket() {
    if (client >= 0)
        close(client);
    if (listener >= 0)
        close(listener);
    struct stat info{};
    if (inode && !lstat(path.c_str(), &info) && info.st_dev == device && info.st_ino == inode)
        unlink(path.c_str());
}
void GuestAgentSocket::disconnect(VirtioSerial &serial) {
    if (client >= 0)
        close(client);
    client = -1;
    pending.clear();
    sent = 0;
    serial.set_host_port_open(false);
}
void GuestAgentSocket::pump(VirtioSerial &serial) {
    if (resetCount != serial.resetCount) {
        resetCount = serial.resetCount;
        pending.clear();
        sent = 0;
    }
    if (client < 0) {
        client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                throw std::runtime_error("Guest-agent socket accept failed");
            return;
        }
        try {
            nonblocking(client);
        } catch (...) {
            close(client);
            client = -1;
            throw;
        }
        serial.set_host_port_open(true);
    }
    serial.poll();
    std::array<uint8_t, 65536> buffer;
    auto capacity = std::min(buffer.size(), serial.host_receive_capacity());
    if (capacity) {
        auto size = recv(client, buffer.data(), capacity, 0);
        if (size > 0)
            serial.receive_from_host(std::span(buffer).first(size));
        else if (!size || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            disconnect(serial);
            return;
        }
    } else {
        // Detect a closed client even when the guest has not opened its port.
        auto size = recv(client, buffer.data(), 1, MSG_PEEK);
        if (!size || (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            disconnect(serial);
            return;
        }
    }
    if (sent == pending.size()) {
        pending = serial.take_guest_output(buffer.size());
        sent = 0;
    }
    if (sent < pending.size()) {
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        auto size = send(client, pending.data() + sent, pending.size() - sent, flags);
        if (size > 0)
            sent += size;
        else if (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            disconnect(serial);
    }
}
} // namespace pc
