#include "jit.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v,const char*s){++checks;if(!v)throw std::runtime_error(s);}
static void equal(const CPU&a,const CPU&b){
    check(std::equal(a.r,a.r+8,b.r),"far-pointer GPRs");
    check(a.ip==b.ip && a.flags==b.flags && a.instructions==b.instructions && a.cr[2]==b.cr[2] && a.interruptBlockedThrough==b.interruptBlockedThrough && a.systemAccess==b.systemAccess,"far-pointer control");
    for(unsigned n=0;n<6;++n)check(a.s[n].base==b.s[n].base&&a.s[n].limit==b.s[n].limit&&a.s[n].selector==b.s[n].selector&&a.s[n].big==b.s[n].big&&a.s[n].unusable==b.s[n].unusable,"far-pointer segment cache");
    check(a.mem==b.mem,"far-pointer RAM/page effects");
}
static void descriptor(CPU&c,unsigned sel,bool wide){
    const unsigned base=0x30000,table=(sel&4)?0x61000:0x60000;
    c.write(table+(sel&~7u),0xffff|(base<<16),4);
    c.write(table+4+(sel&~7u),(base>>16)|0x008ff200|(wide?0x400000:0),4);
}
static std::vector<uint8_t> program(bool code32,unsigned w,bool a32,unsigned seg,unsigned dst,bool prefix=true){
    std::vector<uint8_t> v;if(prefix)v={0x04,1};
    if((w==4)!=code32)v.push_back(0x66);if(a32!=code32)v.push_back(0x67);
    v.push_back(0x64); // FS override, including a source segment that is itself replaced.
    if(seg==ES||seg==DS)v.push_back(seg==ES?0xc4:0xc5);
    else {v.push_back(0x0f);v.push_back(seg==SS?0xb2:seg==FS?0xb4:0xb5);}
    v.push_back(0x40|(dst<<3)|(a32?4:0));if(a32)v.push_back(0x73);v.push_back(0x13);v.push_back(0xf4);return v;
}
static void paging(CPU&c,bool warm){
    c.write(0x80000,0x81007,4);for(unsigned n=0;n<256;++n)c.write(0x81000+4*n,(n<<12)|(n==0x60||n==0x61?3:7),4);
    c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();
    if(warm){++c.systemAccess;for(unsigned n=0;n<256;++n)c.read(n<<12,1);--c.systemAccess;}
}
int main(){try{
#if defined(__APPLE__) && defined(__aarch64__)
    for(unsigned mode=0;mode<4;++mode)for(bool code32:{false,true})for(unsigned w:{2u,4u})for(bool a32:{false,true})
    for(unsigned seg:{ES,DS,SS,FS,GS})for(unsigned dst=0;dst<8;++dst)for(bool ldt:{false,true})for(unsigned translation=0;translation<3;++translation){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=mode?0x11:0x10;c.flags=0xa03|(mode==3?0x20000:0);c.s[CS].big=code32;c.s[CS].selector=mode==2?3:0;c.s[FS].base=0x10000;
        for(unsigned n=0;n<8;++n)c.r[n]=0xaabb0000+n;c.r[AX]=0x123400ff;c.r[BX]=0x2000;c.r[SI]=0x20;
        c.gdtBase=0x60000;c.gdtLimit=63;c.ldtBase=0x61000;c.ldtLimit=63;
        const unsigned sel=8|(ldt?4:0)|(mode==2?3:0);descriptor(c,sel,!code32);
        const uint32_t address=a32?0x12053:0x12033,value=w==4?0x87654321u:0x4321u;c.write(address,value,w);c.write(address+w,sel,2);
        auto code=program(code32,w,a32,seg,dst);std::copy(code.begin(),code.end(),c.mem.begin());if(translation)paging(c,translation==2);
        auto ref=std::make_unique<CPU>(c);ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(c.get(dst,w)==value,"far-pointer offset loaded");
        if(translation!=1)check(jit.nativeInstructions==2,"warm far pointer stays native");
        check(c.interruptBlockedThrough==0,"LSS must not create MOV/POP SS shadow");
    }
    // A split offset/selector spans two warm pages; a single split data access
    // conservatively replays. Invalid descriptors leave both destinations old.
    for(unsigned seg:{ES,DS,SS,FS,GS})for(unsigned w:{2u,4u})for(unsigned variant=0;variant<8;++variant){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.flags=0xa93;c.r[AX]=0xff;c.r[BX]=0x2000;c.r[SI]=0x20;c.s[FS].base=0x10000;c.gdtBase=0x60000;c.gdtLimit=63;
        const uint16_t sel=variant==3?0:variant==4?0x48:8;descriptor(c,8,true);
        if(variant==5)c.write(0x6000c,c.read(0x6000c,4)&~0x8000u,4);
        if(variant==6)c.write(0x6000c,c.read(0x6000c,4)&~0x1000u,4);
        const auto address=variant==1?0x13000-w:variant==2?0x12fff:0x12053;
        c.r[BX]+=address-0x12053;c.write(address,0x1234,w);c.write(address+w,sel,2);
        auto code=program(true,w,true,seg,SP);if(variant==5)code.insert(code.end()-1,{0x31,0xc9});std::copy(code.begin(),code.end(),c.mem.begin());if(variant==7)paging(c,false);
        struct Stop{};auto ref=std::make_unique<CPU>(c);unsigned faults[2]={};
        for(unsigned n=0;n<2;++n)(n?ref.get():&c)->observeFault=[&,n](const Fault&){++faults[n];throw Stop{};};
        ref->step();try{ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}equal(c,*ref);check(faults[0]==faults[1],"same descriptor faults");
        if(variant==1)check(jit.nativeInstructions==2,"separately warm pointer fields may span pages");
        if(variant==2)check(jit.nativeInstructions==1,"cross-page field replays before access");
    }
    for(unsigned seg:{ES,DS,SS,FS,GS})for(unsigned w:{2u,4u})for(bool denied:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.s[CS].selector=3;c.s[FS].base=0x10000;c.r[SI]=0x20;c.r[BX]=0x3000-w-0x53;c.r[SP]=0xbabe1234;c.r[AX]=0xff;
        const auto address=0x13000-w;c.write(address,0x1234,w);c.write(0x13000,8,2);auto code=program(true,w,true,seg,SP);std::copy(code.begin(),code.end(),c.mem.begin());paging(c,false);
        c.write(0x81000+0x13*4,denied?0x13003:0,4);c.flushTLB();c.read(address,w);
        struct Stop{};auto ref=std::make_unique<CPU>(c);unsigned faults=0;
        for(auto cpu:{&c,ref.get()})cpu->observeFault=[&](const Fault&f){check(f.vector==14,"selector-field page fault");++faults;throw Stop{};};
        ref->step();try{ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}equal(c,*ref);
        check(faults==2&&c.cr[2]==0x13000&&c.r[SP]==0xbabe1234,"selector fault preserves complete destination state");
    }
    // Prior dirty address GPRs must be canonicalized, and the new selector/
    // destination must be visible to following native memory operations.
    {
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.s[FS].base=0x10000;c.s[DS].base=0x20000;c.r[BX]=0xdead;c.r[SI]=0x20;c.r[AX]=0xff;c.gdtBase=0x60000;c.gdtLimit=63;descriptor(c,8,true);
        c.write(0x12053,0x40,4);c.write(0x12057,8,2);c.write(0x30040,0x11223344,4);
        const uint8_t code[]={0xbb,0,0x20,0,0,0x04,1,0x64,0xc5,0x7c,0x73,0x13,0x8b,7,0x42,0xf4};std::copy(std::begin(code),std::end(code),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);for(unsigned n=0;n<5;++n)ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);check(jit.nativeInstructions==5&&c.r[AX]==0x11223344,"new GPR/segment used by following native operation");
    }
    // MMIO callbacks must execute only in reference replay, with the complete
    // prefix state and decoded end IP visible on every pointer-byte read.
    for(unsigned seg:{ES,DS,SS,FS,GS}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.s[CS].big=true;c.s[FS].base=0x10000;c.s[FS].limit=UINT32_MAX;c.r[BX]=0x90000;c.r[SI]=0;c.r[AX]=0xff;
        auto code=program(true,4,true,seg,SP);std::copy(code.begin(),code.end(),c.mem.begin());auto ref=std::make_unique<CPU>(c);
        std::vector<std::array<uint64_t,5>> events[2];const uint8_t pointer[]={0x78,0x56,0x34,0x12,0,1};
        for(unsigned n=0;n<2;++n){auto cpu=n?ref.get():&c;cpu->videoRead=[&,n,cpu](uint32_t a){events[n].push_back({a,cpu->ip,cpu->instructions,cpu->flags,cpu->r[SP]});return pointer[a-0xa0013];};}
        ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(events[0]==events[1]&&events[0].size()==6&&jit.nativeInstructions==1,"MMIO pointer replay is exact");
    }
    for(bool a20:{false,true}){
        auto p=std::make_unique<CPU>(2*1024*1024);auto&c=*p;c.s[CS].big=true;c.a20=a20;c.s[FS].base=0xffff0;c.r[BX]=c.r[SI]=0;
        auto code=program(true,4,true,DS,DI,false);code[code.size()-2]=0x33;std::copy(code.begin(),code.end(),c.mem.begin());c.write(0x100023,0x1234,4);c.write(0x100027,0x100,2);
        auto ref=std::make_unique<CPU>(c);ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);check(jit.nativeInstructions==1,"far-pointer A20 translation");
    }
    {
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.s[SS].base=0x10000;c.s[SS].big=false;c.r[SP]=0x4000;c.r[BX]=0x2000;c.r[AX]=0x11223344;c.gdtBase=0x60000;c.gdtLimit=63;descriptor(c,8,true);
        const uint8_t code[]={0x0f,0xb2,0x23};std::copy(std::begin(code),std::end(code),c.mem.begin());c.mem[0x100]=0x50;c.mem[0x101]=0xf4;c.write(0x2000,0x23400,4);c.write(0x2004,8,2);
        Jit jit(c,true,0,65536);c.ip=0x100;jit.run(); // Cache PUSH with old SS.B.
        c.ip=0;auto ref=std::make_unique<CPU>(c);ref->step();jit.runBatch();equal(c,*ref);
        c.ip=ref->ip=0x100;ref->step();jit.run();equal(c,*ref);check(c.r[SP]==0x233fc&&c.read(0x533fc,4)==0x11223344,"cached stack operation revalidates LSS stack width");
    }
    // Non-SS pointer loads can run in cached and directly linked loops. Their
    // canonical CPU inputs and following cached GPR results must remain exact.
    for(bool links:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.s[CS].big=true;c.s[FS].base=0x10000;c.r[BX]=0x2000;c.r[SI]=0x20;c.write(0x12053,0x40,4);c.write(0x12057,0x100,2);
        const uint8_t code[]={0x64,0xc5,0x7c,0x73,0x13,0x40,0xeb,0xf8};std::copy(std::begin(code),std::end(code),c.mem.begin());auto ref=std::make_unique<CPU>(c);Jit jit(c,true,0,65536);jit.setDirectLinks(links,0);
        for(unsigned n=0;n<100;++n){auto batch=jit.runBatch(31);while(ref->instructions<c.instructions)ref->step();equal(c,*ref);check(batch.instructions>3,"far-pointer cached batch executes multiple blocks");}
        if(links)check(jit.directLinkTransitions>0,"far-pointer loop exercises promoted direct links");
    }
    // LSS returns at its boundary without adding an interrupt shadow.
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})for(bool links:{false,true}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.s[CS].big=true;c.r[BX]=0x1000;c.write(0x1000,0x2000,4);c.write(0x1004,0x100,2);
        const uint8_t code[]={0x0f,0xb2,3,0x41,0xf4};std::copy(std::begin(code),std::end(code),c.mem.begin());Jit jit(c,true,0,65536,mode);jit.setDirectLinks(links,0);
        for(unsigned n=0;n<50;++n){c.ip=0;c.r[CX]=0;c.interruptBlockedThrough=0;auto ref=std::make_unique<CPU>(c);ref->step();auto batch=jit.runBatch();equal(c,*ref);check(batch.instructions==1&&batch.dispatches==1&&c.r[CX]==0,"LSS polls immediately before successor");}
    }
#endif
    std::cout<<"Passed "<<checks<<" far-pointer JIT checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
