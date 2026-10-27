#pragma once
#include "frontend.hpp"
#include <string>
#include <functional>

namespace pc {
// One session per process at present (signal capture/JIT diagnostics are global).
// The frontend is created and called on the session thread. A toolkit may bridge
// this boundary to its main thread. There are no frontend calls in CPU dispatch.
using FrontendFactory = std::function<std::unique_ptr<Frontend>(unsigned, unsigned, DisplayShape)>;
// Polled between execution batches, never from CPU/JIT dispatch. A UI queues
// commands here rather than paying control-file IO or sharing machine state.
class SessionControl {
public:
    virtual ~SessionControl() = default;
    virtual std::vector<std::string> takeCommands() = 0;
    virtual void reportError(std::string_view) {}
};
struct PcSessionOptions {
    std::string machine = "pc-legacy";
    std::string disk, snapshot;
    std::string rom; // Empty selects the embedded standalone option ROM.
    std::string controlFile;
    std::string bootText;
    unsigned memoryMiB = 16;
    bool network = true;
    bool audio = true;
    bool initializeVga = true;
    bool headless = false;
    bool startPaused = false;
    uint64_t instructionLimit = 0;
};
int runPcSession(const PcSessionOptions &, FrontendFactory, SessionControl * = nullptr);
// CLI compatibility entry point; frontends normally use the typed options above.
int runPcSessionCommandLine(int argc, char **argv, FrontendFactory, SessionControl * = nullptr);
}
