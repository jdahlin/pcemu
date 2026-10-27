#include "pc_session.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <stdexcept>
#include <pthread.h>

// No SDL, host GUI, installed guest media, or large disk fixture. Exercise the
// typed library entry and its in-memory controls independently of the CLI main.
static int checkSessions() {
    namespace fs = std::filesystem;
    auto work = fs::temp_directory_path() / ("pcemu-library-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(work);
    struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{work};
    try {
        std::string boot(512, '\0');
        boot[0] = '\x40'; boot[1] = '\xeb'; boot[2] = '\xfd';
        boot[510] = '\x55'; boot[511] = '\xaa';
        std::ofstream(work / "boot.raw", std::ios::binary).write(boot.data(), boot.size());
        pc::PcSessionOptions options;
        options.disk = (work / "boot.raw").string(); options.rom.clear();
        options.headless = true; options.network = false; options.memoryMiB = 2;
        options.startPaused = true;
        struct Controls final : pc::SessionControl {
            unsigned polls = 0;
            std::string checkpoint;
            std::vector<std::string> takeCommands() override {
                switch (++polls) {
                case 1: return {"resume"};
                case 2: return {"pause"};
                case 3: return {"reboot", "resume"};
                case 4: return {"suspend " + checkpoint};
                default: throw std::runtime_error("Session failed to suspend");
                }
            }
        } control;
        control.checkpoint = (work / "saved state").string();
        if (pc::runPcSession(options, nullptr, &control) != 0 || control.polls != 4 ||
            !fs::exists(fs::path(control.checkpoint) / "state.bin"))
            throw std::runtime_error("Typed session/control API failed");
        options.disk.clear(); options.snapshot = control.checkpoint;
        struct Quit final : pc::SessionControl {
            std::vector<std::string> takeCommands() override { return {"quit"}; }
        } quit;
        if (pc::runPcSession(options, nullptr, &quit) != 0)
            throw std::runtime_error("Sequential library snapshot restore failed");
        // A sink frontend exercises the embedded display path without opening
        // a host window or touching any installed guest image.
        unsigned frames=0;
        struct Sink final : pc::Frontend {
            unsigned &frames;
            unsigned *audioSamples=nullptr;
            explicit Sink(unsigned &count,unsigned *samples=nullptr) : frames(count),audioSamples(samples) {}
            bool queueAudio(std::span<const int16_t> samples) override {
                if(!audioSamples)return false;
                if(samples.empty())return *audioSamples!=0;
                if(samples.size()%2)throw std::runtime_error("Invalid session PCM");
                *audioSamples+=samples.size();return true;
            }
            std::vector<pc::FrontendEvent> poll() override { return {}; }
            void present(std::span<const uint32_t> pixels,unsigned w,unsigned h,pc::DisplayShape) override {
                if (pixels.size()!=size_t(w)*h) throw std::runtime_error("Invalid startup framebuffer");
                ++frames;
            }
            bool capturePointer(bool enabled) override { return enabled; }
        };
        auto factory=[&](unsigned,unsigned,pc::DisplayShape) { return std::make_unique<Sink>(frames); };
        for (auto profile : {"win311","win95"}) {
            options.machine=profile; options.snapshot.clear(); options.disk=(work/"boot.raw").string();
            options.headless=false; options.startPaused=false;
            options.memoryMiB=std::string(profile)=="win95" ? 64 : 16;
            if (pc::runPcSession(options,factory,&quit)!=0)
                throw std::runtime_error("Embedded cold boot failed on a small worker stack");
            options.snapshot=control.checkpoint; options.disk.clear();
            if (pc::runPcSession(options,factory,&quit)!=0)
                throw std::runtime_error("Embedded restore failed on a small worker stack");
        }
        if (!frames) throw std::runtime_error("Embedded startup did not present a frame");
        // A RAM-only DSP silence block exercises the actual session-to-frontend
        // PCM bridge without a video window, SDL audio device, or sound hardware.
        std::string audioBoot(512,'\0');
        const uint8_t audioCode[]={0xfa,0xba,0x2c,0x02,0xb0,0x40,0xee,0xb0,156,0xee,
            0xb0,0x80,0xee,0xb0,9,0xee,0x30,0xc0,0xee,0xb9,0x10,0x27,0xe2,0xfe,0xf4};
        std::copy(std::begin(audioCode),std::end(audioCode),audioBoot.begin());
        audioBoot[510]='\x55';audioBoot[511]='\xaa';
        std::ofstream(work/"audio.raw",std::ios::binary).write(audioBoot.data(),audioBoot.size());
        unsigned audioSamples=0;
        struct AudioControl final:pc::SessionControl {
            unsigned &samples;unsigned polls=0;
            explicit AudioControl(unsigned &s):samples(s){}
            std::vector<std::string> takeCommands() override {
                if(samples)return {"quit"};
                if(++polls>20)throw std::runtime_error("Session failed to deliver PCM");
                return {};
            }
        } audioControl(audioSamples);
        options={};options.disk=(work/"audio.raw").string();options.network=false;options.memoryMiB=2;
        auto audioFactory=[&](unsigned,unsigned,pc::DisplayShape){return std::make_unique<Sink>(frames,&audioSamples);};
        if(pc::runPcSession(options,audioFactory,&audioControl)!=0 || !audioSamples)
            throw std::runtime_error("Frontend PCM delivery failed");
        // Tracing allocates its large history ring on demand, also off-stack.
        std::vector<std::string> arguments{"pcemu","--headless","--no-network","--vgabios","",
            "--disk",(work/"boot.raw").string(),"--ram-mb","2","--steps","128",
            "--trace-tail",(work/"trace.txt").string()};
        std::vector<char *> argv;
        for (auto &argument : arguments) argv.push_back(argument.data());
        if (pc::runPcSessionCommandLine(int(argv.size()),argv.data(),{})!=0 ||
            !fs::file_size(work/"trace.txt")) throw std::runtime_error("Trace history failed on a small worker stack");
        std::cout << "512 KiB worker: typed controls, embedded Win311/Win95 cold boot/restore and tracing passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}

// Exercise the same small stack as SwiftUI's Dispatch worker. Main-thread tests
// masked the former multi-megabyte session stack allocation.
int main() {
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return 1;
    if (pthread_attr_setstacksize(&attributes,512*1024) != 0) {
        pthread_attr_destroy(&attributes); return 1;
    }
    int status=1;
    pthread_t worker;
    const int created=pthread_create(&worker,&attributes,[](void *context) -> void * {
        *static_cast<int *>(context)=checkSessions(); return nullptr;
    },&status);
    pthread_attr_destroy(&attributes);
    if (created != 0 || pthread_join(worker,nullptr) != 0) return 1;
    return status;
}
