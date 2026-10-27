#include "quake_benchmark.hpp"
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
}
