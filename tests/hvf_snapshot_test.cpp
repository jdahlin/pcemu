#include "hvf_machine.hpp"
#include <array>
#include <iostream>
#include <thread>
#include <vector>
#include <stdexcept>
using namespace pc;
static void check(bool good,const char *message) { if (!good) throw std::runtime_error(message); }
int main() {
    try {
        std::vector<uint8_t> snapshot, ram;
        uint64_t x0=0, counter=0, secondaryPC=0;
        std::array<HvfMachine::PrivateInterruptState,2> interruptState;
        {
            HvfMachine vm(2*1024*1024,HvfMachine::GicProfile::HyperVNonsecure,{},2);
            const std::array<uint32_t,2> code{0x91000400,0x17ffffff};
            vm.writeMemory(HvfMachine::ramBase,{reinterpret_cast<const uint8_t *>(code.data()),sizeof(code)});
            vm.start(HvfMachine::ramBase); vm.setReg(0,11);
            check(vm.startCPU(1,HvfMachine::ramBase,17)==0,"CPU1 start");
            vm.run(std::chrono::milliseconds(10));
            vm.sendPrivateInterrupt(3,7);
            for (unsigned i=0; i<2; ++i) {
                interruptState[i]=vm.privateInterruptState(i);
                check(interruptState[i].available && (interruptState[i].pending&(1u<<7)),"SGI was not pending before capture");
            }
            vm.pauseClock(true); counter=vm.virtualCounterSample().value;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            check(vm.virtualCounterSample().value==counter,"Paused architectural clock advanced");
            x0=vm.reg(0); secondaryPC=vm.cpuPC(1);
            snapshot=vm.saveState(); ram.assign(vm.ram().begin(),vm.ram().end());
        }
        {
            HvfMachine vm(2*1024*1024,HvfMachine::GicProfile::HyperVNonsecure,{},2,{},true);
            vm.writeMemory(HvfMachine::ramBase,ram); vm.restoreState(snapshot);
            check(vm.reg(0)==x0,"Primary register restore");
            check(vm.cpuOnline(1) && vm.cpuPC(1)==secondaryPC,"Secondary state restore");
            check(vm.virtualCounterSample().value==counter,"Restored clock changed");
            for (unsigned i=0; i<2; ++i) {
                auto current=vm.privateInterruptState(i);
                check(current.available && current.pending==interruptState[i].pending && current.enabled==interruptState[i].enabled &&
                      current.active==interruptState[i].active,"Native GIC private interrupt state changed");
            }
            vm.pauseClock(false); vm.run(std::chrono::milliseconds(10));
            check(vm.reg(0)>x0,"Restored guest did not continue");
        }
        std::cout<<"HVF SMP, GIC, registers, virtual clock and resume passed\n"; return 0;
    } catch(const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
}
