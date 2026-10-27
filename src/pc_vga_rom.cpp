#include "pc_vga_rom.hpp"
namespace pc {
// Generated from firmware/vga; the option ROM executes on the guest CPU.
std::span<const uint8_t> builtinVgaRom() {
    static constexpr uint8_t bytes[] = {
#include PCEMU_BUILTIN_VGA_ROM
    };
    static_assert(sizeof(bytes) == 32768);
    return bytes;
}
}
