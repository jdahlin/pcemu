// The Sound Blaster's digital signal processor (DSP) accepts commands through
// byte I/O ports. It requests sample bytes from the primary DMA controller at a
// programmable sample clock, converts unsigned 8-bit PCM to an analog level,
// and interrupts the CPU at the end of each programmed block. Auto-initialize
// playback repeats DSP blocks while the DMA controller independently repeats
// its RAM buffer. Their lengths need not match (double-buffered players use a
// DSP block half the DMA buffer). Reading DSP status acknowledges its interrupt;
// the CPU must separately issue End Of Interrupt to the PIC.
//
// This board uses base 220h, byte DMA 1, IRQ7 and DSP version 3.02 (SB Pro).
// Relative offsets select mixer index/data, DSP reset, read data, write/status,
// and read-status/interrupt acknowledgment. Mixer stereo alternates left/right
// bytes; the time constant defines a BYTE period in microseconds, not a stereo
// frame period. Speaker and PCM/master volume controls affect presentation only,
// never DMA counters or block interrupts. The machine supplies PIT-frequency
// guest time, physical RAM, DMA acknowledgments and PIC wiring.
//
// IMPLEMENTATION NOTE: DSP initialization takes 100 microseconds; commands are
// otherwise accepted immediately. Events process in bounded 65,536-sample batches
// with remaining due samples retained for the next poll. Host PCM is zero-order
// held stereo16/48 kHz and bounded to one second; host backpressure never stops
// guest transfers. Mixer reset is independent of DSP reset. Presentation queues
// are transient. No host device is opened by this model.
// FIXME: ADC, ADPCM, MIDI, FM synthesis, SB16 commands, analog filtering and exact
// DSP/FIFO/bus latency are unsupported. Direct DAC commands latch output only;
// continuous timed direct-DAC presentation is not implemented. Reserved commands
// are ignored; this subset is not complete DSP 3.02 conformance.
// Creative Sound Blaster Series Hardware Programming Guide, first edition,
// chapters 2 (DSP ports/reset), 3 (PCM DMA), 4 (mixer), 6 (commands).
// https://www.ardent-tool.com/sound/Sound_Blaster_HW_Programming_Guide_1st.pdf#page=24
#include "sound_blaster.hpp"
#include <algorithm>
namespace pc {
// Install CT1345 mixer defaults independently of a DSP reset; volume level 4
// and mono output follow the register defaults in chapter 4.
// https://www.ardent-tool.com/sound/Sound_Blaster_HW_Programming_Guide_1st.pdf#page=65
SoundBlasterDevice::SoundBlasterDevice() { reset_mixer(); }
// Reset stored mixer registers to mono/level-4 PCM/master output. Analog filters
// are stored controls only. Creative guide chapter 4, SB Pro mixer registers.
// https://www.ardent-tool.com/sound/Sound_Blaster_HW_Programming_Guide_1st.pdf#page=65
void SoundBlasterDevice::reset_mixer() {
    state.mixer.fill(0);state.mixer[0x04]=state.mixer[0x22]=state.mixer[0x26]=0x88;
}
// Append bounded DSP response data. An unread full response queue retains its
// oldest data; capacity/overflow policy is host protection, not a FIFO claim.
// Creative guide chapter 2, Read Data and Read-Buffer Status.
// https://www.ardent-tool.com/sound/Sound_Blaster_HW_Programming_Guide_1st.pdf#page=24
void SoundBlasterDevice::queue_response(uint8_t value) {
    if(state.response_count<state.response.size()) {
        state.response[(state.response_head+state.response_count)%state.response.size()]=value;
        ++state.response_count;
    }
}
// Read DSP/mixer state. The interrupt acknowledgment does not clear PIC service
// state or remove unread response bytes. Creative guide chapters 2 and 3.
// https://www.ardent-tool.com/sound/Sound_Blaster_HW_Programming_Guide_1st.pdf#page=24
uint8_t SoundBlasterDevice::read_register(unsigned offset) {
    switch(Register(offset)) {
    case Register::MixerAddress:return state.mixer_index;
    case Register::MixerData:return state.mixer[state.mixer_index];
    case Register::ReadData: {
        if(!state.response_count)return 0xff;
        uint8_t result=state.response[state.response_head];
        state.response_head=(state.response_head+1)%state.response.size();--state.response_count;
        return result;
    }
    case Register::WriteCommandStatus:
        return (state.reset_high||state.reset_pending||state.high_speed)?0x80:0;
    case Register::ReadStatusAcknowledge:
        state.irq_pending=false;return state.response_count?0x80:0;
    default:return 0xff;
    }
}
// Start a DSP block independent of the 8237 count. Reset the sample phase so the
// first DMA byte occurs after a full time-constant period, never at command issue.
// Creative guide chapter 3, Single-cycle/Auto-initialize DMA programming.
// https://www.ardent-tool.com/sound/Sound_Blaster_HW_Programming_Guide_1st.pdf#page=40
void SoundBlasterDevice::start_output(uint64_t ticks,bool automatic,bool high_speed,bool silence,uint32_t count) {
    state.active=true;state.paused=false;state.auto_initialize=automatic;state.exit_auto=false;
    state.high_speed=high_speed;state.silence=silence;state.remaining=count;
    state.last_ticks=ticks;state.phase=state.pending_samples=0;
}
// Execute a complete command, with little-endian N-1 lengths. Unsupported
// commands do not consume invented argument bytes. High-speed stops via reset.
// Creative guide chapter 6, DSP commands 10h..F2h.
// https://www.ardent-tool.com/sound/Sound_Blaster_HW_Programming_Guide_1st.pdf#page=86
void SoundBlasterDevice::execute_command(uint64_t ticks) {
    uint32_t count=unsigned(state.arguments[0])+(unsigned(state.arguments[1])<<8)+1;
    switch(Command(state.command)) {
    case Command::DirectDac:state.left=state.right=(int(state.arguments[0])-128)*256;break;
    case Command::TimeConstant:state.time_constant=state.arguments[0];state.phase=0;break;
    case Command::BlockSize:state.block_size=count;break;
    case Command::SingleCycleOutput:start_output(ticks,false,false,false,count);break;
    case Command::AutoInitializeOutput:start_output(ticks,true,false,false,state.block_size);break;
    case Command::HighSpeedAutoOutput:start_output(ticks,true,true,false,state.block_size);break;
    case Command::HighSpeedSingleOutput:start_output(ticks,false,true,false,state.block_size);break;
    case Command::Silence:start_output(ticks,false,false,true,count);break;
    case Command::Pause:state.paused=true;break;
    case Command::Continue:state.paused=false;state.last_ticks=ticks;break;
    case Command::SpeakerOn:state.speaker=true;break;
    case Command::SpeakerOff:state.speaker=false;break;
    case Command::SpeakerStatus:queue_response(state.speaker?0xff:0);break;
    case Command::ExitAutoInitialize:state.exit_auto=true;break;
    case Command::Identify:queue_response(~state.arguments[0]);break;
    case Command::Version:queue_response(3);queue_response(2);break;
    case Command::WriteTest:state.test_register=state.arguments[0];break;
    case Command::ReadTest:queue_response(state.test_register);break;
    case Command::TriggerIrq:state.irq_pending=true;break;
    }
}
// Decode mixer, reset edge, and DSP command/data writes. DSP reset retains the
// mixer and cumulative diagnostics but aborts DMA/IRQ/parser/response state.
// Creative guide chapter 2 reset procedure, and chapters 4/6 programming.
// https://www.ardent-tool.com/sound/Sound_Blaster_HW_Programming_Guide_1st.pdf#page=24
void SoundBlasterDevice::write_register(unsigned offset,uint8_t value,uint64_t ticks) {
    switch(Register(offset)) {
    case Register::MixerAddress:state.mixer_index=value;return;
    case Register::MixerData:
        if(state.mixer_index==0)reset_mixer();
        else {
            if(state.mixer_index==0x0e && !(state.mixer[0x0e]&2) && (value&2))state.right_next=false;
            state.mixer[state.mixer_index]=value;
        }
        return;
    case Register::Reset:
        if(value&1) {state.reset_high=true;state.active=false;state.irq_pending=false;}
        else if(state.reset_high) {
            auto mixer=state.mixer;auto mixer_index=state.mixer_index;
            state={};state.mixer=mixer;state.mixer_index=mixer_index;
            state.last_ticks=ticks;state.reset_pending=true;
            state.reset_ready_at=ticks+(PitFrequencyHz*100+MicrosecondsPerSecond-1)/MicrosecondsPerSecond;
            output.clear();
        }
        return;
    case Register::WriteCommandStatus:break;
    default:return;
    }
    if(state.reset_high||state.reset_pending||state.high_speed)return;
    if(state.arguments_needed) {
        state.arguments[state.argument_count++]=value;
        if(state.argument_count==state.arguments_needed) {state.arguments_needed=0;execute_command(ticks);}
        return;
    }
    state.command=value;state.argument_count=0;state.arguments={};
    switch(Command(value)) {
    case Command::DirectDac:case Command::TimeConstant:case Command::Identify:case Command::WriteTest:
        state.arguments_needed=1;return;
    case Command::SingleCycleOutput:case Command::BlockSize:case Command::Silence:
        state.arguments_needed=2;return;
    default:execute_command(ticks);return;
    }
}
// Resample a held unsigned-PCM DAC level to fixed host stereo16/48 kHz. SB Pro
// stereo consumes alternating bytes; the clock still measures bytes/second.
// Volume/speaker affect PCM only. Creative guide chapters 3 and 4.
// https://www.ardent-tool.com/sound/Sound_Blaster_HW_Programming_Guide_1st.pdf#page=50
void SoundBlasterDevice::emit_sample() {
    // CT1345 specifies -46 dB at zero, -11 dB at default level four, and
    // approximately 4 dB steps to 0 dB at seven. This Q15 curve approximates
    // intermediate analog attenuation; filters/crosstalk are not modeled.
    // Creative guide CT1345 voice/master volume, printed 4-9.
    // https://www.ardent-tool.com/sound/Sound_Blaster_HW_Programming_Guide_1st.pdf#page=67
    static constexpr int GainQ15[8]={164,2320,3677,5827,9235,14637,23198,32768};
    state.host_phase+=(256-unsigned(state.time_constant))*HostFramesPerSecond;
    while(state.host_phase>=MicrosecondsPerSecond) {
        state.host_phase-=MicrosecondsPerSecond;
        int16_t samples[2]{};
        for(unsigned side=0;side<2;++side) {
            unsigned shift=side?1:5;
            unsigned master=(state.mixer[0x22]>>shift)&7,pcm=(state.mixer[4]>>shift)&7;
            samples[side]=state.speaker&&!state.silence?(side?state.right:state.left):0;
            samples[side]=(int64_t(samples[side])*GainQ15[master]*GainQ15[pcm])/(int64_t(1)<<30);
        }
        ++counters.pcm_frames;
        if(samples[0]||samples[1])++counters.non_silent_frames;
        if(output.size()>=MaximumOutputSamples) {counters.dropped_frames+=output.size()/2;output.clear();}
        for(int16_t sample:samples) {
            for(unsigned shift:{0u,8u})counters.pcm_hash=(counters.pcm_hash^uint8_t(uint16_t(sample)>>shift))*1099511628211ull;
            output.push_back(sample);
        }
    }
}
// Advance fractional sample clocks on the shared 1,193,182-Hz timebase. Process
// DMA before block completion; the 8237 buffer's terminal count is independent.
// Creative guide chapter 3, DMA transfer and interrupt acknowledgment sequence.
// https://www.ardent-tool.com/sound/Sound_Blaster_HW_Programming_Guide_1st.pdf#page=40
void SoundBlasterDevice::advance_sample_clock(uint64_t ticks,I8237Device &dma,std::span<const uint8_t> ram) {
    if(state.reset_pending&&ticks>=state.reset_ready_at) {state.reset_pending=false;queue_response(0xaa);}
    if(ticks<state.last_ticks) {state.last_ticks=ticks;state.phase=state.pending_samples=0;return;}
    uint64_t elapsed=ticks-state.last_ticks;state.last_ticks=ticks;
    if(!state.active||state.paused)return;
    const uint64_t denominator=PitFrequencyHz*(256-unsigned(state.time_constant));
    // Split seconds to keep multiplication bounded for ordinary long time jumps.
    const uint64_t period=256-unsigned(state.time_constant),seconds=elapsed/PitFrequencyHz;
    state.pending_samples+=seconds*(MicrosecondsPerSecond/period);
    const uint64_t extraMicroseconds=seconds*(MicrosecondsPerSecond%period);
    state.pending_samples+=extraMicroseconds/period;
    state.phase+=(extraMicroseconds%period)*PitFrequencyHz+(elapsed%PitFrequencyHz)*MicrosecondsPerSecond;
    state.pending_samples+=state.phase/denominator;state.phase%=denominator;
    unsigned budget=65536;
    while(state.pending_samples&&budget--&&state.active) {
        --state.pending_samples;
        bool transferred=state.silence;
        if(!state.silence) {
            dma.set_request(DmaChannel,true);
            if(auto transfer=dma.acknowledge_memory_read(DmaChannel)) {
                uint8_t value=transfer->address<ram.size()?ram[transfer->address]:0xff;
                int16_t sample=(int(value)-128)*256;
                if(state.mixer[0x0e]&2) {
                    if(state.right_next)state.right=sample;else state.left=sample;
                    state.right_next=!state.right_next;
                } else state.left=state.right=sample;
                ++counters.dma_bytes;transferred=true;
            }
            dma.set_request(DmaChannel,false);
        }
        emit_sample();
        if(transferred&&--state.remaining==0) {
            state.irq_pending=true;++counters.blocks;
            if(state.auto_initialize&&!state.exit_auto)state.remaining=state.block_size;
            else {state.active=false;state.high_speed=false;state.pending_samples=0;}
        }
    }
}
} // namespace pc
