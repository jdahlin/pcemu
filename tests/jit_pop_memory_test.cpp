#include "jit.hpp"
#include "jit_runtime.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v,const char*m){++checks;if(!v)throw std::runtime_error(m);}
static void equal(const CPU&a,const CPU&b){
    check(std::equal(a.r,a.r+8,b.r),"POP r/m registers");
    check(a.ip==b.ip && a.flags==b.flags && a.instructions==b.instructions && a.cr[2]==b.cr[2],"POP r/m control/flags/CR2");
    check(a.mem==b.mem,"POP r/m memory/page effects");
}
static void paging(CPU&c){c.write(0x80000,0x81007,4);for(unsigned p=0;p<256;++p)c.write(0x81000+p*4,(p<<12)|7,4);
    c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();}
int main(){try{
    for(bool code32:{false,true})for(bool wide:{false,true})for(bool address32:{false,true})
    for(bool stack32:{false,true})for(bool paged:{false,true})for(bool reg:{false,true})
    for(int override:{-1,int(FS),int(GS)})for(unsigned form=0;form<(reg?8u:11u);++form){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=code32;
        c.s[SS].big=stack32;c.s[SS].base=0x10000;c.s[DS].base=0x20000;c.s[FS].base=0x30000;c.s[GS].base=0x40000;
        for(unsigned r=0;r<8;++r)c.r[r]=0x2000+r*0x200;c.r[AX]=0x400ff;c.r[SP]=0x18040;c.flags=0x202;
        unsigned w=wide?4:2;auto post=c.r[SP]+w;
        if(!stack32)post=(c.r[SP]&0xffff0000)|(post&65535);
        unsigned source=c.s[SS].base+(stack32?c.r[SP]:c.r[SP]&65535);
        c.write(source,0xdeadbeef,4);
        std::vector<uint8_t> code={0x04,1};if(wide!=code32)code.push_back(0x66);if(address32!=code32)code.push_back(0x67);
        if(override>=0)code.push_back(override==FS?0x64:0x65);code.push_back(0x8f);
        unsigned offset=0,segment=DS;
        if(reg)code.push_back(0xc0|form);
        else if(form==8){code.push_back(address32?5:6);offset=0x5000;
            for(unsigned n=0;n<(address32?4u:2u);++n)code.push_back(offset>>(n*8));}
        else if(form==9 && address32){code.insert(code.end(),{4,0x8d,0,0x50,0,0});offset=c.r[CX]*4+0x5000;}
        else if(form==10 && address32){code.insert(code.end(),{0x44,0x8c,0x13});offset=post+c.r[CX]*4+0x13;segment=SS;}
        else{
            unsigned rm=form&7;code.push_back(0x40|rm);int disp=0x23;
            if(address32){if(rm==SP){code.push_back(0x24);disp=8;}
                offset=(rm==SP?post:rm==AX?0x40000:c.r[rm])+disp;if(rm==BP||rm==SP)segment=SS;
            }else{const int base[]={BX,BX,BP,BP,SI,DI,BP,BX};const int index[]={SI,DI,SI,DI,-1,-1,-1,-1};
                offset=(c.r[base[rm]]+(index[rm]>=0?c.r[index[rm]]:0)+disp)&65535;if(base[rm]==BP)segment=SS;}
            code.push_back(uint8_t(disp));
        }
        if(override>=0)segment=override;auto target=c.s[segment].base+offset;
        unsigned end=code.size();code.push_back(0xf4);std::copy(code.begin(),code.end(),c.mem.begin());if(paged)paging(c);
        if(paged){c.read(0,1);c.read(source,w);if(!reg){auto value=c.read(target,w);c.write(target,value,w);}}
        auto ref=std::make_unique<CPU>(c);ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(c.ip==end && jit.nativeInstructions==2,"POP r/m lowers with preceding lazy flags");
        if(!reg)check(c.read(target,w)==(wide?0xdeadbeefu:0xbeefu),"POP memory post-SP destination");
    }
    // Exact overlapping operands, including post-ESP arithmetic and 16-bit SP wrap.
    for(bool stack32:{false,true})for(bool word:{false,true})for(int displacement:{-4,-3,-2,0,1,8})for(unsigned sp:{0x8040u,0xfffeu}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.s[SS].big=stack32;
        c.r[SP]=sp;c.flags=0x202;c.write(sp,0x87654321,4);
        std::vector<uint8_t> code={0x04,1};if(word)code.push_back(0x66);code.insert(code.end(),{0x8f,0x44,0x24,uint8_t(displacement),0xf4});
        std::copy(code.begin(),code.end(),c.mem.begin());auto ref=std::make_unique<CPU>(c);ref->step();ref->step();
        Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
    }
    // Guard failures must replay before device reads, dirty-bit effects or
    // code writes. Callbacks observe the reference post-pop SP at the store.
    for(bool paged:{false,true})for(unsigned scenario=0;scenario<11;++scenario){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=c.s[SS].big=true;
        c.r[SP]=0x28040;c.r[AX]=0xff;c.flags=0x202;unsigned target=0x40000;
        if(scenario==1)c.r[SP]=0xa0040;
        if(scenario==2)target=0xa0040;
        if(scenario==3)target=0x30; // Same bitmap word as translated code.
        if(scenario==4)target=0x28041; // Overlap source, no repeated read.
        if(scenario==5)c.s[DS].unusable=true;
        if(scenario==6)c.s[SS].unusable=true;
        if(scenario==7)c.r[SP]=0x28fff;
        if(scenario==8)target=0x40fff;
        if(scenario==10)target=0;
        c.write(c.r[SP],0x12345678,4);
        std::vector<uint8_t> code={0x04,1,0x8f,5};for(unsigned n=0;n<4;++n)code.push_back(target>>(n*8));code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());if(paged){paging(c);if(scenario==9)c.write(0x81100,0,4);c.flushTLB();}
        auto ref=std::make_unique<CPU>(c);unsigned reads[2]={},writes[2]={};std::vector<uint32_t> storeSP[2];bool faults[2]={};struct Stop{};
        for(unsigned k=0;k<2;++k){auto*cpu=k?ref.get():&c;
            cpu->observeFault=[&,k](const Fault&){faults[k]=true;throw Stop{};};
            cpu->videoRead=[&,k](uint32_t){++reads[k];return uint8_t(0x73);};
            cpu->videoWrite=[&,k,cpu](uint32_t,uint8_t){++writes[k];storeSP[k].push_back(cpu->r[SP]);};
            if(scenario==0){cpu->watchStart=0;cpu->watchLength=UINT32_MAX;cpu->watchWrite=[&,k,cpu](uint32_t,uint32_t,int){++writes[k];storeSP[k].push_back(cpu->r[SP]);};}
        }
        try{ref->step();ref->step();}catch(const Stop&){}
        Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}
        equal(c,*ref);check(faults[0]==faults[1] && reads[0]==reads[1] && writes[0]==writes[1] && storeSP[0]==storeSP[1],"POP replay callbacks and SP ordering");
    }
    // Destination dirty-bit updates can alias the value being popped. Read
    // that value first in the interpreter, never as part of a native preflight.
    for(bool word:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=c.s[SS].big=true;
        c.r[AX]=0xff;c.r[SP]=0x81100;c.flags=0x202;
        std::vector<uint8_t> code={0x04,1};if(word)code.push_back(0x66);code.insert(code.end(),{0x8f,5,0,0,4,0,0xf4});
        std::copy(code.begin(),code.end(),c.mem.begin());paging(c);
        c.read(0,1);c.read(0x81100,4);c.read(0x40000,4);
        const auto before=c.read(0x81100,word?2:4);check(!(before&64),"self-PTE source starts clean");
        auto ref=std::make_unique<CPU>(c);ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(c.read(0x40000,word?2:4)==before && (c.read(0x81100,4)&64),"POP source precedes aliased PTE dirty update");
    }
    // Warm and cold helper contracts include restricted user pages, stale CR3,
    // TLB collisions and unchanged state on every declined probe.
    for(unsigned scenario=0;scenario<10;++scenario){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].selector=3;c.s[SS].big=true;
        c.r[SP]=0x28040;c.write(0x28040,0x12345678,4);paging(c);
        NativeMemoryMove op;op.width=4;op.segment=DS;op.store=true;
        c.read(0x28040,4);c.write(0x40000,0,4);
        if(scenario==1)c.flushTLB();
        if(scenario==2)c.tlbCR3^=4096;
        if(scenario==3)c.tlb[(0x40^(0x40>>10))&1023].permissions=4;
        if(scenario==4)c.tlb[(0x40^(0x40>>10))&1023].dirty=false;
        if(scenario==5)c.codeUsed[0x40000>>6]=1;
        if(scenario==6)c.tlb[(0x28^(0x28>>10))&1023].tag^=1024;
        if(scenario==7 || scenario==8)c.tlb[(0x28^(0x28>>10))&1023].permissions=2;
        if(scenario==8)c.systemAccess=1;
        if(scenario==9){c.s[CS].selector=0;c.tlb[(0x40^(0x40>>10))&1023].permissions=4;}
        auto before=c.mem;auto cr2=c.cr[2];auto epoch=c.tlbEpoch;
        auto result=pc::jit_detail::nativePopMemory(&c,&op,0x28040,0x40000);
        const bool allowed=scenario==0 || scenario>=8;
        check(result==(allowed?1u:0u),"POP paired warm guards");
        if(!allowed)check(c.mem==before && c.r[SP]==0x28040 && c.cr[2]==cr2 && c.tlbEpoch==epoch,"declined POP probe has no architectural/page effects");
        else check(c.read(0x40000,4)==0x12345678 && c.r[SP]==0x28040,"POP helper leaves SP commit to native caller");
    }
    // Repeated pairs keep cached SP dirty inside the frame. The helper must
    // use supplied offsets, preserve live flags and leave fetch TLBs untouched.
    for(bool links:{false,true})for(bool paged:{false,true})for(bool stack32:{false,true})for(bool word:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.s[SS].big=stack32;
        c.flags=0x203;c.r[SP]=0x18040;c.r[CX]=200;
        std::vector<uint8_t> code;if(word)code.push_back(0x66);code.push_back(0x50);
        if(word)code.push_back(0x66);code.insert(code.end(),{0x8f,0x44,0x24,8,0x40,0x49,0x75});
        code.push_back(uint8_t(-int(code.size()+1)));code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());if(paged){paging(c);c.write(0x18048,0,4);}
        auto ref=std::make_unique<CPU>(c);Jit jit(c,true,0,65536);jit.setDirectLinks(links,0);unsigned batches=0;
        while(!c.halted && batches++<100){auto batch=jit.runBatch();
            check(batch.instructions>0 && batch.instructions<=127,"POP paired batch bound");
            for(unsigned n=0;n<batch.instructions;++n)ref->step();
            equal(c,*ref);
        }
        check(c.halted && c.r[AX]==200 && c.r[CX]==0 && c.r[SP]==0x18040 && c.read(0x18048,word?2:4)==199,
              "POP paired loop uses cached post-SP address");
        check(jit.nativeInstructions>=990,"POP paired loop remains native");
        if(links)check(jit.directLinkTransitions>0,"POP paired loop links");
    }
    std::cout<<"Passed "<<checks<<" POP r/m JIT checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
