#pragma once
#include <optional>
#include <string_view>
namespace pc {
struct CPU;
// Boundary map for SHA-256 7cdb244122cc607cc6f162524bfcac60f6d11d8ca2ff07473d6cd962311161fb
// (original DOS Quake 1.06). The wrapper checks the executable hash offline.
// CL_GetMessage stores td_starttime on frame td_startframe+1; finish entry
// precedes CL_FinishTimeDemo's timing/report work. No executable is patched.
// https://github.com/id-Software/Quake/blob/master/WinQuake/cl_demo.c
struct QuakeProfileWindow {
    enum class Event { None, Begin, End };
    bool begun = false, ended = false;
    unsigned base = 0, beginFrame = 0, endFrame = 0;
    Event observe(CPU &cpu);
};
struct QuakeTimedemoResult {
    unsigned frames;
    double seconds, fps;
};
// Parse a complete original Quake CL_FinishTimeDemo console line. Reject partial,
// nonfinite, zero/negative and unrelated lines; no gameplay success is inferred.
std::optional<QuakeTimedemoResult> parseQuakeTimedemo(std::string_view transcript);
}
