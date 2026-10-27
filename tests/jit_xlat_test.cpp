#include "jit.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v,const char*m){++checks;if(!v)throw std::runtime_error(m);}
static void equal(const CPU&a,const CPU&b,bool full=true){check(std::equal(a.r,a.r+8,b.r),"XLAT GPRs");
    check(a.ip==b.ip && a.flags==b.flags && a.instructions==b.instructions && a.cr[2]==b.cr[2],"XLAT control/flags/CR2");
    if(full)check(a.mem==b.mem,"XLAT full memory effects");
    else check(std::equal(a.mem.begin()+0x80000,a.mem.begin()+0x82000,b.mem.begin()+0x80000),"XLAT page-table effects");}
static void paging(CPU&c){c.write(0x80000,0x81007,4);for(unsigned n=0;n<256;++n)c.write(0x81000+n*4,(n<<12)|7,4);
    c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();}
int main(){try{
    for(unsigned mode=0;mode<4;++mode)for(bool code32:{false,true})for(bool address32:{false,true})
    for(bool operandOverride:{false,true})for(bool paged:{false,true})for(bool warm:{false,true})
    for(int segment:{-1,int(ES),int(CS),int(SS),int(FS),int(GS)}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=mode?0x11:0x10;c.s[CS].big=code32;c.s[SS].big=!code32;
        c.s[CS].selector=mode==2?3:0;c.flags=0x3202|(mode==3?0x20000:0);
        // Synthetic flat caches isolate lookup arithmetic; bounds have separate tests.
        for(unsigned s=0;s<6;++s){c.s[s].limit=UINT32_MAX;if(s!=CS)c.s[s].base=(s+1)*0x10000;}
        for(unsigned a=0x100;a<0x80000;++a)c.mem[a]=uint8_t((a*37)^(a>>8));
        std::vector<uint8_t> code={0x83,0xc2,1};if(operandOverride)code.push_back(0x66);if(address32!=code32)code.push_back(0x67);
        const unsigned prefix[]={0x26,0x2e,0x36,0x3e,0x64,0x65};if(segment>=0)code.push_back(prefix[segment]);
        code.push_back(0xd7);code.push_back(0xf4);std::copy(code.begin(),code.end(),c.mem.begin());if(paged)paging(c);
        auto ref=std::make_unique<CPU>(c);Jit jit(c,true,0,65536);
        for(unsigned bx:{0x4000u,0xfff0u,0xffffu,0x10000u,0x1ffffu})
        for(unsigned al=0;al<256;++al)for(unsigned upper:{0u,0x1234cd00u,0xffffab00u}){
            unsigned offset=(bx+al)&(address32?UINT32_MAX:65535u);
            unsigned linear=c.s[segment<0?DS:segment].base+offset;
            for(auto*cpu:{&c,ref.get()}){
                cpu->ip=cpu->instructions=0;cpu->flags=0x3202|(mode==3?0x20000:0);
                cpu->r[DX]=upper==0?UINT32_MAX:upper==0x1234cd00?(code32?0x7fffffffu:0x7fffu):0;
                cpu->r[AX]=upper|al;cpu->r[BX]=bx;
                if(paged){cpu->flushTLB();cpu->read(0,1);if(warm)cpu->read(linear,1);}
            }
            ref->step();ref->step();auto native=jit.nativeInstructions;jit.run();equal(c,*ref,false);
            check(c.mem[linear]==ref->mem[linear],"XLAT leaves table byte unchanged");
            check((c.r[AX]&~255u)==upper,"XLAT preserves AH and upper EAX");
            check(jit.nativeInstructions-native==(!paged||warm||(linear>>12)==0?2u:1u) && c.nativeMemoryCalls==0,"XLAT warm native / cold pre-access replay");
        }
        equal(c,*ref); // Exhaustive byte matrix plus one full-RAM comparison per context.
    }
    // A preceding AL writer changes the lookup index; its flags must survive
    // XLAT replacing AL with a byte unrelated to the arithmetic result.
    for(bool address32:{false,true})for(unsigned al:{0u,1u,127u,255u}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.flags=0xa02;
        c.r[AX]=0x1234ab00|al;c.r[BX]=0xffff;
        std::vector<uint8_t> code={0x04,1};if(!address32)code.push_back(0x67);code.push_back(0xd7);
        code.insert(code.end(),{0x74,2,0x40,0x90,0xf4});
        for(unsigned n=0x100;n<0x10100;++n)c.mem[n]=uint8_t(n*17+0x73);
        std::copy(code.begin(),code.end(),c.mem.begin());auto ref=std::make_unique<CPU>(c);ref->step();ref->step();ref->step();
        Jit jit(c,true,0,65536);jit.run();equal(c,*ref);check(jit.nativeInstructions==3,"XLAT observes cached AL writer and preserves branch flags");
    }
    // CPU address wrapping and A20 masking are independent of operand prefixes.
    for(bool a20:{false,true})for(unsigned bx:{0xffffff80u,0x100080u})for(unsigned al:{0u,127u,128u,255u}){
        auto p=std::make_unique<CPU>(2*1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.flags=0x202;c.a20=a20;
        c.r[AX]=0x1234ab00|al;c.r[BX]=bx;c.r[DX]=UINT32_MAX;
        for(unsigned n=0x100;n<c.mem.size();++n)c.mem[n]=uint8_t(n*11+(n>>16));
        const uint8_t code[]={0x83,0xc2,1,0x66,0xd7,0xf4};std::copy(std::begin(code),std::end(code),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
    }
    // Declines cannot call a device twice or alter guest code via a page walk
    // before replay. Segment/read faults must expose prior lazy flags exactly.
    for(bool paged:{false,true})for(unsigned scenario=0;scenario<7;++scenario){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.flags=0x202;c.r[AX]=0x1234ab20;
        c.r[DX]=UINT32_MAX;c.r[BX]=scenario==1?0x9ffe0:scenario==2?0x8fe0:0x3ffe0;
        if(scenario==3)c.s[DS].unusable=true;
        const uint8_t code[]={0x83,0xc2,1,0xd7,0xf4};std::copy(std::begin(code),std::end(code),c.mem.begin());
        if(paged){paging(c);if(scenario==4)c.write(0x81100,0,4);if(scenario==5){c.write(0x81100,0x40003,4);c.s[CS].selector=3;}
            if(scenario==2)c.write(0x81024,0x81007,4);c.flushTLB();}
        if(scenario==6){c.r[BX]=0xdfffffe0;c.r[AX]=0x1234ab20;}
        auto ref=std::make_unique<CPU>(c);std::vector<uint64_t> trace[2];bool faults[2]={};struct Stop{};
        for(unsigned k=0;k<2;++k){auto*cpu=k?ref.get():&c;
            cpu->videoRead=[&,k,cpu](uint32_t a){trace[k].insert(trace[k].end(),{a,cpu->r[AX],cpu->flags});return uint8_t(0x73);};
            cpu->observeFault=[&,k](const Fault&){faults[k]=true;throw Stop{};};
        }
        try{ref->step();ref->step();}catch(const Stop&){}
        Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}
        equal(c,*ref);check(trace[0]==trace[1] && faults[0]==faults[1],"XLAT callback/fault replay exactly once");
    }
    // AL feeds the next lookup while LAHF changes AH: only the low byte may
    // affect indexing, even across cached/directly linked native frames.
    for(bool links:{false,true})for(bool paged:{false,true})for(bool address32:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.flags=0x203;
        c.r[AX]=0x1234ab01;c.r[BX]=0x4000;c.r[CX]=200;
        for(unsigned n=0;n<256;++n)c.mem[0x4000+n]=uint8_t(n+17);
        std::vector<uint8_t> code;if(!address32)code.push_back(0x67);code.insert(code.end(),{0xd7,0x9f,0x49,0x75});
        code.push_back(uint8_t(-int(code.size()+1)));code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());if(paged){paging(c);c.read(0x4000,1);}
        auto ref=std::make_unique<CPU>(c);Jit jit(c,true,0,65536);jit.setDirectLinks(links,0);unsigned batches=0;
        while(!c.halted && batches++<100){auto batch=jit.runBatch();check(batch.instructions>0 && batch.instructions<=127,"XLAT batch bound");
            for(unsigned n=0;n<batch.instructions;++n)ref->step();
            equal(c,*ref);
        }
        check(c.halted && (c.r[AX]&255)==73 && (c.r[AX]&0xffff0000)==0x12340000 && (c.flags&CF),"XLAT chain preserves upper GPR and flags");
        check(jit.nativeInstructions>=790,"XLAT chain stays native");if(links)check(jit.directLinkTransitions>0,"XLAT linked loop");
    }
    std::cout<<"Passed "<<checks<<" XLAT JIT checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
