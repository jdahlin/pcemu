#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
namespace pc {
struct Machine;
// Toolkit-independent PC presentation. It owns font/rendering policy, not the
// VGA hardware or the frontend window. Captures intentionally omit the cursor.
class PcDisplay {
  public:
    // Physical native VGA text/graphics extent; fallback text stays 640x400.
    std::pair<unsigned,unsigned> dimensions(const Machine &machine) const;
    void loadFont(std::span<const uint8_t> bytes);
    void refresh(Machine &machine, bool executeVga);
    void save(Machine &machine, const std::string &framePath, bool verbose) const;
    void render(Machine &machine, std::span<uint32_t> pixels, unsigned displayWidth,
                unsigned displayHeight, uint64_t frameTicks) const;
  private:
    // Present current text memory/font/attributes in the frontend's logical frame.
    uint32_t text_pixel(const Machine &machine, unsigned x, unsigned y,
                        unsigned width, unsigned height, bool animate, uint64_t ticks) const;
    std::array<uint8_t, 2048> font{};
};
} // namespace pc
