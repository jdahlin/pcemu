#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <vector>
namespace pc {
// Modern virtio-net, one RX/TX split-queue pair. All DMA runs with the vCPU stopped.
class VirtioNet {
  public:
    using Bytes = std::vector<uint8_t>;
    VirtioNet(std::span<uint8_t> ram, uint64_t ramBase, std::function<void(bool)> interrupt);
    uint64_t read(unsigned offset, unsigned width = 4) const;
    void write(unsigned offset, uint64_t value, unsigned width = 4);
    void receive(const uint8_t *data, size_t length);
    std::deque<Bytes> outgoing;
    uint64_t transmitted = 0, received = 0, dropped = 0, resetCount = 0;
    const std::string &error() const {
        return failure;
    }

  private:
    struct Queue {
        uint32_t size = 256, ready = 0;
        uint64_t descriptors = 0, available = 0, used = 0;
        uint16_t last = 0, index = 0;
    };
    struct Buffer {
        uint64_t address;
        uint32_t length;
    };
    std::span<uint8_t> ram;
    uint64_t ramBase;
    std::function<void(bool)> interrupt;
    std::array<Queue, 2> queues{};
    uint32_t featureSelect = 0, driverSelect = 0, queueSelect = 0, status = 0, isr = 0;
    uint64_t driverFeatures = 0;
    std::string failure;
    static constexpr uint64_t features =
        (1ull << 32) | (1ull << 29) | (1ull << 16) | (1ull << 5) | (1ull << 3) | 1;
    uint8_t *memory(uint64_t address, uint64_t length) const;
    uint64_t get(uint64_t address, unsigned width) const;
    void put(uint64_t address, uint64_t value, unsigned width);
    void validate(const Queue &queue) const;
    std::vector<Buffer> chain(const Queue &queue, uint16_t head, bool writable) const;
    Bytes gather(const std::vector<Buffer> &buffers) const;
    void scatter(const std::vector<Buffer> &buffers, const Bytes &packet);
    uint16_t pending(const Queue &queue) const;
    void complete(Queue &queue, uint16_t head, uint32_t length);
    void signal(Queue &queue, uint16_t previous);
    void transmit();
    void broken(const std::string &reason);
};
} // namespace pc
