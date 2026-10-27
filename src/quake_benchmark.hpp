#pragma once
#include <optional>
#include <string_view>
namespace pc {
struct QuakeTimedemoResult {
    unsigned frames;
    double seconds, fps;
};
// Parse a complete original Quake CL_FinishTimeDemo console line. Reject partial,
// nonfinite, zero/negative and unrelated lines; no gameplay success is inferred.
std::optional<QuakeTimedemoResult> parseQuakeTimedemo(std::string_view transcript);
}
