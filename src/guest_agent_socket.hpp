#pragma once
#include "virtio_serial.hpp"
#include <string>
#include <sys/types.h>
#include <vector>
namespace pc {
// Private local stream socket; transports guest-agent bytes without interpreting commands.
class GuestAgentSocket {
  public:
    explicit GuestAgentSocket(const std::string &path);
    ~GuestAgentSocket();
    GuestAgentSocket(const GuestAgentSocket &) = delete;
    GuestAgentSocket &operator=(const GuestAgentSocket &) = delete;
    void pump(VirtioSerial &serial);

  private:
    int listener = -1, client = -1;
    std::string path;
    dev_t device = 0;
    ino_t inode = 0;
    std::vector<uint8_t> pending;
    size_t sent = 0;
    uint64_t resetCount = 0;
    void disconnect(VirtioSerial &serial);
};
} // namespace pc
