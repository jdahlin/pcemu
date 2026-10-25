#include "hvf_machine.hpp"
#include "arm_hyperv_reference_clock.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace pc;
// Assemble genuine guest instructions; no host function calls or relocations.
asm(R"(
.text
.p2align 2
_reference_page_guest_start:
 mov x2, #0x1000
 movk x2, #0x4000, lsl #16
1:
 ldr w6, [x2]
 mrs x0, cntvct_el0
 ldr x3, [x2, #8]
 umulh x4, x0, x3
 ldr x5, [x2, #16]
 add x4, x4, x5
 ldr w7, [x2]
 hvc #42
 b 1b
_reference_page_guest_end:
)");
extern "C" const uint8_t reference_page_guest_start[],reference_page_guest_end[];
int main(){
 unsigned checks=0;auto check=[&](bool v,const char *m){++checks;if(!v)throw std::runtime_error(m);};
 try{
  uint64_t sleep=0;HvfMachine vm(2*1024*1024,true,[&]{return sleep;},2);
  ArmHyperVReferenceClock page(vm.ram(),HvfMachine::ramBase);
  auto origin=std::chrono::steady_clock::now();
  auto reference=[&]{return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-origin).count()/100)+10000000;};
  auto sample=[&]{auto c=vm.virtualCounterSample();return ArmHyperVReferenceClock::Sample{c.value,reference(),c.frequency};};
  check(page.configure(HvfMachine::ramBase+4097,sample()),"Cannot publish reference page");
  unsigned invalidations=0,updates=0;
  vm.setCounterRebaseHandler([&](bool before){
   if(before){page.invalidate();++invalidations;check(page.sequence()==0,"Rebase did not invalidate page");}
   else{check(page.update(sample()),"Cannot rebase reference page");++updates;}
  });
  vm.writeMemory(HvfMachine::ramBase,{reference_page_guest_start,size_t(reference_page_guest_end-reference_page_guest_start)});
  vm.start(HvfMachine::ramBase);check(vm.startCPU(1,HvfMachine::ramBase,0)==0,"Cannot start VP1");
  std::array<uint64_t,2> first{};std::array<uint32_t,2> sequences{};
  auto collect=[&](bool rebased){
   std::array<bool,2> seen{};auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
   while(!(seen[0]&&seen[1]) && std::chrono::steady_clock::now()<deadline){
    auto e=vm.run(std::chrono::milliseconds(100));if(e.reason==HvfMachine::ExitReason::Canceled)continue;
    check(e.reason==HvfMachine::ExitReason::Exception && e.exceptionClass()==0x16 && (e.syndrome&0xffff)==42,"Unexpected page guest exit");
    auto value=vm.reg(4),now=reference();
    check(vm.reg(6)!=0 && vm.reg(6)==vm.reg(7),"Unstable guest reference-page sequence");
    check(value<=now+1000 && now-value<1000000,"Guest page diverged from reference domain");
    if(rebased){check(value>=first[e.cpu] && value-first[e.cpu]<30000000,"Guest page retained simulated sleep discontinuity");check(vm.reg(6)!=sequences[e.cpu],"Guest did not see new sequence");}
    else {first[e.cpu]=value;sequences[e.cpu]=uint32_t(vm.reg(6));}
    seen[e.cpu]=true;
   }
   check(seen[0]&&seen[1],"Both VPs must read reference page");
  };
  collect(false);sleep=5*vm.virtualCounterSample().frequency;
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));collect(true);
  check(invalidations==1 && updates==1,"Unexpected rebase callback order/count");
  // The handler borrows page; remove it before page destruction.
  vm.setCounterRebaseHandler({});
  std::cout<<"{\"test\":\"reference-page\",\"passed\":true,\"checks\":"<<checks<<"}\n";
 }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
