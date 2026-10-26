#pragma once
#include "arm_fw_cfg.hpp"
#include <cstdint>
#include <span>
#include <string>
namespace pc {
// fw_cfg RAM framebuffer adapter. Validates the guest-provided descriptor and
// exports pixels without depending on the CPU backend or a GUI frontend.
// Wire reference: QEMU RAMFBCfg layout and ramfb_fw_cfg_write.
// https://github.com/qemu/qemu/blob/master/hw/display/ramfb.c#L23
class ArmRamFb {
  public:
    // Bind the fw_cfg item and guest RAM without changing guest state.
    // IMPLEMENTATION NOTE: Host wiring around RAMFBCfg.
    // https://github.com/qemu/qemu/blob/master/hw/display/ramfb.c#L23
    ArmRamFb(ArmFwCfg &config, std::span<const uint8_t> memory, uint64_t base)
        : fwCfg(config), ram(memory), ramBase(base) {}
    // Describe the validated mode; return invalid/inactive when no display is available.
    std::string mode_description() const;
    // Copy visible pixels to opaque host ARGB; return false without a valid descriptor.
    bool read_pixels(std::vector<uint32_t> &output, unsigned &width, unsigned &height) const;
    // Retain output between calls. False leaves its pixels untouched; dimensions
    // still describe the current mode (zero when invalid/inactive).
    // Returns true only when visible bytes or the descriptor changed.
    bool changed_pixels(std::vector<uint32_t> &output, unsigned &width, unsigned &height) const;
    // Export visible RGB pixels as PPM; throw when the descriptor is invalid/inactive.
    void save_ppm(const std::string &path) const;

  private:
    friend class ArmVirtMachine;
    friend struct ArmSnapshotAccess; // Host checkpoint fields; never serialize host bindings.
    ArmFwCfg &fwCfg;
    std::span<const uint8_t> ram;
    uint64_t ramBase;
    mutable std::vector<uint8_t> previousDescriptor, previousPixels;
};
} // namespace pc
