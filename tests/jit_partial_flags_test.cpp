#include "jit.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v,const char*m){++checks;if(!v)throw std::runtime_error(m);}
static void equal(const CPU&a,const CPU&b){check(std::equal(a.r,a.r+8,b.r),"partial-flags GPRs");
    check(a.ip==b.ip && a.flags==b.flags && a.instructions==b.instructions && a.cr[2]==b.cr[2],"partial-flags flags/IP/count/CR2");check(a.mem==b.mem,"partial-flags RAM");}
int main(){try{
    for(bool code32:{false,true})for(unsigned op:{0xf5u,0xf8u,0xf9u,0x9eu})
    for(unsigned image=0;image<256;++image)for(unsigned flags:{2u,0xad7u,0x3202u,0x30002u}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.s[CS].big=code32;c.r[AX]=0x123400ab|(image<<8);c.flags=flags;
        // Prefixes do not change the width of these fixed flag operations.
        c.mem[0]=0x66;c.mem[1]=0x67;c.mem[2]=op;c.mem[3]=0xf4;
        auto ref=std::make_unique<CPU>(c);ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(jit.nativeInstructions==1,"every partial flag form native");
    }
    // Arithmetic flags may be lazy. SAHF preserves prior OF; carry controls
    // preserve every other flag. Follow with ADC, LAHF, SETO or a full writer.
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})
    for(unsigned op:{0xf5u,0xf8u,0xf9u,0x9eu})for(unsigned value:{0u,1u,0x7fu,0x80u,0xffu})
    for(unsigned image:{0u,1u,0x55u,0x80u,0xd5u,0xffu})for(unsigned tail=0;tail<4;++tail){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.s[CS].big=true;c.r[AX]=0x12340000|(image<<8)|value;c.r[DX]=0x87654321;c.flags=0xa03;
        std::vector<uint8_t> code={0x04,1,uint8_t(op)};
        if(tail==0)code.insert(code.end(),{0x80,0xd2,0}); // ADC DL,0
        if(tail==1)code.push_back(0x9f);
        if(tail==2)code.insert(code.end(),{0x0f,0x90,0xc2}); // SETO DL: interpreter observes restart flags
        if(tail==3)code.insert(code.end(),{0x80,0xea,1}); // SUB DL,1 kills flags
        auto instructions=3u;code.push_back(0xf4);std::copy(code.begin(),code.end(),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);for(unsigned n=0;n<instructions;++n)ref->step();Jit jit(c,true,0,65536,mode);while(c.ip<code.size()-1)jit.run();equal(c,*ref);
        check(jit.nativeInstructions==(tail==2?2:instructions),"partial flag producer/update native, SETO replay exact");
    }
    // Mixed updates and consumers in cached/linked frames. Every iteration
    // observes CMC input, SAHF OF preservation and reserved AH bit masking.
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})for(bool links:{false,true}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.s[CS].big=true;c.r[AX]=0x1234ff7f;c.r[CX]=200;c.r[DX]=0x87654321;c.flags=0xa03;
        std::vector<uint8_t> code={0x04,1,0xf8,0xf5,0x80,0xd2,0,0xf9,0xf5,0x9e,0x70,0,0x9f,0x49,0x75};
        code.push_back(uint8_t(-int(code.size()+1)));code.push_back(0xf4);std::copy(code.begin(),code.end(),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);Jit jit(c,true,0,65536,mode);jit.setDirectLinks(links,0);unsigned batches=0;
        while(!c.halted&&batches++<512){auto batch=jit.runBatch();check(batch.instructions>0 && batch.instructions<=127,"partial flag batch bound");
            for(unsigned n=0;n<batch.instructions;++n)ref->step();equal(c,*ref);}
        check(c.halted && c.r[CX]==0 && jit.nativeInstructions>=2190,"partial flags cached/trace loop native");
        if(links&&mode!=Jit::BlockMode::Trace)check(jit.directLinkTransitions>0,"partial flags linked loop");
    }
    std::cout<<"Passed "<<checks<<" partial flags JIT checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
