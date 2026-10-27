#include "jit.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v,const char *s){++checks;if(!v)throw std::runtime_error(s);}
static void equal(const CPU &a,const CPU &b){
    check(std::equal(a.r,a.r+8,b.r),"flag-stack GPRs");
    check(a.ip==b.ip && a.flags==b.flags && a.instructions==b.instructions && a.cr[2]==b.cr[2],"flag-stack control/flags");
    check(a.mem==b.mem,"flag-stack memory/page effects");
}
static void paging(CPU &c){c.write(0x80000,0x81007,4);for(unsigned n=0;n<256;++n)c.write(0x81000+4*n,(n<<12)|7,4);
    c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();}
int main(){try{
    for(unsigned mode=0;mode<6;++mode)for(unsigned iopl=0;iopl<4;++iopl)
    for(bool code32:{false,true})for(bool wide:{false,true})for(bool stack32:{false,true})
    for(bool paged:{false,true})for(bool push:{false,true})for(unsigned scenario=0;scenario<8;++scenario){
        auto p=std::make_unique<CPU>(1024*1024);auto &c=*p;
        c.cr[0]=mode?0x11:0x10;c.s[CS].big=code32;c.s[CS].selector=mode && mode<5?mode-1:0;
        c.s[SS].big=stack32;c.s[SS].base=0x20000;
        for(auto &s:c.s)s.limit=UINT32_MAX;
        c.flags=0x110202|(iopl<<12)|(mode==5?0x20000:0);c.r[AX]=0x123456ff;
        unsigned sp=0x8040;
        if(scenario==2)sp=push?0x8001:0x8fff;
        if(scenario==3)c.s[SS].unusable=true;
        if(scenario==4)c.s[SS].base=0xa0000-sp;
        if(scenario==7)sp=push?1:0xffff;
        c.r[SP]=(stack32?0:0xbeef0000)|sp;
        c.write(c.s[SS].base+sp,0xffffffff,4);
        std::vector<uint8_t> code={0x04,1};if(wide!=code32)code.push_back(0x66);
        code.push_back(push?0x9c:0x9d);unsigned end=code.size();code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());
        if(paged){paging(c);if(scenario==6)c.write(0x81000+0x28*4,0x28005,4);}
        auto ref=std::make_unique<CPU>(c);unsigned reads[2]={},writes[2]={};bool faults[2]={};struct Stop{};
        for(unsigned k=0;k<2;++k){auto *cpu=k?ref.get():&c;
            cpu->observeFault=[&,k](const Fault&){faults[k]=true;throw Stop{};};
            cpu->videoRead=[&,k](uint32_t){++reads[k];return uint8_t(0xff);};
            cpu->videoWrite=[&,k](uint32_t,uint8_t){++writes[k];};
            if(scenario==5)cpu->watchWrite=[&,k](uint32_t,uint32_t,int){++writes[k];};
        }
        // Warm only normal stacks. Cold writes must retain page-table effects
        // and faults; user read-only writes are deliberately left cold.
        if(paged && scenario==0)for(auto *cpu:{&c,ref.get()}){cpu->read(0,1);cpu->write(0x28000,0,1);}
        try{ref->step();ref->step();}catch(const Stop&){}
        Jit jit(c,true,0,65536);try{for(unsigned n=0;n<3 && c.ip!=end;++n)jit.run();}catch(const Stop&){}
        equal(c,*ref);check(faults[0]==faults[1] && reads[0]==reads[1] && writes[0]==writes[1],"flag-stack fault/callback ordering");
        if(scenario==0 && (mode!=5 || iopl==3))check(jit.nativeInstructions==2 && c.nativeMemoryCalls==0,"warm flag stack lowers");
    }
    // Exhaust all relevant incoming POPF bits at every privilege/IOPL pair.
    for(unsigned mode=0;mode<6;++mode)for(unsigned iopl=0;iopl<4;++iopl)
    for(unsigned bit=0;bit<32;++bit)for(bool word:{false,true}){
        if(mode==5 && iopl!=3)continue;
        auto p=std::make_unique<CPU>(65536);auto &c=*p;c.cr[0]=mode?0x11:0x10;
        c.s[CS].big=true;c.s[CS].selector=mode && mode<5?mode-1:0;c.s[SS].big=true;
        c.flags=0x310203|(iopl<<12)|(mode==5?0x20000:0);c.r[SP]=0x4000;
        // Clear VM outside VM mode; retain arbitrary high/reserved flag bits.
        if(mode!=5)c.flags&=~0x20000u;
        c.write(0x4000,1u<<bit,4);
        std::vector<uint8_t> code={0x04,1};if(word)code.push_back(0x66);code.push_back(0x9d);code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());auto ref=std::make_unique<CPU>(c);
        ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(jit.nativeInstructions==2,"POPF bit/privilege matrix lowers");
    }
    // A PUSHF that overwrites its own translated bytes uses the existing
    // code-write helper and exits after the completed instruction, once.
    for(bool word:{false,true}) {
        auto p=std::make_unique<CPU>(65536);auto &c=*p;c.cr[0]=0x11;c.s[CS].big=c.s[SS].big=true;
        c.flags=0x110202;c.r[AX]=0xff;c.r[SP]=word?2:4;
        std::vector<uint8_t> code={0x04,1};if(word)code.push_back(0x66);code.push_back(0x9c);code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());auto ref=std::make_unique<CPU>(c);
        ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(jit.nativeInstructions==2 && c.nativeMemoryCalls==1,"PUSHF code write commits once");
    }
    // IF/TF changes must stop before the following instruction, including when
    // warmed blocks would otherwise execute in a cached or directly linked tail.
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})
    for(bool links:{false,true})for(bool paged:{false,true})for(unsigned value:{2u,0x202u,0x302u}){
        auto p=std::make_unique<CPU>(1024*1024);auto &c=*p;c.cr[0]=0x11;c.s[CS].big=c.s[SS].big=true;c.flags=2;
        c.r[SP]=0x28000;c.write(0x28000,value,4);
        std::vector<uint8_t> code={0x40,0xeb,1,0x90,0x41,0x9d,0x42,0xf4};
        std::copy(code.begin(),code.end(),c.mem.begin());if(paged){paging(c);c.read(0x28000,4);}
        Jit jit(c,true,0,65536,mode);jit.setDirectLinks(links,0);jit.run();jit.run();
        for(unsigned round=0;round<20;++round){c.ip=0;c.r[SP]=0x28000;c.flags=2;c.instructions=100+round*100;
            auto ref=std::make_unique<CPU>(c);auto batch=jit.runBatch();
            for(unsigned n=0;n<batch.instructions;++n)ref->step();
            equal(c,*ref);
            check(batch.instructions==4 && c.ip==6,"POPF returns before next instruction for device poll");
        }
    }
    std::cout<<"Passed "<<checks<<" flag-stack JIT checks\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
