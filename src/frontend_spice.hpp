#pragma once
#include "frontend.hpp"
#include <string>
namespace pc {
class VirtioSerial;
// A real SPICE server backed by the emulator's software display and USB input.
// All public methods and agent callbacks run on the VM thread with VPs stopped.
class SpiceFrontend final : public Frontend {
  public:
    explicit SpiceFrontend(unsigned port, const std::string &passwordFile = {});
    ~SpiceFrontend() override;
    std::vector<FrontendEvent> poll() override;
    void present(std::span<const uint32_t>, unsigned, unsigned,
                 DisplayShape = DisplayShape::SquarePixels) override;
    void setPointerShape(const PointerShape &) override;
    bool capturePointer(bool) override {
        return true;
    }
    void pumpAgent(VirtioSerial *serial);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace pc
