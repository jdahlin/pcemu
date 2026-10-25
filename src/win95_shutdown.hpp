#pragma once
#include <array>
#include <cstdint>
#include <span>
namespace pc {
// Original Windows 95 WIN.COM finishes shutdown by broadcasting INT 2F/160E,
// BL=3, then spinning in NOP/JMP -3. Match the code, not a fixed guest address
// or a quiet disk: an idle system or a save prompt must never count as shutdown.
inline bool win95ShutdownComplete(std::span<const uint8_t> ram, uint32_t cr0,
                                 uint32_t codeBase, uint32_t ip, uint32_t ax, uint32_t bx) {
    if ((cr0 & 1) || (ax & 0xffff) != 0x160e || (bx & 0xff) != 3) return false;
    constexpr std::array<uint8_t, 10> terminal{0xb3, 3, 0xb8, 0x0e, 0x16, 0xcd, 0x2f, 0x90, 0xeb, 0xfd};
    for (unsigned back : {7u, 8u}) {
        const uint64_t start = uint64_t(codeBase) + ((ip - back) & 0xffff);
        if (start + terminal.size() > ram.size()) continue;
        bool matches = true;
        for (unsigned i = 0; i < terminal.size(); ++i)
            matches &= ram[start + i] == terminal[i];
        if (matches) return true;
    }
    return false;
}
// Pace physical keyboard input so Explorer can open Run before the command is
// typed. No force flag is used: Windows can ask applications to save or cancel.
class Win95ShutdownRequest {
    unsigned step = 0;
    uint64_t nextTime = 0, nextInstructions = 0;
  public:
    bool requested = false;
    void request(uint64_t now, uint64_t instructions) {
        if (requested) return;
        requested = true;
        nextTime = now;
        nextInstructions = instructions;
    }
    template<class Scan, class Key>
    void tick(uint64_t now, uint64_t instructions, bool protectedMode, bool keyboardEmpty,
              Scan scan, Key key) {
        constexpr char command[] = "rundll32.exe user.exe,ExitWindows";
        constexpr unsigned end = 5 + sizeof(command);
        if (!requested || step >= end || !protectedMode || !keyboardEmpty ||
            now < nextTime || instructions < nextInstructions) return;
        if (step < 4) scan(std::array<uint8_t, 4>{0x1d, 1, 0x81, 0x9d}[step]);
        else if (step == 4) key('r');
        else if (step < end - 1) key(command[step - 5]);
        else key(0x1c0d);
        ++step;
        nextTime = now + (step <= 5 ? 500 : 80);
        nextInstructions = instructions + 100000;
    }
};
} // namespace pc
