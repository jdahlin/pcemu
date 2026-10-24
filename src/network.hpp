#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>
#include <chrono>
#include <fstream>
#include <memory>
namespace pc {
struct NE2000;
// Original unprivileged IPv4 socket gateway. No host network extensions or
// external emulator/network-stack library. Host sockets never enter snapshots.
class Network {
    struct State;
    std::unique_ptr<State> state;

  public:
    explicit Network(const std::string &capture = "");
    ~Network();
    void pump(NE2000 &nic, uint64_t epoch, uint64_t resets);
    using Receiver = std::function<void(const uint8_t *, size_t)>;
    void pump(std::deque<std::vector<uint8_t>> &outgoing, Receiver receive,
              uint64_t epoch = 0, uint64_t resets = 0);
    // Bind only loopback. Port zero requests an ephemeral host port.
    uint16_t forwardTCP(uint16_t hostPort, uint16_t guestPort = 22);
};
} // namespace pc
