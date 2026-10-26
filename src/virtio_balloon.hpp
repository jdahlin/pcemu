#pragma once
#include "virtio_device.hpp"
#include <map>
namespace pc {
// Traditional Virtio balloon: guest PFN accounting, host reclamation and statistics.
class VirtioBalloon : public VirtioDevice {
  public:
    using Discard = std::function<bool(uint64_t, size_t)>;
    // Bind RAM and optional reclaim callback; granule must be aligned and a power of two.
    VirtioBalloon(std::span<uint8_t> ram, uint64_t base, Interrupt irq, size_t hostPageSize = 4096,
                  Discard discard = {}, bool legacyPfn64 = false);
    // Request a guest balloon target in 4096-byte pages.
    void set_requested_pages(uint32_t pages);
    // Return the retained statistics buffer so the driver supplies a fresh report.
    void request_statistics();
    // Read the host target, which survives a device reset.
    // IMPLEMENTATION NOTE: Host inspection of Virtio 1.2 balloon state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3240006
    uint32_t requested_pages() const {
        return target;
    }
    // Read the guest-reported balloon size, distinct from tracked PFNs.
    // IMPLEMENTATION NOTE: Host inspection of Virtio 1.2 balloon state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3240006
    uint32_t actual_pages() const {
        return actual;
    }
    // Read the number of distinct PFNs currently accounted as inflated.
    // IMPLEMENTATION NOTE: Host inspection of Virtio 1.2 balloon state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3240006
    uint32_t inflated_pages() const {
        return inflatedCount;
    }
    // Inspect the last guest statistics by tag; values/units depend on each tag.
    // IMPLEMENTATION NOTE: Host inspection of Virtio 1.2 balloon state.
    // https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html#x1-3240006
    const std::map<uint16_t, uint64_t> &statistics() const {
        return stats;
    }
    uint64_t inflateRequests = 0, deflateRequests = 0, inflatePages = 0, deflatePages = 0;
    uint64_t discardBytes = 0, discardFailures = 0, statsReports = 0;

  private:
    friend struct ArmSnapshotAccess; // Host checkpoint fields; never serialize host bindings.
    uint64_t ramBase;
    uint32_t maximumPages, target = 0, actual = 0, inflatedCount = 0;
    size_t hostPageSize, pagesPerHostPage;
    std::vector<bool> inflated, discarded;
    Discard discard;
    bool wantStats = false, legacyPfn64;
    std::map<uint16_t, uint64_t> stats;
    std::optional<uint32_t> handle(unsigned queue, const Chain &chain) override;
    uint64_t read_device_configuration(unsigned offset, unsigned width) const override;
    void write_device_configuration(unsigned offset, uint64_t value, unsigned width) override;
    void reset_device() override;
};
} // namespace pc
