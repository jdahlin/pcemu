#include "arm_hyperv_reference_clock.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>
using namespace pc;
namespace {
unsigned checks=0;
void check(bool value){++checks;if(!value){std::cerr<<"reference clock check "<<checks<<" failed\n";std::exit(1);}}
uint64_t get(std::span<const uint8_t> p,size_t at,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(p[at+i])<<(8*i);return v;}
uint64_t read(std::span<const uint8_t> p,uint64_t counter){return uint64_t((__uint128_t(counter)*get(p,8,8))>>64)+get(p,16,8);}
}
int main(){
 constexpr uint64_t base=0x140000000;
 std::vector<uint8_t> ram(3*4096,0xa5);ArmHyperVReferenceClock clock(ram,base);
 auto p=std::span<uint8_t>(ram).subspan(4096,4096);
 ArmHyperVReferenceClock::Sample s{240000000,10000000,24000000};
 check(clock.registration()==0 && clock.sequence()==0);
 check(!clock.update(s));check(clock.configure(base+4097,s));
 check(clock.registration()==base+4097 && clock.sequence()!=0);
 check(get(p,0,4)==clock.sequence() && get(p,4,4)==0);
 check(read(p,s.counter)==s.reference100ns);
 for(size_t i=24;i<4096;++i)check(p[i]==0);
 for(uint64_t frequency:{24000000ull,10000001ull,1000000000ull,~0ull}){
  s.frequency=frequency;
  for(uint64_t ticks:{0ull,1ull,240000000ull,~0ull}){
   s.counter=ticks;s.reference100ns=uint64_t((__uint128_t(ticks)*10000000)/frequency);
   check(clock.update(s));check(read(p,ticks)==s.reference100ns);
   if(ticks<~0ull-100000){
    auto got=read(p,ticks+100000),want=s.reference100ns+uint64_t((__uint128_t(100000)*10000000)/frequency);
    check(got>=want ? got-want<=1 : want-got<=1);
   }
  }
 }
 auto old=ram;auto reg=clock.registration();auto seq=clock.sequence();
 for(uint64_t bad:{base+4099,base-4095,base+3*4096+1,~uint64_t(4095)|1}){
  check(!clock.configure(bad,s));check(ram==old && clock.registration()==reg && clock.sequence()==seq);
 }
 check(!clock.configure(base+8193,{0,0,0}));check(ram==old);
 check(!clock.configure(base+8193,{0,~0ull,24000000}));check(ram==old);
 check(clock.configure(base+8193,{100,10,24000000}));check(get(p,0,4)==0);
 auto second=std::span<uint8_t>(ram).subspan(8192,4096);
 auto lastSeq=clock.sequence();check(clock.update({200,100,24000000}));check(clock.sequence()!=lastSeq);
 check(read(second,200)==100);
 check(!clock.update({0,0,10000000}));check(clock.sequence()==0 && get(second,0,4)==0);
 check(clock.update({300,110,24000000}));check(clock.sequence()!=0 && read(second,300)==110);
 check(clock.configure(0,{0,0,0}));check(clock.registration()==0 && get(second,0,4)==0);
 check(std::all_of(ram.begin(),ram.begin()+4096,[](uint8_t v){return v==0xa5;}));
 std::cout<<checks<<" reference clock checks passed\n";
}
