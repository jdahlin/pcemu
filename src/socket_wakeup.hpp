#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <utility>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

namespace pc {
// One-shot socket readiness. The worker only requests an owner-thread wakeup;
// it never touches guest memory or mutable network/CPU state.
class SocketWakeup {
  public:
    struct Watch {
        int fd;
        short events;
        // Keep the original descriptor alive until a poll snapshot retires.
        std::shared_ptr<void> lifetime;
        bool operator==(const Watch &other) const {
            return fd == other.fd && events == other.events && lifetime == other.lifetime;
        }
    };

    explicit SocketWakeup(std::function<void()> wake) : notify(std::move(wake)) {
        if (!notify) throw std::invalid_argument("Network wakeup callback is required");
        if (pipe(control)) throw std::runtime_error("Cannot create network wakeup pipe");
        for (int fd : control) {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
        try { worker = std::thread([this] { run(); }); }
        catch (...) { close(control[0]); close(control[1]); throw; }
    }
    ~SocketWakeup() {
        { std::lock_guard lock(mutex); stopping = true; signal(); }
        worker.join();
        close(control[0]); close(control[1]);
    }
    SocketWakeup(const SocketWakeup &) = delete;
    SocketWakeup &operator=(const SocketWakeup &) = delete;

    void arm(std::vector<Watch> next) {
        std::lock_guard lock(mutex);
        if (armed && next == watches) return;
        watches = std::move(next);
        armed = true;
        ++generation;
        signal();
    }
    bool consume() { return notified.exchange(false); }

  private:
    int control[2]{};
    std::mutex mutex;
    std::thread worker;
    std::function<void()> notify;
    std::vector<Watch> watches;
    bool armed = false, stopping = false;
    uint64_t generation = 0;
    std::atomic<bool> notified{false};

    void signal() {
        const char byte = 0;
        // A full nonblocking pipe already contains a pending notification.
        while (write(control[1], &byte, 1) < 0 && errno == EINTR) {}
    }
    void run() {
        for (;;) {
            std::vector<Watch> snapshot;
            uint64_t version;
            {
                std::lock_guard lock(mutex);
                if (stopping) return;
                if (armed) snapshot = watches;
                version = generation;
            }
            std::vector<pollfd> descriptors{{control[0], POLLIN, 0}};
            for (const auto &watch : snapshot)
                descriptors.push_back({watch.fd, watch.events, 0});
            int result = poll(descriptors.data(), descriptors.size(), -1);
            if (result < 0 && errno == EINTR) continue;
            if (descriptors[0].revents) {
                char buffer[128];
                while (read(control[0], buffer, sizeof(buffer)) > 0) {}
                continue; // Rebuild before using potentially stale readiness.
            }
            if (result <= 0) continue;
            {
                std::lock_guard lock(mutex);
                if (stopping) return;
                if (!armed || version != generation) continue;
                armed = false; // No level-triggered spin while the owner is busy.
                notified.store(true);
            }
            notify();
        }
    }
};
} // namespace pc
