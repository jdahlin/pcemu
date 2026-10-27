#include "jit.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool value,const char *message){++checks;if(!value)throw std::runtime_error(message);}
static void equal(const CPU&a,const CPU&b){
    check(std::equal(a.r,a.r+8,b.r),"decoded boundary registers");
    check(a.ip==b.ip && a.flags==b.flags && a.instructions==b.instructions && a.cr[2]==b.cr[2],"decoded boundary restart state");
    check(a.mem==b.mem,"decoded boundary memory/page effects");
}
int main(){try{
    // INT's return IP and canonical prefix registers are visible to callbacks
    // only after the generated host frame has returned. Both handled BIOS calls
    // and actual real-mode frame construction match reference execution.
    for(unsigned vector:{3u,0x31u})for(bool handled:{false,true})for(bool code32:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.s[CS].big=code32;c.r[SP]=0x8000;c.r[AX]=0xff;c.flags=0x202;
        c.write(vector*4,0x12340456,4);
        const uint8_t code[]={0x04,1,0xcd,uint8_t(vector)};std::copy(std::begin(code),std::end(code),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);unsigned callbacks[2]={};
        for(unsigned n=0;n<2;++n){auto*cpu=n?ref.get():&c;cpu->bios=[&,cpu,n](uint8_t v){
            ++callbacks[n];check(v==vector && cpu->ip==4 && cpu->r[AX]==0,"decoded INT observes committed prefix");return handled;};}
        ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(callbacks[0]==1 && callbacks[1]==1 && jit.nativeInstructions==2,"INT remains compiled across callbacks");
    }
    // Cold RAM, MMIO and late write faults must use decoded memory semantics,
    // preserving the read-before-write order and precise flags on restart.
    for(unsigned group:{0u,1u,2u,3u,4u,5u,6u,7u})for(bool multiply:{false,true})for(unsigned scenario=0;scenario<3;++scenario){
        if(multiply && group<4)continue;
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.r[AX]=7;c.r[DX]=0;c.r[CX]=3;c.flags=0x203;
        const unsigned at=scenario==1?0xa0000:0x24000;c.write(at,2,4);
        std::vector<uint8_t> code={0x04,1,uint8_t(multiply?0xf7:0xc1),uint8_t((group<<3)|5)};
        for(unsigned n=0;n<4;++n)code.push_back(at>>(8*n));if(!multiply)code.push_back(3);
        std::copy(code.begin(),code.end(),c.mem.begin());
        c.write(0x80000,0x81007,4);for(unsigned n=0;n<256;++n)c.write(0x81000+n*4,(n<<12)|7,4);
        if(scenario==2)c.write(0x81000+(at>>12)*4,0,4);
        c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();
        auto ref=std::make_unique<CPU>(c);unsigned reads[2]={},writes[2]={},faults[2]={};struct Stop{};
        for(unsigned n=0;n<2;++n){auto*cpu=n?ref.get():&c;
            cpu->observeFault=[&,n](const Fault&f){faults[n]=f.vector+1;throw Stop{};};
            cpu->videoRead=[&,n](uint32_t a){++reads[n];return uint8_t((a&3)?0:2);};
            cpu->videoWrite=[&,n](uint32_t,uint8_t){++writes[n];};
        }
        try{ref->step();ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}
        equal(c,*ref);check(reads[0]==reads[1] && writes[0]==writes[1] && faults[0]==faults[1],"decoded memory fault/callback ordering");
        check(jit.nativeInstructions==2,"cold/MMIO arithmetic uses compiled decoded helper");
    }
    // TEST reads its operand and writes flags, never a register destination.
    for(bool byte:{false,true})for(bool immediate:{false,true}) {
        CPU c(1024*1024);c.cr[0]=0x11;c.s[CS].big=true;c.r[AX]=0x12345678;c.flags=0x203;
        std::vector<uint8_t> code={uint8_t(immediate?(byte?0xf6:0xf7):(byte?0x84:0x85)),5,0,0,10,0};
        if(immediate)for(unsigned n=0;n<(byte?1u:4u);++n)code.push_back(0xff);
        std::copy(code.begin(),code.end(),c.mem.begin());auto ref=std::make_unique<CPU>(c);
        for(auto cpu:{&c,ref.get()})cpu->videoRead=[](uint32_t){return uint8_t(0x55);};
        ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(c.r[AX]==0x12345678 && jit.nativeInstructions==1,"decoded TEST has no register writeback");
    }
    // A late MMIO guard in an already promoted loop must leave linked/cached
    // frames, complete exactly one decoded operation, and stop for device poll.
    // Callback changes to EAX cannot replace AND's already captured left operand.
    for(bool links:{false,true}) {
        CPU c(1024*1024);c.cr[0]=0x11;c.s[CS].big=true;c.r[SI]=0x24000;c.write(0x24000,0xffffffff,4);
        const uint8_t code[]={0x83,0xc0,1,0x23,0x06,0xeb,0xf9};
        std::copy(std::begin(code),std::end(code),c.mem.begin());
        Jit jit(c,true,0,65536);jit.setDirectLinks(links,0);
        for(unsigned n=0;n<16;++n){c.ip=0;c.r[AX]=0x12345678;c.flags=0x202;jit.runBatch(32);}
        c.ip=0;c.r[AX]=0x12345678;c.flags=0x202;c.r[SI]=0xa0000;
        auto ref=std::make_unique<CPU>(c);unsigned reads[2]={};
        for(unsigned n=0;n<2;++n){auto*cpu=n?ref.get():&c;cpu->videoRead=[&,cpu,n](uint32_t a){++reads[n];cpu->r[AX]=0xdeadbeef;return uint8_t(0x0f0f0f0fu>>((a&3)*8));};}
        auto before=jit.nativeInstructions;auto batch=jit.runBatch(32);
        for(unsigned n=0;n<batch.instructions;++n)ref->step();equal(c,*ref);
        check(batch.instructions==2 && jit.nativeInstructions-before==2 && reads[0]==4 && reads[1]==4,
              "late decoded guard exits linked frames exactly once");
        if(links)check(jit.directLinkTransitions>0,"decoded guard exercised promoted code");
    }
    std::cout<<"Passed "<<checks<<" decoded boundary JIT checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
