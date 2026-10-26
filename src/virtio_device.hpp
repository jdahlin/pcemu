#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pc {
// Modern split-ring core for the guest-tool devices. All calls run with VPs stopped.
class VirtioDevice {
  public:
    using Interrupt = std::function<void(bool, unsigned)>;
    VirtioDevice(std::span<uint8_t> ram, uint64_t base, uint32_t id, unsigned queues,
                 uint64_t features, Interrupt interrupt);
    virtual ~VirtioDevice() = default;
    uint64_t read(unsigned offset, unsigned width = 4) const;
    void write(unsigned offset, uint64_t value, unsigned width = 4);
    const std::string &error() const {
        return failure;
    }
    bool running() const {
        return (status & 15) == 15 && !(status & 192);
    }
    unsigned interruptQueue() const {
        return lastInterruptQueue;
    }
    uint64_t completions = 0, resetCount = 0;

  protected:
    struct Segment {
        std::span<uint8_t> bytes;
        bool writable;
    };
    struct Chain {
        std::vector<Segment> segments;
        size_t readable = 0, writable = 0;
        std::vector<uint8_t> input(size_t maximum) const;
        void output(std::span<const uint8_t> data) const;
    };
    virtual std::optional<uint32_t> handle(unsigned queue, const Chain &chain) = 0;
    virtual uint64_t configRead(unsigned, unsigned) const {
        return 0;
    }
    virtual void configWrite(unsigned, uint64_t, unsigned) {}
    virtual void deviceReset() {}
    virtual void notified(unsigned queue) {
        process(queue);
    }
    void process(unsigned queue);
    void configurationChanged();
    bool negotiated(unsigned bit) const {
        return (driverFeatures & (1ull << bit)) != 0;
    }
    std::span<uint8_t> memory(uint64_t address, uint64_t length) const;
    void broken(const std::string &reason);

  private:
    struct Queue {
        uint32_t size = 256;
        bool ready = false;
        uint64_t descriptors = 0, available = 0, used = 0;
        uint16_t nextAvailable = 0, nextUsed = 0;
    };
    std::span<uint8_t> ram;
    uint64_t ramBase, features;
    uint32_t id, featureSelect = 0, driverSelect = 0, queueSelect = 0;
    uint32_t status = 0, interruptStatus = 0, generation = 0;
    uint64_t driverFeatures = 0;
    unsigned lastInterruptQueue = 0;
    std::vector<Queue> queues;
    Interrupt interrupt;
    std::string failure;
    uint64_t get(uint64_t address, unsigned width) const;
    void put(uint64_t address, uint64_t value, unsigned width);
    void validate(const Queue &queue) const;
    Chain chain(const Queue &queue, uint16_t head) const;
    void reset();
};
} // namespace pc
