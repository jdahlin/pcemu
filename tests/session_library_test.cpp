#include "pc_session.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <stdexcept>

// No SDL, host GUI, installed guest media, or large disk fixture. Exercise the
// typed library entry and its in-memory controls independently of the CLI main.
int main() {
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
        std::cout << "Typed session, in-memory control, suspend and restore passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
