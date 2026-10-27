#include "pc_audio.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include <unistd.h>
using namespace pc;
int main() {
    auto path=std::filesystem::temp_directory_path()/("pcemu-audio-"+std::to_string(getpid())+".wav");
    struct Cleanup {std::filesystem::path p;~Cleanup(){std::filesystem::remove(p);}} cleanup{path};
    try {
        auto check=[](bool value){if(!value)throw std::runtime_error("PC audio contract failed");};
        PcAudioPacer pacer;
        check(pacer.delay_ns(0,0)==0);
        check(pacer.delay_ns(596591,480000000)==10000000); // Half a guest second, 20 ms ahead.
        check(pacer.delay_ns(1193182,500000000)==20000000); // Bounded wait.
        check(pacer.delay_ns(1193182,995000000)==0); // Within permitted lead.
        check(pacer.delay_ns(1193182,1100000000)==0); // Slow host never gets a negative wait.
        check(pacer.delay_ns(1,1200000000)==0); // Rewind rebases.
        pacer.reset();check(pacer.delay_ns(9000000,1300000000)==0);
        {
            PcWaveCapture wave(path.string());
            std::array<int16_t,4> samples{-32768,32512,0,-256};
            wave.append(samples);
            bool rejected=false;
            try {wave.append(std::span<const int16_t>(samples.data(),1));}
            catch(const std::invalid_argument &) {rejected=true;}
            check(rejected);wave.finish();wave.finish();
        }
        std::ifstream input(path,std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
        auto number=[&](unsigned offset,unsigned n){uint32_t v=0;for(unsigned i=0;i<n;++i)v|=uint32_t(bytes.at(offset+i))<<(8*i);return v;};
        check(bytes.size()==52 && std::string(bytes.begin(),bytes.begin()+4)=="RIFF");
        check(number(4,4)==44 && std::string(bytes.begin()+8,bytes.begin()+16)=="WAVEfmt ");
        check(number(16,4)==16 && number(20,2)==1 && number(22,2)==2);
        check(number(24,4)==48000 && number(28,4)==192000 && number(32,2)==4 && number(34,2)==16);
        check(std::string(bytes.begin()+36,bytes.begin()+40)=="data" && number(40,4)==8);
        const std::array<uint8_t,8> expected{0,128,0,127,0,0,0,255};
        check(std::equal(expected.begin(),expected.end(),bytes.begin()+44));
        {PcWaveCapture empty(path.string());} // Destructor finalizes empty/exception paths.
        check(std::filesystem::file_size(path)==44);
        std::cout<<"PC audio pacing and WAV byte-format checks passed\n";
    } catch(const std::exception &error) {std::cerr<<error.what()<<'\n';return 1;}
}
