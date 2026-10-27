#include "jit.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v,const char*m){++checks;if(!v)throw std::runtime_error(m);}
static void equal(const CPU&a,const CPU&b){check(std::equal(a.r,a.r+8,b.r),"LOOP GPRs/counter");
    check(a.ip==b.ip && a.flags==b.flags && a.instructions==b.instructions,"LOOP target/flags/count");
    check(a.mem==b.mem,"LOOP leaves memory unchanged");}
int main(){try{
    for(bool code32:{false,true})for(bool operandOverride:{false,true})for(bool addressOverride:{false,true})
    for(unsigned op=0xe0;op<=0xe3;++op)for(bool zero:{false,true})for(bool pending:{false,true})
    for(unsigned count:{0u,1u,2u,0xffffu,0x10000u,0x10001u,0x80000000u,0xffffffffu})
    for(int displacement:{-128,-1,0,1,127}){
        auto p=std::make_unique<CPU>(128*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=code32;
        c.r[AX]=0x12345678;c.r[CX]=count;c.flags=0x133a97|(zero?ZF:0);if(!zero)c.flags&=~uint32_t(ZF);
        std::vector<uint8_t> code;
        if(pending)code={0x3c,uint8_t(zero?0x78:0x79)};else code={0x90,0x90};
        if(operandOverride)code.push_back(0x66);if(addressOverride)code.push_back(0x67);code.push_back(0x64);
        code.push_back(op);code.push_back(uint8_t(displacement));auto end=code.size();code.push_back(0xf4);
        // Set initial ZF opposite to the pending CMP result to catch stale reads.
        if(pending)c.flags^=ZF;
        std::copy(code.begin(),code.end(),c.mem.begin()+0x200);c.ip=0x200;
        auto ref=std::make_unique<CPU>(c);unsigned expectedInstructions=pending?2:3;
        for(unsigned n=0;n<expectedInstructions;++n)ref->step();
        Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(jit.nativeInstructions==expectedInstructions,"LOOP family lowers natively");
        if(op==0xe0||op==0xe1){bool counter32=addressOverride?!code32:code32;
            unsigned remaining=counter32?count-1:(count-1)&65535u;
            bool take=remaining && zero==(op==0xe1);
            unsigned target=(0x200+end+displacement)&((operandOverride?!code32:code32)?UINT32_MAX:65535u);
            check(c.ip==(take?target:0x200+end),"LOOPE/LOOPNE uses post-decrement count and prior ZF");
        }
    }
    // The ZF producer can alias the counter itself. LOOP must use flags from
    // CMP before decrementing that same CX/ECX value, never flags of the decrement.
    for(bool code32:{false,true})for(bool addressOverride:{false,true})for(unsigned op:{0xe0u,0xe1u})
    for(unsigned count:{0u,1u,2u,0x10001u}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=code32;c.r[CX]=count;c.flags=0x202;
        std::vector<uint8_t> code={0x83,0xf9,1};if(addressOverride)code.push_back(0x67);code.push_back(op);code.push_back(2);code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());auto ref=std::make_unique<CPU>(c);ref->step();ref->step();
        Jit jit(c,true,0,65536);jit.run();equal(c,*ref);check(jit.nativeInstructions==2,"counter producer and conditional LOOP are native");
    }
    // Relative targets wrap at the selected operand size independently of CX/ECX.
    for(bool code32:{false,true})for(bool operandOverride:{false,true})for(bool addressOverride:{false,true})
    for(unsigned op:{0xe0u,0xe1u})for(unsigned start:{0xfff0u,0xfffbu}){
        auto p=std::make_unique<CPU>(128*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=code32;c.r[CX]=2;c.flags=0x202|(op==0xe1?ZF:0);
        std::vector<uint8_t> code={0x90,0x90};if(operandOverride)code.push_back(0x66);if(addressOverride)code.push_back(0x67);
        code.push_back(op);code.push_back(127);unsigned pos=start;
        for(auto b:code){c.mem[pos]=b;pos=(pos+1)&(code32?UINT32_MAX:65535u);}c.ip=start;
        auto ref=std::make_unique<CPU>(c);ref->step();ref->step();ref->step();Jit jit(c,true,0,65536);
        while(c.instructions<3)jit.run();
        equal(c,*ref);
    }
    // Cached and linked branches must consume fresh flags on every iteration.
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})
    for(bool links:{false,true})for(bool wordCounter:{false,true})for(unsigned op:{0xe0u,0xe1u}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.r[CX]=0x12340000|200;c.r[AX]=0;
        if(!wordCounter)c.r[CX]=200;c.flags=0xa03;
        std::vector<uint8_t> code={0x40,0x3c,uint8_t(op==0xe1?1:0xff)};
        if(wordCounter)code.push_back(0x67);code.push_back(op);code.push_back(uint8_t(-int(code.size()+1)));code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());auto ref=std::make_unique<CPU>(c);Jit jit(c,true,0,65536,mode);jit.setDirectLinks(links,0);
        unsigned batches=0;while(!c.halted && batches++<100){auto batch=jit.runBatch();check(batch.instructions>0 && batch.instructions<=127,"conditional LOOP batch bound");
            for(unsigned n=0;n<batch.instructions;++n)ref->step();
            equal(c,*ref);
        }
        check(c.halted,"conditional LOOP finite execution");check(jit.nativeInstructions>=6,"conditional LOOP cache executes natively");
        if(links && op==0xe0)check(jit.directLinkTransitions>0,"conditional LOOP traverses direct links");
    }
    std::cout<<"Passed "<<checks<<" conditional LOOP JIT checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
