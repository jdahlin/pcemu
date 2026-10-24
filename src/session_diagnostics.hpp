#pragma once
#include <cstdint>
#include <iosfwd>
namespace pc {
struct Machine;
struct CPU;
struct MachineProfile;
class Jit;
struct CaptureContext {
    const MachineProfile &profile;
    uint64_t executionWallNs;
    bool enableJit, followJumps, traceJit, enableHostInput;
};
void dumpCpu(CPU &cpu);
void writeMachineState(std::ostream &output, Machine &machine, const Jit &jit,
                       const CaptureContext &context);
} // namespace pc
