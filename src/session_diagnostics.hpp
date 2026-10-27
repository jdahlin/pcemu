#pragma once
#include <cstdint>
#include <iosfwd>
namespace pc {
struct Machine;
struct VGA;
struct CPU;
struct MachineProfile;
class Jit;
struct CaptureContext {
    const MachineProfile &profile;
    uint64_t executionWallNs;
    bool enableJit, followJumps, traceJit, enableHostInput;
};
// Emit read-only VGA registers, DAC and plane bytes for external test oracles.
void writeVgaState(std::ostream &output, const VGA &vga);
void dumpCpu(CPU &cpu);
void writeMachineState(std::ostream &output, Machine &machine, const Jit &jit,
                       const CaptureContext &context);
} // namespace pc
