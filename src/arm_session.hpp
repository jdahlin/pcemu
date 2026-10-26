#pragma once
#include "arm_virt.hpp"
#include "pc_session.hpp"
namespace pc {
struct ArmSessionOptions {
    ArmVirtMachine::Config machine;
    std::string snapshot, runDirectory;
    bool paused = false;
    // Zero runs until a lifecycle command/guest power-off, useful for GUI use.
    uint64_t maximumSlices = 0;
};
// Blocking owner-thread runner with the same frontend/control contracts as x86.
int runArmSession(ArmSessionOptions, FrontendFactory, SessionControl * = nullptr);
}
