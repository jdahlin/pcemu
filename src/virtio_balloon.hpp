#pragma once
#include "virtio_device.hpp"
#include <map>
namespace pc {
class VirtioBalloon : public VirtioDevice {
  public:
    using Discard = std::function<bool(uint64_t, size_t)>;
    VirtioBalloon(std::span<uint8_t> ram, uint64_t base, Interrupt irq, size_t hostPageSize = 4096,
                  Discard discard = {}, bool legacyPfn64 = false);
    void setTargetPages(uint32_t pages);
    void requestStats();
    uint32_t targetPages() const {
        return target;
    }
    uint32_t actualPages() const {
        return actual;
    }
    uint32_t inflatedPages() const {
        return inflatedCount;
    }
    const std::map<uint16_t, uint64_t> &statistics() const {
        return stats;
    }
    uint64_t inflateRequests = 0, deflateRequests = 0, inflatePages = 0, deflatePages = 0;
    uint64_t discardBytes = 0, discardFailures = 0, statsReports = 0;

  private:
    uint64_t ramBase;
    uint32_t maximumPages, target = 0, actual = 0, inflatedCount = 0;
    size_t hostPageSize, pagesPerHostPage;
    std::vector<bool> inflated, discarded;
    Discard discard;
    bool wantStats = false, legacyPfn64;
    std::map<uint16_t, uint64_t> stats;
    std::optional<uint32_t> handle(unsigned queue, const Chain &chain) override;
    uint64_t configRead(unsigned offset, unsigned width) const override;
    void configWrite(unsigned offset, uint64_t value, unsigned width) override;
    void deviceReset() override;
};
} // namespace pc
