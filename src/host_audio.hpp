#pragma once
#include <cstdint>
#include <memory>
#include <span>
namespace pc {
// Host service for the HDA model's fixed stereo signed-16-bit 48 kHz PCM format.
class HostAudio {
  public:
    virtual ~HostAudio() = default;
    // Queue borrowed interleaved samples; implementation bounds queued latency.
    virtual void queue_samples(std::span<const int16_t> samples) = 0;
};
// Open SDL audio without creating a video window; throw if output cannot be opened.
std::unique_ptr<HostAudio> open_host_audio();
} // namespace pc
