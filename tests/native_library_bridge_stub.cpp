// A headless session stand-in: snapshots publish atomically like both real engines.
#include "NativeBridge.h"
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>
namespace {
std::mutex mutex;
std::condition_variable ready;
std::queue<std::string> commands;
std::string error;
bool failSnapshot = false;
int run() {
    for (;;) {
        std::unique_lock lock(mutex);
        ready.wait(lock, [] { return !commands.empty(); });
        auto command = commands.front(); commands.pop(); lock.unlock();
        auto split = command.find(' ');
        auto op = command.substr(0, split);
        if (op == "fail-next-snapshot") { failSnapshot = true; continue; }
        if (op == "suspend" || op == "save-snapshot") {
            if (failSnapshot) {
                std::lock_guard lock(mutex); error = "Simulated snapshot failure"; failSnapshot = false; continue;
            }
            auto target = command.substr(split + 1);
            auto temporary = target + ".saving";
            std::filesystem::create_directories(temporary);
            std::ofstream(temporary + "/state.bin") << "test checkpoint";
            std::filesystem::rename(temporary, target);
            if (op == "fail-next-snapshot") { failSnapshot = true; continue; }
        if (op == "suspend") return 0;
        }
    }
}
}
extern "C" {
void pcemu_native_prepare() {}
int pcemu_native_run(const char*, const char*, const char*, const char*, unsigned, const char*, int) { return run(); }
int pcemu_native_run_arm(const char*, const char*, const char*, unsigned, unsigned, const char*) { return run(); }
void pcemu_native_command(const char* command) {
    { std::lock_guard lock(mutex); commands.emplace(command); }
    ready.notify_one();
}
int pcemu_native_error(char* message, int capacity) {
    std::lock_guard lock(mutex);
    if (error.empty()) return 0;
    error.copy(message, capacity - 1); message[std::min(error.size(), size_t(capacity - 1))] = 0;
    error.clear(); return 1;
}
void pcemu_native_event(int, uint16_t, int, int, int, int) {}
int pcemu_native_frame(uint32_t*, int, int*, int*, int*) { return 0; }
int pcemu_native_cursor(uint32_t*, int*, int*, int*, int*, int*) { return 0; }
}
