#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
namespace pc {
struct Machine;
// Toolkit-independent PC presentation. It owns font/rendering policy, not the
// VGA hardware or the frontend window. Captures intentionally omit the cursor.
class PcDisplay {
  public:
    void loadFont(std::span<const uint8_t> bytes);
    void refresh(Machine &machine, bool executeVga);
    void save(Machine &machine, const std::string &framePath, bool verbose) const;
    void render(Machine &machine, std::span<uint32_t> pixels, unsigned displayWidth,
                unsigned displayHeight, uint64_t frameTicks) const;
  private:
    std::array<uint8_t, 2048> font{};
};
} // namespace pc
