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
    check(std::equal(a.r,a.r+8,b.r),"#UD GPRs");
    check(a.ip==b.ip&&a.flags==b.flags&&a.instructions==b.instructions&&a.cr[2]==b.cr[2]&&a.interruptBlockedThrough==b.interruptBlockedThrough&&a.systemAccess==b.systemAccess&&a.halted==b.halted,"#UD control");
    for(unsigned n=0;n<6;++n)check(a.s[n].base==b.s[n].base&&a.s[n].limit==b.s[n].limit&&a.s[n].selector==b.s[n].selector&&a.s[n].big==b.s[n].big&&a.s[n].unusable==b.s[n].unusable,"#UD segments");
    check(a.mem==b.mem,"#UD RAM/page effects");
}
int main(){try{
#if defined(__APPLE__) && defined(__aarch64__)
    // Mode-invalid ARPL faults before ModRM/operand fetch. Every prefix count,
    // source operand byte and code default preserves the exact restart image.
    for(bool vm:{false,true})for(bool code32:{false,true})for(bool prefixALU:{false,true})
    for(unsigned prefixes=0;prefixes<15;++prefixes)for(unsigned operand:{0u,0xc8u,0xffu}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.cr[0]=vm?0x11:0x10;c.s[CS].big=code32;c.flags=0xad7|(vm?0x20000:0);c.instructions=0x100000005ull;
        for(unsigned r=0;r<8;++r)c.r[r]=0xfeed0000+r;c.r[AX]=0x123456ff;
        std::vector<uint8_t> v;if(prefixALU)v={0x04,1};for(unsigned n=0;n<prefixes;++n)v.push_back(n%3==0?0x66:n%3==1?0x67:0x64);v.insert(v.end(),{0x63,uint8_t(operand),0xf4});std::copy(v.begin(),v.end(),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);struct Stop{};std::array<unsigned,2> faults{};
        for(unsigned n=0;n<2;++n){auto cpu=n?ref.get():&c;cpu->observeFault=[&,n](const Fault&f){check(f.vector==6&&!f.hasError,"#UD vector/error");++faults[n];throw Stop{};};}
        try{if(prefixALU)ref->step();ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}equal(c,*ref);
        check(faults[0]==1&&faults[1]==1&&jit.nativeInstructions==(prefixALU?2u:1u),"compiled #UD prefix accounting");
    }
    // A missing ModRM page must not override #UD. A missing opcode page must.
    for(unsigned mode=0;mode<4;++mode){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.cr[3]=0x80000;c.flags=0x20ad7;c.s[CS].big=true;
        c.write(0x80000,0x81007,4);for(unsigned n=0;n<256;++n)c.write(0x81000+n*4,(n<<12)|7,4);if(mode!=3)c.write(0x81004,0,4);c.cr[0]|=0x80000000;c.flushTLB();
        c.ip=mode==1?4094:4095;if(mode)c.mem[c.ip]=0x66;c.mem[mode?c.ip+1:c.ip]=0x63;
        if(mode==3)c.read(4096,1);
        auto ref=std::make_unique<CPU>(c);struct Stop{};unsigned faults=0;
        for(auto cpu:{&c,ref.get()})cpu->observeFault=[&](const Fault&f){check(f.vector==(mode==2?14:6),"#UD versus opcode fetch fault priority");++faults;throw Stop{};};
        try{ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}equal(c,*ref);check(faults==2&&jit.nativeInstructions==(mode==2?0u:1u),"opcode page accounting");
    }
    {
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.s[CS].base=0xa0000;c.ip=0x10;c.mem[0xa0010]=0x63;
        auto ref=std::make_unique<CPU>(c);unsigned reads[2]={};struct Stop{};
        for(unsigned n=0;n<2;++n){auto cpu=n?ref.get():&c;cpu->videoRead=[&,cpu,n](uint32_t a){++reads[n];return cpu->mem[a];};cpu->observeFault=[](const Fault&){throw Stop{};};}
        try{ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}equal(c,*ref);
        check(reads[0]==1&&reads[1]==1&&jit.nativeInstructions==0,"device opcode fetch replays its callback");
    }
    // Complete real-mode frame and V86-to-ring-0 frame delivery, with no error
    // code for #UD. Subsequent guest execution resumes at the installed handler.
    for(bool vm:{false,true})for(bool batch:{false,true}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.flags=0x202;c.r[SP]=0x8000;
        if(vm){
            c.gdtBase=0x6000;c.gdtLimit=31;c.write(0x6008,0xffff,4);c.write(0x600c,0x00cf9a00,4);c.write(0x6010,0xffff,4);c.write(0x6014,0x00cf9200,4);
            c.cr[0]|=1;c.trSelector=24;c.trBase=0x3000;c.trLimit=0x100;c.tr32=true;c.write(0x3004,0x9000,4);c.write(0x3008,16,2);
            c.idtBase=0x2000;c.idtLimit=2047;c.write(0x2000+6*8,0x00080500,4);c.write(0x2004+6*8,0x8e00,4);c.flags|=0x20000;
            for(unsigned n=0;n<6;++n)c.segment(n,(n==CS?1:n==SS?2:n==ES?3:n==DS?4:n==FS?5:6)*0x100);
            c.r[SP]=0x12340800;
        }else c.write(6*4,0x500,4);
        c.ip=0x10;c.mem[c.s[CS].base+0x10]=0x63;auto ref=std::make_unique<CPU>(c);ref->step();Jit jit(c,true,0,65536);
        if(batch){auto result=jit.runBatch(64);check(result.instructions==1&&result.dispatches==1,"#UD batch stops before handler execution");}else jit.run();equal(c,*ref);
        check(c.ip==0x500&&c.r[SP]==(vm?0x8fdc:0x7ffa),"#UD complete frame size");
        if(vm){const uint32_t frame[]={0x10,0x100,0x20202,0x12340800,0x200,0x300,0x400,0x500,0x600};for(unsigned n=0;n<9;++n)check(c.read(0x8fdc+n*4,4)==frame[n],"#UD exact V86 frame");}
    }
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})for(bool links:{false,true})for(bool clear:{false,true})for(bool vm:{false,true}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.cr[0]=vm?0x11:0x10;c.flags=vm?0x20002:2;c.mem[0]=0x63;c.mem[1]=0xc8;Jit jit(c,true,clear?0:8,65536,mode);jit.setDirectLinks(links,0);struct Stop{};
        unsigned clearedNative=0;
        for(unsigned n=0;n<30;++n){c.ip=0;auto ref=std::make_unique<CPU>(c);ref->observeFault=[](const Fault&){throw Stop{};};c.observeFault=[&](const Fault&){if(clear){clearedNative+=jit.nativeInstructions;jit.clear();}throw Stop{};};try{ref->step();}catch(const Stop&){}try{jit.run();}catch(const Stop&){}equal(c,*ref);}
        check(clearedNative+jit.nativeInstructions>=20,"#UD cached warmup and callback cache invalidation");
        // Reuse a compiled invalid-mode address in valid protected mode.
        c.observeFault={};c.cr[0]=0x11;c.flags=2;c.ip=0;c.r[AX]=0;c.r[CX]=3;auto ref=std::make_unique<CPU>(c);ref->step();jit.run();equal(c,*ref);check(c.r[AX]==3&&(c.flags&ZF),"#UD mode guard admits ordinary ARPL semantics");
    }
    for(bool shadow:{false,true}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.cr[0]=0x11;c.flags=0x20002|(shadow?0:TF);c.interruptBlockedThrough=shadow?1:0;c.mem[0]=0x63;
        auto ref=std::make_unique<CPU>(c);struct Stop{};for(auto cpu:{&c,ref.get()})cpu->observeFault=[](const Fault&){throw Stop{};};
        try{ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}equal(c,*ref);check(jit.nativeInstructions==0,"#UD respects TF and interrupt-shadow forced stepping");
    }
    {
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.mem[0]=0x63;c.mem[1]=0xc8;c.mem[2]=0xf4;struct Stop{};c.observeFault=[](const Fault&){throw Stop{};};Jit jit(c,true,0,65536);
        try{jit.run();}catch(const Stop&){}c.ip=0;c.r[AX]=3;c.write(0,0x40,1);c.write(1,0x90,1);auto ref=std::make_unique<CPU>(c);ref->step();ref->step();jit.run();equal(c,*ref);check(c.r[AX]==4,"#UD boundary code invalidation");
    }
#endif
    std::cout<<"PASS "<<checks<<" compiled #UD checks\n";
}catch(const Fault&f){std::cerr<<"FAIL uncaught fault "<<unsigned(f.vector)<<" after "<<checks<<" checks\n";return 1;}
catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<" after "<<checks<<" checks\n";return 1;}}
