#pragma once
#include "arm_fw_cfg.hpp"
#include <cstdint>
#include <span>
#include <string>
namespace pc {
// fw_cfg RAM framebuffer adapter. Validates the guest-provided descriptor and
// exports pixels without depending on the CPU backend or a GUI frontend.
class ArmRamFb {
  public:
    ArmRamFb(ArmFwCfg &config, std::span<const uint8_t> memory, uint64_t base)
        : fwCfg(config), ram(memory), ramBase(base) {}
    std::string mode() const;
    bool pixels(std::vector<uint32_t> &output, unsigned &width, unsigned &height) const;
    // Retain output between calls. False leaves its pixels untouched; dimensions
    // still describe the current mode (zero when invalid/inactive).
    // Returns true only when visible bytes or the descriptor changed.
    bool changedPixels(std::vector<uint32_t> &output, unsigned &width, unsigned &height) const;
    void save(const std::string &path) const;
  private:
    ArmFwCfg &fwCfg;
    std::span<const uint8_t> ram;
    uint64_t ramBase;
    mutable std::vector<uint8_t> previousDescriptor, previousPixels;
};
} // namespace pc
