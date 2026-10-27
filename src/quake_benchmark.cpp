#include "quake_benchmark.hpp"
#include "cpu.hpp"
#include <cmath>
#include <sstream>
#include <string>
namespace pc {
// Original Quake prints frames, elapsed real time, then frames/time, with one
// decimal place. Require every label and a complete line before reporting it.
// https://github.com/id-Software/Quake/blob/master/WinQuake/cl_demo.c#L311-L324
std::optional<QuakeTimedemoResult> parseQuakeTimedemo(std::string_view transcript) {
    size_t start = 0;
    while (start < transcript.size()) {
        auto end = transcript.find('\n', start);
        if (end == std::string_view::npos) break;
        std::istringstream input{std::string(transcript.substr(start, end - start))};
        unsigned frames = 0;
        double seconds = 0, fps = 0;
        std::string a, b, c, extra;
        if (input >> frames >> a >> seconds >> b >> fps >> c && !(input >> extra) &&
            frames > 0 && frames < 10000000 && a == "frames" && b == "seconds" && c == "fps" &&
            std::isfinite(seconds) && seconds > 0 && std::isfinite(fps) && fps > 0)
            return QuakeTimedemoResult{frames, seconds, fps};
        start = end + 1;
    }
    return {};
}
// Pin both execution mode and instruction bytes at the two guest boundaries.
// Caller invokes this before CPU::step, so no instruction scope crosses start/
// stop. Validate the frame counter instead of guessing from console messages.
QuakeProfileWindow::Event QuakeProfileWindow::observe(CPU &cpu) {
    if (ended || !(cpu.cr[0] & 1) || cpu.cpl() != 3 || !cpu.s[CS].big) return Event::None;
    if (!begun && cpu.ip == 0x3ac9 && cpu.s[CS].base == cpu.s[DS].base) {
        base = cpu.s[CS].base;
        const uint8_t expected[] = {0xd9,0x1d,0x94,0xc9,0x06,0};
        for (unsigned n = 0; n < sizeof(expected); ++n)
            if (cpu.read(base + 0x3ac3 + n, 1) != expected[n])
                throw std::runtime_error("Quake timedemo start fingerprint mismatch");
        beginFrame = cpu.read(base + 0x95bb4, 4);
        if (beginFrame != cpu.read(base + 0x6c990, 4) + 1)
            throw std::runtime_error("Quake timing did not start after its warmup frame");
        begun = true;
        return Event::Begin;
    }
    if (begun && cpu.ip == 0x3f86 && cpu.s[CS].base == base) {
        const uint8_t expected[] = {0x83,0xec,0x04,0xc7,0x05,0x80,0xc9,0x06,0,0,0,0,0};
        for (unsigned n = 0; n < sizeof(expected); ++n)
            if (cpu.read(base + 0x3f86 + n, 1) != expected[n])
                throw std::runtime_error("Quake timedemo finish fingerprint mismatch");
        endFrame = cpu.read(base + 0x95bb4, 4);
        if (endFrame <= beginFrame) throw std::runtime_error("Empty Quake profile interval");
        ended = true;
        return Event::End;
    }
    return Event::None;
}

}
