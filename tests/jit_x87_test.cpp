#include "jit.hpp"
#include <algorithm>
#include <bit>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool value,const char *message){++checks;if(!value)throw std::runtime_error(message);}
static void equal(const CPU&a,const CPU&b){
    check(std::equal(a.r,a.r+8,b.r),"x87 GPRs");
    check(a.ip==b.ip && a.flags==b.flags && a.instructions==b.instructions && a.cr[2]==b.cr[2],"x87 restart state");
    check(std::equal(a.fp,a.fp+8,b.fp) && a.fpControl==b.fpControl && a.fpStatus==b.fpStatus && a.fpTag==b.fpTag,
          "x87 arithmetic/control/status/tags");
    check(a.fpIP==b.fpIP && a.fpData==b.fpData && a.fpCS==b.fpCS && a.fpDS==b.fpDS && a.fpOpcode==b.fpOpcode,"x87 environment");
    check(a.mem==b.mem,"x87 RAM/page effects");
}
int main(){try{
    // All implemented register forms and memory groups use decoded operations.
    // Faults leave generated host frames before architectural delivery; EM/TS
    // precedes unusable segments, page faults and device operand callbacks.
    for(bool code32:{false,true})for(bool wide:{false,true})for(bool address32:{false,true})
    for(unsigned scenario=0;scenario<6;++scenario)for(unsigned op=0xd8;op<=0xdf;++op)
    for(unsigned form=0;form<72;++form){
        auto p=std::make_unique<CPU>(1024*1024);auto &c=*p;
        c.cr[0]=0x11|(scenario==1?4:scenario==2?8:0);c.s[CS].big=code32;c.s[SS].big=true;
        c.r[AX]=0xff;c.flags=0x202;c.fpStatus=3<<11;c.fpTag=0;c.fpControl=0x77f;
        for(unsigned n=0;n<8;++n)c.fp[n]=std::bit_cast<uint64_t>(double(n+1)+.25);
        unsigned m=form<64?0xc0+form:((form-64)<<3)|(address32?5:6);
        unsigned target=scenario==3?0x28fff:scenario==4?0xa0000:0x24000;
        c.write(target,0x3fc00000,4);c.write(target+4,0x3ff80000,4);
        std::vector<uint8_t> code={0x04,1};
        if(wide!=code32)code.push_back(0x66);
        if(address32!=code32)code.push_back(0x67);
        code.push_back(op);code.push_back(m);
        if(form>=64)for(unsigned n=0;n<(address32?4u:2u);++n)code.push_back(target>>(8*n));
        const auto end=code.size();code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());
        if(scenario==3){
            c.write(0x80000,0x81007,4);
            for(unsigned n=0;n<256;++n)c.write(0x81000+4*n,(n<<12)|7,4);
            c.write(0x81000+4*0x29,0,4);c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();
        }
        if(scenario==5)c.s[DS].unusable=true;
        auto ref=std::make_unique<CPU>(c);unsigned reads[2]={},writes[2]={},faults[2]={};struct Stop{};
        for(unsigned n=0;n<2;++n){auto *cpu=n?ref.get():&c;
            cpu->observeFault=[&,n](const Fault&f){faults[n]=f.vector+1;throw Stop{};};
            cpu->videoRead=[&,n](uint32_t){++reads[n];return uint8_t(0);};
            cpu->videoWrite=[&,n](uint32_t,uint8_t){++writes[n];};
        }
        try{ref->step();ref->step();}catch(const Stop&){}
        Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}
        try{equal(c,*ref);}catch(...){std::cerr<<"op="<<std::hex<<op<<" m="<<m<<" scenario="<<scenario<<" code32="<<code32<<" wide="<<wide<<" addr32="<<address32<<" IP="<<c.ip<<"/"<<ref->ip<<" flags="<<c.flags<<"/"<<ref->flags<<" count="<<c.instructions<<"/"<<ref->instructions<<" CR2="<<c.cr[2]<<"/"<<ref->cr[2]<<"\n";throw;}
        check(reads[0]==reads[1] && writes[0]==writes[1] && faults[0]==faults[1],"x87 fault/device ordering");
        check(jit.nativeInstructions==2 && jit.nativeHelperInstructions==1,"decoded x87 is compiled helper work");
        if(!faults[0])check(c.ip==end,"decoded x87 end IP");
    }
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace}) {
        CPU c(65536);c.s[CS].big=true;c.s[CS].base=0x1000;
        const uint8_t code[]={0xb8,1,0,0,0,0x40,0x41,0x42,0xf4};
        std::copy(std::begin(code),std::end(code),c.mem.begin()+0x1000);
        Jit jit(c,true,0,65536,mode);jit.setObservationPoints({0x1006});jit.run();
        check(c.ip==6 && c.r[AX]==2 && c.r[CX]==0 && jit.nativeInstructions==2,"exact native observation boundary");
        jit.run();check(c.r[CX]==1 && c.r[DX]==1,"observation does not skip target instruction");
    }
    // Warming both the fence entry and its predecessors must not let the
    // cached batching path execute through an observation boundary.
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})
    for(bool warm:{false,true}) {
        CPU c(65536);c.cr[0]=0x11;c.s[CS].big=true;
        c.mem[0]=0xeb;c.mem[1]=6;c.mem[8]=0xeb;c.mem[9]=6;c.mem[16]=0x40;c.mem[17]=0xf4;
        Jit jit(c,true,0,65536,mode);jit.setObservationPoints({16});
        if(warm)for(unsigned ip:{0u,8u,16u}){c.ip=ip;jit.run();}
        c.ip=0;c.r[AX]=0;c.halted=false;auto before=jit.nativeInstructions;
        auto batch=jit.runBatch(64);
        check(c.ip==16 && c.r[AX]==0 && batch.instructions==2 && jit.nativeInstructions-before==2,
              "cold/cached batching stops before observation point");
        jit.runBatch(64);check(c.r[AX]==1 && c.halted,"next batch executes observed entry once");
        bool rejected=false;try{jit.setDirectLinks(true);}catch(const std::invalid_argument&){rejected=true;}
        check(rejected,"links cannot be enabled after observation points");
    }
    std::cout<<"Passed "<<checks<<" decoded x87 JIT checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
