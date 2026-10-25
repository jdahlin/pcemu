#include "hvf_machine.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace pc;
int main(){
 unsigned checks=0;
 auto check=[&](bool v,const char *m){++checks;if(!v)throw std::runtime_error(m);};
 try {
  uint64_t sleep=0;HvfMachine vm(2*1024*1024,true,[&]{return sleep;},2);
  // MRS X0,CNTVCT_EL0; MRS X1,CNTFRQ_EL0; HVC #42; B back to MRS.
  constexpr std::array<uint32_t,4> code{0xd53be040,0xd53be001,0xd4000542,0x17fffffd};
  vm.writeMemory(HvfMachine::ramBase,{reinterpret_cast<const uint8_t*>(code.data()),sizeof(code)});
  vm.start(HvfMachine::ramBase);check(vm.startCPU(1,HvfMachine::ramBase,0)==0,"Cannot start second VP");
  std::array<uint64_t,2> first{};
  auto collect=[&](bool resumed){
   std::array<bool,2> seen{};
   auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
   while(!(seen[0]&&seen[1]) && std::chrono::steady_clock::now()<deadline){
    auto exit=vm.run(std::chrono::milliseconds(100));
    if(exit.reason==HvfMachine::ExitReason::Canceled)continue;
    if(exit.exceptionClass()!=0x16) std::cerr<<"Counter guest ESR="<<std::hex<<exit.syndrome<<" PC="<<exit.pc<<std::dec<<" ELR="<<std::hex<<vm.systemReg(0xc201)<<" ESR_EL1="<<vm.systemReg(0xc290)<<std::dec<<" CPU="<<exit.cpu<<"\n";
    check(exit.reason==HvfMachine::ExitReason::Exception && exit.exceptionClass()==0x16 && (exit.syndrome&0xffff)==42,"Unexpected counter guest exit");
    auto guest=vm.reg(0),frequency=vm.reg(1);auto host=vm.virtualCounterSample();
    check(frequency==host.frequency && frequency>10000000,"Guest counter frequency mismatch");
    check(host.value>=guest && host.value-guest<frequency,"Host virtual-counter sampling mismatch");
    if(resumed){check(host.sleepAdjustment==sleep,"Sleep adjustment not captured");check(guest-first[exit.cpu]>4*frequency,"Guest counter did not advance after simulated sleep");}
    else first[exit.cpu]=guest;
    seen[exit.cpu]=true;
   }
   check(seen[0]&&seen[1],"Missing VP counter observation");
  };
  collect(false);
  auto sample=vm.virtualCounterSample();sleep=5*sample.frequency;
  // Clock synchronization deliberately checks at most once per host second.
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  collect(true);
  std::cout<<"{\"test\":\"reference-counter\",\"passed\":true,\"checks\":"<<checks<<"}\n";
 }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
