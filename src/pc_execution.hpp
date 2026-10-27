#pragma once
#include "jit.hpp"
#include "machine.hpp"
namespace pc {
// The normal execution scheduling contract, shared by the session and headless
// workload runners. Poll after each native batch, with the session's additional
// 50,000-step timer checkpoint. Guest instruction accounting can overshoot the
// soft budget by one block; Jit::runBatch owns its precise stopping rules.
inline void pollPcDevices(Machine &machine,uint64_t &nextTimer) {
    machine.poll();
    if(machine.cpu.instructions>=nextTimer) {
        machine.poll();
        nextTimer=machine.cpu.instructions+50000;
    }
}
inline Jit::BatchResult runPcBatch(Machine &machine,Jit &jit,uint64_t &nextTimer,unsigned budget=64) {
    auto result=jit.runBatch(budget);
    pollPcDevices(machine,nextTimer);
    return result;
}
} // namespace pc
