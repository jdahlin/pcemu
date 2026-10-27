#include "jit.hpp"
#include "jit_runtime.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool b,const char *m) {++checks;if(!b)throw std::runtime_error(m);}
static void equal(const CPU &a,const CPU &b) {
    check(std::equal(a.r,a.r+8,b.r),"segment GPRs");
    check(a.ip==b.ip && a.flags==b.flags && a.instructions==b.instructions &&
          a.interruptBlockedThrough==b.interruptBlockedThrough && a.cr[2]==b.cr[2],"segment control/shadow");
    for(unsigned i=0;i<6;++i)check(a.s[i].base==b.s[i].base && a.s[i].limit==b.s[i].limit &&
        a.s[i].selector==b.s[i].selector && a.s[i].big==b.s[i].big && a.s[i].unusable==b.s[i].unusable,"hidden segment state");
    check(a.mem==b.mem,"segment RAM/page effects");
}
static void paging(CPU &c) {
    c.write(0x80000,0x81007,4);
    for(unsigned page=0;page<256;++page)c.write(0x81000+4*page,(page<<12)|7,4);
    c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();
}
int main(){try {
    for(unsigned mode=0;mode<3;++mode) for(bool code32:{false,true})
    for(bool wide:{false,true}) for(bool address32:{false,true})
    for(bool load:{false,true}) for(unsigned seg=0;seg<6;++seg)
    for(unsigned form=0;form<3;++form) for(bool paged:{false,true}) {
        if(load && seg==CS)continue; // MOV CS retains the interpreter path.
        auto p=std::make_unique<CPU>(1024*1024);auto &c=*p;
        c.cr[0]=mode?0x11:0x10;c.s[CS].big=code32;c.flags=0x202|(mode==2?0x20000:0);
        for(auto &s:c.s)s.limit=UINT32_MAX;
        for(unsigned r=0;r<8;++r)c.r[r]=0x12340000+r*0x1111;
        c.r[AX]=0x123456ff;c.r[BX]=load?0x12340008:0x12345678;c.r[SI]=0x4000;
        c.gdtBase=0x10000;c.gdtLimit=0x17;
        c.write(0x10008,0x3456abcd,4);c.write(0x1000c,0x12c09278,4);
        c.write(0x4000,8,2);
        // Selector is read dynamically; MOV from a segment must preserve word
        // upper halves and zero-extend dword register destinations.
        if(!load)c.s[seg].selector=0x9876;
        std::vector<uint8_t> code={0x04,1};
        if(wide!=code32)code.push_back(0x66);
        if(address32!=code32)code.push_back(0x67);
        code.push_back(load?0x8e:0x8c);
        if(form==0)code.push_back(0xc0|(seg<<3)|BX);
        else if(form==1) {
            code.push_back((seg<<3)|(address32?5:6));
            for(unsigned n=0;n<(address32?4u:2u);++n)code.push_back(0x4000>>(8*n));
        } else {
            code.push_back((seg<<3)|(address32?SI:4)); // [ESI] / [SI].
        }
        auto end=code.size();code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());
        if(paged)paging(c);
        auto ref=std::make_unique<CPU>(c);
        // Warm descriptor/source pages identically. Cold cases are tested below.
        if(paged)for(auto *cpu:{&c,ref.get()}){cpu->read(0,1);cpu->systemRead(0x10008,4);cpu->read(0x4000,2);}
        ref->step();ref->step();
        Jit jit(c,true,0,65536);
        for(unsigned n=0;n<3 && c.ip!=end;++n)jit.run();
        equal(c,*ref);
        if(!(load && seg==CS))check(jit.nativeInstructions==2,"segment operation must lower");
    }
    for(bool load:{false,true})for(bool word:{false,true})for(unsigned reg=0;reg<8;++reg)
    for(unsigned seg:{unsigned(ES),unsigned(SS),unsigned(DS),unsigned(FS),unsigned(GS)}) {
        auto p=std::make_unique<CPU>(1024*1024);auto &c=*p;
        c.cr[0]=0x11;c.s[CS].big=true;c.flags=0x202;
        c.gdtBase=0x10000;c.gdtLimit=0x17;c.write(0x10008,0xffff,4);c.write(0x1000c,0x409200,4);
        c.r[reg]=0xdead0008;c.s[seg].selector=0x9876;
        std::vector<uint8_t> code={0x90,0x90};if(word)code.push_back(0x66);
        code.push_back(load?0x8e:0x8c);code.push_back(0xc0|(seg<<3)|reg);code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());auto ref=std::make_unique<CPU>(c);
        ref->step();ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(jit.nativeInstructions==3,"all selector GPR assignments lower");
    }
    // Warm descriptor probes must remain current even for the same selector,
    // LDT descriptors, ring-3 system reads, null selectors and real/VM loads.
    for(unsigned mode=0;mode<3;++mode)for(unsigned seg:{unsigned(ES),unsigned(SS),unsigned(DS),unsigned(FS),unsigned(GS)})
    for(bool ldt:{false,true})for(bool paged:{false,true})for(unsigned variant=0;variant<8;++variant){
        auto p=std::make_unique<CPU>(1024*1024);auto &c=*p;
        c.cr[0]=mode?0x11:0x10;c.flags=0x202|(mode==2?0x20000:0);c.s[CS].selector=3;
        c.gdtBase=c.ldtBase=0x10000;c.gdtLimit=c.ldtLimit=0x17;
        unsigned sel=(ldt?12:8),hi=0x12c09278;
        if(variant==1)sel=0;
        if(variant==2)hi&=~0x8000u; // Nonpresent valid data -> #NP/#SS.
        if(variant==3)hi=0; // Invalid and absent -> #GP first.
        if(variant==4)hi=0x12c09878; // Execute-only code -> #GP for data/SS.
        if(variant==5)c.gdtLimit=c.ldtLimit=0;
        c.write(0x10008,0x3456abcd,4);c.write(0x1000c,hi,4);
        if(paged)paging(c);
        auto ref=std::make_unique<CPU>(c);
        if(paged && variant!=6)for(auto *cpu:{&c,ref.get()})cpu->systemRead(0x10008,4);
        bool fault=false;try{ref->segment(seg,sel);}catch(const Fault&){fault=true;}
        if(!fault && seg==SS)ref->interruptBlockedThrough=7;
        auto before=c.mem;
        auto result=pc::jit_detail::nativeSegment(&c,seg,sel,7);
        if(paged && variant==6 && mode==1){
            check(result==0 && c.mem==before,"cold descriptor declines without A/D writes");
        }else if(fault)check(result==0,"invalid descriptor must replay");
        else {check(result==1,"valid segment helper");equal(c,*ref);}
        if(result==1 && mode==1 && sel && variant==7) {
            for(auto *cpu:{&c,ref.get()}){cpu->write(0x10008,0x98761234,4);cpu->write(0x1000c,0x34409256,4);}
            ref->segment(seg,sel);pc::jit_detail::nativeSegment(&c,seg,sel,7);equal(c,*ref);
        }
    }
    // Source/descriptor faults, device callbacks and shadow boundaries must
    // replay exactly once after cached prefix registers and flags are committed.
    for(bool sourceDevice:{false,true})for(bool faulting:{false,true})for(bool ss:{false,true}) {
        auto p=std::make_unique<CPU>(1024*1024);auto &c=*p;
        c.cr[0]=0x11;c.s[CS].big=true;c.flags=0x202;c.r[AX]=0xff;
        c.gdtBase=sourceDevice?0x10000:0xa0000;c.gdtLimit=0x17;
        c.write(0x10008,0xffff,4);c.write(0x1000c,0x409200,4);
        unsigned target=sourceDevice?0xa0000:0x4000;c.write(0x4000,8,2);
        std::vector<uint8_t> code={0x04,1,0x8e,uint8_t((ss?SS:DS)<<3|5)};
        for(unsigned n=0;n<4;++n)code.push_back(target>>(8*n));
        code.push_back(0xf4);std::copy(code.begin(),code.end(),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);unsigned reads[2]={};bool faults[2]={};struct Stop{};
        for(unsigned k=0;k<2;++k){auto *cpu=k?ref.get():&c;
            cpu->videoRead=[&,k](uint32_t addr){++reads[k];if(sourceDevice)return uint8_t((addr&1)?0:8);
                unsigned hi=faulting?0:0x409200;return uint8_t(addr<0xa000c?0xff:hi>>((addr-0xa000c)*8));};
            cpu->observeFault=[&,k](const Fault&){faults[k]=true;throw Stop{};};
        }
        // A source device returns a valid selector; force its descriptor invalid when requested.
        if(sourceDevice && faulting)for(auto *cpu:{&c,ref.get()})cpu->write(0x1000c,0,4);
        try{ref->step();ref->step();}catch(const Stop&){}
        Jit jit(c,true,0,65536);try{for(unsigned n=0;n<3 && c.ip<8;++n)jit.run();}catch(const Stop&){}
        equal(c,*ref);check(reads[0]==reads[1] && faults[0]==faults[1],"segment replay callbacks/faults once");
    }
    // Segment loads may continue into memory operations and linked frames;
    // their new hidden base must take effect without a stale-address cache.
    for(bool linked:{false,true})for(bool paged:{false,true})for(bool memory:{false,true}) {
        auto p=std::make_unique<CPU>(1024*1024);auto &c=*p;
        c.cr[0]=0x11;c.s[CS].big=true;c.flags=0x203;c.r[BX]=8;c.r[CX]=80;c.r[SI]=0x4000;
        c.gdtBase=0x10000;c.gdtLimit=0x17;c.write(0x10008,0xffff,4);c.write(0x1000c,0x409202,4);
        c.write(0x4000,8,2);c.write(0x24000,8,2);c.write(0x24010,0x12345678,4);
        std::vector<uint8_t> code={0x40,0x8e,uint8_t(memory?0x1e:0xdb),0x8c,0xda,
                                  0x8b,0x3d,0x10,0x40,0,0,0x49,0x75,0xf2,0xf4};
        std::copy(code.begin(),code.end(),c.mem.begin());if(paged)paging(c);
        if(paged){c.systemRead(0x10008,4);c.read(0x4000,2);c.read(0x24000,2);}
        auto ref=std::make_unique<CPU>(c);Jit jit(c,true,0,65536);jit.setDirectLinks(linked,0);
        unsigned batches=0;
        while(!c.halted && batches++<100){auto batch=jit.runBatch();
            check(batch.instructions>0 && batch.instructions<=127,"segment batch bound");
            for(unsigned n=0;n<batch.instructions;++n)ref->step();
            equal(c,*ref);
        }
        check(c.halted && c.r[DI]==0x12345678 && c.s[DS].base==0x20000,"new segment base used by following loads");
        check(jit.nativeInstructions>=470,"segment loop stays native");
        if(linked)check(jit.directLinkTransitions>0,"segment loop links");
    }
    // A descriptor's low word can be warm while its high word faults on
    // another page. No architectural segment update may escape the replay.
    for(bool memory:{false,true})for(unsigned seg:{unsigned(DS),unsigned(SS)}) {
        auto p=std::make_unique<CPU>(1024*1024);auto &c=*p;
        c.cr[0]=0x11;c.s[CS].big=true;c.flags=0x202;c.r[AX]=0xff;c.r[BX]=8;
        c.gdtBase=0x10ff4;c.gdtLimit=0x17;c.write(0x10ffc,0xffff,4);c.write(0x4000,8,2);
        std::vector<uint8_t> code={0x04,1,0x8e,uint8_t((seg<<3)|(memory?5:0xc3))};
        if(memory)code.insert(code.end(),{0,0x40,0,0});
        code.push_back(0xf4);std::copy(code.begin(),code.end(),c.mem.begin());
        paging(c);c.write(0x81000+0x11*4,0,4);c.flushTLB();
        c.systemRead(0x10ffc,4);c.read(0x4000,2);
        auto ref=std::make_unique<CPU>(c);bool faults[2]={};struct Stop{};
        for(unsigned k=0;k<2;++k){auto *cpu=k?ref.get():&c;
            cpu->observeFault=[&,k,cpu](const Fault &f){
                check(f.vector==14 && f.error==0 && cpu->cr[2]==0x11000 && cpu->ip==2,
                      "descriptor high-word page fault at original instruction");
                faults[k]=true;throw Stop{};
            };
        }
        try{ref->step();ref->step();}catch(const Stop&){}
        Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}
        equal(c,*ref);check(faults[0] && faults[1],"descriptor page fault delivered once");
    }
    // A cached tail may have completed instructions not yet added to CPU's
    // counter. MOV SS must leave that tail before establishing its shadow.
    for(bool linked:{false,true})for(bool paged:{false,true}) {
        auto p=std::make_unique<CPU>(1024*1024);auto &c=*p;
        c.cr[0]=0x11;c.s[CS].big=true;c.flags=0x202;c.r[DX]=8;
        c.gdtBase=0x10000;c.gdtLimit=0x17;
        c.write(0x10008,0xffff,4);c.write(0x1000c,0x409200,4);
        std::vector<uint8_t> code={0x40,0xeb,1,0x90,0x41,0x8e,0xd2,0x40,0xf4};
        std::copy(code.begin(),code.end(),c.mem.begin());if(paged)paging(c);
        if(paged)c.systemRead(0x10008,4);
        Jit jit(c,true,0,65536);jit.setDirectLinks(linked,0);
        jit.run();jit.run();
        for(unsigned round=0;round<20;++round) {
            c.ip=0;c.r[AX]=c.r[CX]=0;c.instructions=100+round*100;c.interruptBlockedThrough=0;
            auto ref=std::make_unique<CPU>(c);auto batch=jit.runBatch();
            for(unsigned n=0;n<batch.instructions;++n)ref->step();
            equal(c,*ref);
            check(batch.instructions==5 && c.interruptBlockedThrough==104+round*100 && c.ip==8,
                  "MOV SS shadow follows accounted prefix and protects next instruction");
        }
    }
    std::cout<<"Passed "<<checks<<" segment JIT checks\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
