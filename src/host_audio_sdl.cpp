// Host-only SDL playback service. Hardware behavior belongs to intel_hda.cpp;
// this backend accepts its fixed stereo signed-16-bit 48 kHz PCM samples.
#include "host_audio.hpp"
#include <SDL.h>
#include <stdexcept>
#include <string>
namespace pc {
namespace {
class SdlHostAudio final : public HostAudio {
    SDL_AudioDeviceID device = 0;

  public:
    // Initialize only SDL's audio subsystem and request the model's exact format.
    SdlHostAudio() {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO))
            throw std::runtime_error(std::string("Audio: ") + SDL_GetError());
        SDL_AudioSpec desired{};
        desired.freq = 48000;
        desired.format = AUDIO_S16SYS;
        desired.channels = 2;
        desired.samples = 512;
        device = SDL_OpenAudioDevice(nullptr, 0, &desired, nullptr, 0);
        if (!device) {
            std::string error = SDL_GetError();
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            throw std::runtime_error("Audio: " + error);
        }
        SDL_PauseAudioDevice(device, 0);
    }
    // Close the output device before releasing this service's subsystem reference.
    ~SdlHostAudio() override {
        SDL_CloseAudioDevice(device);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    // Bound latency to 100 ms; a host stall drops old queued output instead of
    // allowing guest RAM usage or host audio delay to grow without limit.
    void queue_samples(std::span<const int16_t> samples) override {
        constexpr unsigned MaximumQueuedBytes = 48000 * 2 * sizeof(int16_t) / 10;
        if (samples.size_bytes() > MaximumQueuedBytes)
            samples = samples.last(MaximumQueuedBytes / sizeof(int16_t));
        if (SDL_GetQueuedAudioSize(device) + samples.size_bytes() > MaximumQueuedBytes)
            SDL_ClearQueuedAudio(device);
        if (SDL_QueueAudio(device, samples.data(), Uint32(samples.size_bytes())))
            throw std::runtime_error(std::string("Audio queue: ") + SDL_GetError());
    }
};
} // namespace
// Construct the host service independently of the display frontend.
std::unique_ptr<HostAudio> open_host_audio() {
    return std::make_unique<SdlHostAudio>();
}
} // namespace pc
