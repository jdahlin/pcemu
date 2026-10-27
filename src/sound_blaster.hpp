#pragma once
#include "i8237.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <vector>
namespace pc {
// Creative Sound Blaster Series Hardware Programming Guide, first edition,
// chapters 2, 3, 4, 6. DSP 3.02 / SB Pro 8-bit PCM playback subset.
// https://www.ardent-tool.com/sound/Sound_Blaster_HW_Programming_Guide_1st.pdf#page=24
struct SoundBlasterDevice {
    static constexpr uint16_t BasePort=0x220;
    static constexpr unsigned DmaChannel=1, Irq=7;
    enum class Register : unsigned {
        MixerAddress=4, MixerData=5, Reset=6, ReadData=10,
        WriteCommandStatus=12, ReadStatusAcknowledge=14
    };
    // Commands and lengths: chapter 6; transfer counts are encoded N-1.
    // https://www.ardent-tool.com/sound/Sound_Blaster_HW_Programming_Guide_1st.pdf#page=86
    enum class Command : uint8_t {
        DirectDac=0x10, SingleCycleOutput=0x14, AutoInitializeOutput=0x1c,
        TimeConstant=0x40, BlockSize=0x48, Silence=0x80,
        HighSpeedAutoOutput=0x90, HighSpeedSingleOutput=0x91,
        Pause=0xd0, SpeakerOn=0xd1, SpeakerOff=0xd3, Continue=0xd4,
        SpeakerStatus=0xd8, ExitAutoInitialize=0xda,
        Identify=0xe0, Version=0xe1, WriteTest=0xe4, ReadTest=0xe8, TriggerIrq=0xf2
    };
    static constexpr uint64_t PitFrequencyHz=1193182, MicrosecondsPerSecond=1000000,
        HostFramesPerSecond=48000;
    static constexpr size_t MaximumOutputSamples=96000;
    struct State {
        std::array<uint8_t,256> mixer{};
        std::array<uint8_t,16> response{};
        uint8_t mixer_index=0, response_head=0, response_count=0;
        uint8_t command=0, argument_count=0, arguments_needed=0;
        std::array<uint8_t,2> arguments{};
        uint8_t time_constant=156, test_register=0;
        uint32_t block_size=65536, remaining=0;
        uint64_t last_ticks=0, phase=0, pending_samples=0, reset_ready_at=0, host_phase=0;
        int16_t left=0,right=0;
        bool active=false, paused=false, auto_initialize=false, exit_auto=false,
             high_speed=false, silence=false, speaker=false, irq_pending=false,
             reset_high=false, reset_pending=false, right_next=false;
    } state;
    // Diagnostic counters survive DSP reset; output is bounded host presentation
    // state. Neither a host sink nor sound-device availability changes guest DMA.
    struct Counters {
        uint64_t dma_bytes=0, blocks=0, pcm_frames=0, non_silent_frames=0,
            dropped_frames=0, pcm_hash=14695981039346656037ull;
    } counters;
    std::vector<int16_t> output;
    // Board reset installs mixer defaults and clears DSP/diagnostic state.
    SoundBlasterDevice();
    // Process elapsed guest sample clocks, independent of host audio availability.
    void advance_sample_clock(uint64_t pitTicks,I8237Device &dma,std::span<const uint8_t> ram);
    // Read a relative mixer/DSP port; status E clears the DSP interrupt latch.
    uint8_t read_register(unsigned offset);
    // Program relative mixer/reset/DSP ports at the supplied guest clock boundary.
    void write_register(unsigned offset,uint8_t value,uint64_t pitTicks);
  private:
    // Restore CT1345 mono/volume defaults without resetting the DSP.
    void reset_mixer();
    // Append a bounded DSP response byte for status/data reads.
    void queue_response(uint8_t value);
    // Apply a fully assembled command at its guest-clock boundary.
    void execute_command(uint64_t pitTicks);
    // Start an independent DSP transfer block and sample-clock phase.
    void start_output(uint64_t pitTicks,bool automatic,bool high_speed,bool silence,uint32_t count);
    // Convert held DAC levels to bounded host-format PCM frames.
    void emit_sample();
};
} // namespace pc
