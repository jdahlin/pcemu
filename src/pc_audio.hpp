#pragma once
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
namespace pc {
// Host policy: throttle accepted live PCM to the deterministic guest clock.
// This never changes PIT/VGA/DSP time; lagging hosts simply receive less audio.
class PcAudioPacer {
    bool anchored=false;
    uint64_t guestOrigin=0,hostOrigin=0,lastGuest=0;
  public:
    void reset() {anchored=false;}
    // Return a bounded host wait, allowing 10 ms of lead and at most 20 ms per
    // iteration so controls remain responsive. Rewinds start a new timeline.
    uint64_t delay_ns(uint64_t guestTicks,uint64_t hostNanoseconds) {
        if(!anchored || guestTicks<lastGuest || hostNanoseconds<hostOrigin) {
            anchored=true;guestOrigin=lastGuest=guestTicks;hostOrigin=hostNanoseconds;return 0;
        }
        lastGuest=guestTicks;
        uint64_t ticks=guestTicks-guestOrigin;
        uint64_t guestNs=(ticks/1193182)*1000000000+(ticks%1193182)*1000000000/1193182;
        uint64_t hostNs=hostNanoseconds-hostOrigin;
        if(guestNs<=hostNs || guestNs-hostNs<=10000000)return 0;
        return std::min<uint64_t>(guestNs-hostNs-10000000,20000000);
    }
};
// Host-only deterministic stereo16/48 kHz WAV capture. Explicit byte encoding
// avoids native endianness/struct padding; hardware behavior stays in the DSP.
// Microsoft RIFF/WAVE format and finalization:
// https://learn.microsoft.com/en-us/windows/win32/medfound/tutorial--decoding-audio
class PcWaveCapture {
    std::ofstream stream;
    uint32_t dataBytes=0;
    bool finished=false;
    // Write one little-endian RIFF integer, independent of the host CPU.
    void integer(uint32_t value,unsigned bytes) {
        for(unsigned i=0;i<bytes;++i)stream.put(char(value>>(8*i)));
    }
  public:
    // Create a valid empty PCM WAVE file; format fields are fixed by HostAudio.
    explicit PcWaveCapture(const std::string &path):stream(path,std::ios::binary) {
        if(!stream)throw std::runtime_error("Cannot open audio capture: "+path);
        stream.write("RIFF",4);integer(36,4);stream.write("WAVEfmt ",8);integer(16,4);
        integer(1,2);integer(2,2);integer(48000,4);integer(192000,4);integer(4,2);integer(16,2);
        stream.write("data",4);integer(0,4);
        if(!stream)throw std::runtime_error("Cannot write WAV header");
    }
    // Finalize even when a guest exception unwinds the session. Explicit finish
    // reports I/O failure; destructors cannot propagate an exception.
    ~PcWaveCapture() {try {finish();}catch(...) {}}
    // Append complete interleaved stereo frames, refusing RIFF's 32-bit overflow.
    void append(std::span<const int16_t> samples) {
        if(finished || samples.size()%2)throw std::invalid_argument("WAV requires complete stereo frames");
        if(samples.size()>(UINT32_MAX-36-dataBytes)/2)throw std::length_error("WAV capture exceeds RIFF size limit");
        for(int16_t value:samples)integer(uint16_t(value),2);
        if(!stream)throw std::runtime_error("Audio capture write failed");
        dataBytes+=samples.size()*2;
    }
    // Patch RIFF/data lengths after the final PCM byte, then close the file.
    void finish() {
        if(finished)return;
        stream.seekp(4);integer(dataBytes+36,4);stream.seekp(40);integer(dataBytes,4);
        stream.flush();
        if(!stream)throw std::runtime_error("Audio capture finalization failed");
        stream.close();finished=true;
    }
};
} // namespace pc
