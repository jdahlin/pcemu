#pragma once
#include "ne2000.hpp"
#include <chrono>
#include <fstream>
#include <memory>
namespace pc {
// Original unprivileged IPv4 socket gateway. No host network extensions or
// external emulator/network-stack library. Host sockets never enter snapshots.
class Network {
    struct State;
    std::unique_ptr<State> state;

  public:
    explicit Network(const std::string &capture = "");
    ~Network();
    void pump(NE2000 &nic, uint64_t epoch, uint64_t resets);
};
} // namespace pc
