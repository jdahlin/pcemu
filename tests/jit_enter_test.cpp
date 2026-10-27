#include "jit.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v,const char*s){++checks;if(!v)throw std::runtime_error(s);}
static void equal(const CPU&a,const CPU&b){
    check(std::equal(a.r,a.r+8,b.r),"ENTER GPRs");
    check(a.ip==b.ip && a.flags==b.flags && a.instructions==b.instructions && a.cr[2]==b.cr[2] && a.interruptBlockedThrough==b.interruptBlockedThrough && a.systemAccess==b.systemAccess,"ENTER control");
    check(a.mem==b.mem,"ENTER RAM/page effects");
}
static std::vector<uint8_t> code(bool code32,unsigned w,unsigned size,unsigned level,bool addressPrefix=false){
    std::vector<uint8_t> v={0x04,1};if((w==4)!=code32)v.push_back(0x66);if(addressPrefix)v.push_back(0x67);
    v.insert(v.end(),{0x64,0xc8,uint8_t(size),uint8_t(size>>8),uint8_t(level),0xf4});return v;
}
static void initialize(CPU&c,bool code32,unsigned w,bool stack32,unsigned mode=0){
    c.cr[0]=mode?0x11:0x10;c.s[CS].big=code32;c.s[CS].selector=mode==2?3:0;c.flags=0xa93|(mode==3?0x20000:0);
    c.s[SS].base=0x20000;c.s[SS].big=stack32;c.s[SS].limit=stack32?UINT32_MAX:65535;c.s[FS].base=0x50000;c.r[AX]=0xff;
    c.r[BP]=w==2?0xabcd4000:0x4000;c.r[SP]=stack32?0x18000:0xface8000;
    for(unsigned n=0;n<0x10000;++n)c.mem[0x20000+n]=uint8_t(n*73+(n>>8));
}
static void paging(CPU&c,unsigned warmth){
    c.write(0x80000,0x81007,4);for(unsigned n=0;n<256;++n)c.write(0x81000+4*n,(n<<12)|7,4);
    c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();
    if(warmth){for(unsigned n=0;n<256;++n){const auto a=n<<12;if(warmth==2)c.write(a,c.read(a,1),1);else c.read(a,1);}}
}
int main(){try{
#if defined(__APPLE__) && defined(__aarch64__)
    for(unsigned mode=0;mode<4;++mode)for(bool code32:{false,true})for(unsigned w:{2u,4u})for(bool stack32:{false,true})
    for(unsigned size:{0u,1u,0x1234u,65535u})for(unsigned level:{0u,1u,2u,3u,15u,31u,32u,33u,255u})for(unsigned translation=0;translation<4;++translation){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;initialize(c,code32,w,stack32,mode);auto v=code(code32,w,size,level,true);std::copy(v.begin(),v.end(),c.mem.begin());if(translation)paging(c,translation-1);
        auto ref=std::make_unique<CPU>(c);ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        if(translation==0 || translation==3)check(jit.nativeInstructions==2,"warm ENTER remains native");
    }
    // Every raw nesting byte is masked to five bits, including partially
    // overlapping source frames observing earlier pushes in program order.
    for(unsigned w:{2u,4u})for(bool stack32:{false,true})for(unsigned raw=0;raw<256;++raw)for(int delta:{-1,0,1,2,3}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;initialize(c,true,w,stack32);c.r[SP]=stack32?0x8108:0xface8108;c.r[BP]=(w==2?0xabcd0000u:0)|(0x8108+delta);
        auto v=code(true,w,17,raw);std::copy(v.begin(),v.end(),c.mem.begin());auto ref=std::make_unique<CPU>(c);ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);check(jit.nativeInstructions==2,"overlapping frame chains stay native");
    }
    for(unsigned w:{2u,4u})for(bool crossingPush:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;initialize(c,true,w,true);c.r[SP]=crossingPush?0x8001:0x8000;c.r[BP]=crossingPush?0x4000:0x4001;
        auto v=code(true,w,7,4);std::copy(v.begin(),v.end(),c.mem.begin());auto ref=std::make_unique<CPU>(c);ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);check(jit.nativeInstructions==1,"cross-page field replays before any frame write");
    }
    // Different virtual pages alias the same frame bytes.
    for(unsigned w:{2u,4u})for(bool stack32:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;initialize(c,true,w,stack32);c.s[SS].base=0x10000;c.r[SP]=stack32?0x8008:0xface8008;c.r[BP]=(w==2?0xabcd0000u:0)|0x7008;
        auto v=code(true,w,8,31);std::copy(v.begin(),v.end(),c.mem.begin());paging(c,2);c.write(0x81000+0x17*4,0x40067,4);c.write(0x81000+0x18*4,0x40067,4);c.flushTLB();
        for(unsigned n:{0x16u,0x17u,0x18u}){auto a=n<<12;c.write(a,c.read(a,1),1);}
        auto ref=std::make_unique<CPU>(c);ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);check(jit.nativeInstructions==2,"physical frame aliases stay native");
    }
    // 32-bit frame-chain addresses retain their high virtual bits even with
    // a 16-bit stack pointer; only push/allocation addressing follows SS.B.
    for(bool stack32:{false,true})for(unsigned level:{0u,31u}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;initialize(c,true,4,stack32,1);c.s[SS].base=0;c.r[SP]=stack32?0xc0028008:0xface8008;c.r[BP]=0xc0024008;
        auto v=code(true,4,0x1234,level);std::copy(v.begin(),v.end(),c.mem.begin());paging(c,0);
        c.write(0x80000+768*4,0x82007,4);for(unsigned n=0;n<256;++n)c.write(0x82000+4*n,(n<<12)|7,4);c.flushTLB();
        for(uint32_t a:{0x7000u,0x8000u,0xc0023000u,0xc0024000u,0xc0027000u,0xc0028000u})c.write(a,c.read(a,1),1);
        auto ref=std::make_unique<CPU>(c);ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);check(jit.nativeInstructions==2,"ENTER high virtual frame chains stay native");
    }
    // A later nested read or push faults after earlier frame writes. Replay
    // must preserve those bytes while restoring architectural BP/SP and flags.
    for(bool readFault:{false,true})for(bool denied:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;initialize(c,true,4,true,2);c.r[SP]=0x8008;c.r[BP]=readFault?0x4004:0x4008;
        c.write(0x24004,0x11223344,4);c.write(0x24000,0x55667788,4);auto v=code(true,4,8,3);v.insert(v.end()-1,{0x31,0xc9});std::copy(v.begin(),v.end(),c.mem.begin());paging(c,2);
        const unsigned page=readFault?0x24:0x27;c.write(0x81000+page*4,denied?(page<<12)|3:0,4);c.flushTLB();
        for(unsigned n:{0x28u,0x24u})if(n!=page){auto a=n<<12;c.write(a,c.read(a,1),1);}
        struct Stop{};unsigned faults=0;auto ref=std::make_unique<CPU>(c);
        for(auto cpu:{&c,ref.get()})cpu->observeFault=[&](const Fault&f){check(f.vector==14,"ENTER page fault");++faults;throw Stop{};};
        ref->step();try{ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}equal(c,*ref);
        check(faults==2&&c.r[SP]==0x8008&&c.r[BP]==(readFault?0x4004:0x4008),"ENTER fault restores BP/SP");
        check(c.read(0x28004,4)==(readFault?0x4004:0x4008),"earlier saved BP remains after fault");
        if(!readFault)check(c.read(0x28000,4)==0x11223344,"earlier nested push remains after fault");
    }
    // Watched writes retain their exact ordered states; marked-code writes
    // replay once with precise generation updates instead of stale continuation.
    for(bool watched:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;initialize(c,true,4,false);c.r[BP]=0x8000;auto v=code(true,4,7,4);std::copy(v.begin(),v.end(),c.mem.begin());
        if(!watched)for(unsigned n=0;n<128;++n)c.markCode(0x27f80+n);
        auto ref=std::make_unique<CPU>(c);std::vector<std::array<uint64_t,7>> events[2];
        if(watched)for(unsigned n=0;n<2;++n){auto cpu=n?ref.get():&c;cpu->watchStart=0x20000;cpu->watchLength=0x10000;cpu->watchWrite=[&,cpu,n](uint32_t a,uint32_t value,int width){events[n].push_back({a,value,uint64_t(width),cpu->r[BP],cpu->r[SP],cpu->flags,cpu->instructions});};}
        ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);check(jit.nativeInstructions==1,"watched/code ENTER replays before writes");
        check(events[0]==events[1]&&c.generation==ref->generation,"ENTER exact callbacks/code generations");
    }
    {
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;initialize(c,true,4,true);c.s[SS].base=0xa0000;c.r[SP]=c.r[BP]=0x108;auto v=code(true,4,9,3);std::copy(v.begin(),v.end(),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);std::vector<std::array<uint64_t,8>> events[2];
        for(unsigned n=0;n<2;++n){auto cpu=n?ref.get():&c;
            cpu->videoRead=[&,cpu,n](uint32_t a){auto value=cpu->mem[a];events[n].push_back({0,a,value,cpu->r[SP],cpu->r[BP],cpu->flags,cpu->instructions,cpu->ip});return value;};
            cpu->videoWrite=[&,cpu,n](uint32_t a,uint8_t value){events[n].push_back({1,a,value,cpu->r[SP],cpu->r[BP],cpu->flags,cpu->instructions,cpu->ip});cpu->mem[a]=value;};
        }
        ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);check(jit.nativeInstructions==1 && events[0]==events[1] && events[0].size()==24,"ENTER MMIO ordering and live state remain exact");
    }
    {
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;initialize(c,true,4,true);
        const uint8_t v[]={0x04,1,0xbc,0,0x80,1,0,0xbd,0,0x40,0,0,0xc8,4,0,2,0x36,0x8b,0x45,0,0xc9,0x42,0xf4};std::copy(std::begin(v),std::end(v),c.mem.begin());Jit jit(c,true,0,65536);
        for(unsigned n=0;n<6;++n){c.s[SS].big=bool(n&1);c.ip=0;c.r[AX]=0xff;auto ref=std::make_unique<CPU>(c);for(unsigned i=0;i<7;++i)ref->step();jit.run();equal(c,*ref);check(c.r[AX]==0x4000 && c.r[SP]==0x18000 && c.r[BP]==0x4000,"dirty inputs and ENTER/LEAVE continuation use current stack width");}
        check(jit.nativeInstructions==42,"stack-width changes retain native ENTER integration");
    }
    {
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;initialize(c,true,4,true);const uint8_t v[]={0xc8,3,0,0,0x40,0xf4};std::copy(std::begin(v),std::end(v),c.mem.begin());Jit jit(c,true,8,65536);
        for(unsigned n=0;n<100;++n){c.ip=0;c.r[SP]=0x18000;c.r[BP]=0x4000;auto ref=std::make_unique<CPU>(c);jit.run();while(ref->instructions<c.instructions)ref->step();equal(c,*ref);}
        check(jit.nativeInstructions==186,"ENTER at start of a short fragment uses normal warmup");
    }
    for(bool links:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;initialize(c,true,4,true);const uint8_t v[]={0xc8,7,0,4,0xc9,0x40,0xeb,0xf8};std::copy(std::begin(v),std::end(v),c.mem.begin());auto ref=std::make_unique<CPU>(c);Jit jit(c,true,0,65536);jit.setDirectLinks(links,0);
        for(unsigned n=0;n<100;++n){auto batch=jit.runBatch(31);while(ref->instructions<c.instructions)ref->step();equal(c,*ref);check(batch.instructions>4,"ENTER cached batch covers multiple blocks");}
        if(links)check(jit.directLinkTransitions>0,"ENTER executes promoted direct links");
    }
    for(bool a20:{false,true}){
        auto p=std::make_unique<CPU>(2*1024*1024);auto&c=*p;initialize(c,true,4,true);c.a20=a20;c.s[SS].base=0xffff0;c.r[SP]=0x2000;c.r[BP]=0x1000;
        auto v=code(true,4,7,4);std::copy(v.begin(),v.end(),c.mem.begin());auto ref=std::make_unique<CPU>(c);ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);check(jit.nativeInstructions==2,"ENTER A20 translation");
    }
#endif
    std::cout<<"Passed "<<checks<<" ENTER JIT checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
