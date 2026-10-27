#include "jit.hpp"
#include "jit_runtime.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v,const char *s){++checks;if(!v)throw std::runtime_error(s);}
static void equal(const CPU&a,const CPU&b){
    check(std::equal(a.r,a.r+8,b.r),"cross-page GPRs");
    check(a.ip==b.ip && a.flags==b.flags && a.instructions==b.instructions && a.cr[2]==b.cr[2],"cross-page control");
    check(a.mem==b.mem,"cross-page memory and paging effects");
}
static void paging(CPU&c){
    c.cr[0]=0x11;c.cr[3]=0x70000;
    c.write(0x70000,0x71007,4);
    for(unsigned n=0;n<256;++n)c.write(0x71000+4*n,(n<<12)|7,4);
    c.write(0x71000,0x4007,4);c.write(0x71004,0x8007,4);c.cr[0]|=0x80000000;c.flushTLB();
}
static void bytes(CPU&c,uint32_t ip,const std::vector<uint8_t>&code){
    for(auto b:code){c.write(c.s[CS].base+ip,b,1);ip=(ip+1)&(c.s[CS].big?UINT32_MAX:65535u);}
}
int main(){try{
#if defined(__APPLE__) && defined(__aarch64__)
    for(bool code32:{false,true})for(bool address32:{false,true})for(unsigned width:{2u,4u})
    for(bool paged:{false,true})for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})for(bool links:{false,true}){
        std::vector<uint8_t> code;
        if((width==4)!=code32)code.push_back(0x66);
        if(address32!=code32)code.push_back(0x67);
        code.push_back(0x39);code.push_back(address32?0x44:0x40);if(address32)code.push_back(0x24);code.push_back(0x24);
        for(unsigned split=1;split<code.size();++split){
            auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.s[CS].big=code32;c.s[SS].base=c.s[DS].base=0x10000;for(auto &segment:c.s)segment.limit=UINT32_MAX;
            c.r[SP]=0x10000;c.r[BX]=0x1000;c.r[SI]=0x20;c.r[AX]=0x87654321;c.flags=0xa93;
            if(paged)paging(c);
            const auto start=4096-split;c.ip=start;bytes(c,start,code);bytes(c,start+code.size(),{0x43,0xf4});
            c.write(address32?0x20024:0x11044,0x87654320,width);
            auto ref=std::make_unique<CPU>(c);ref->step();Jit jit(c,true,0,65536,mode);jit.setDirectLinks(links,0);auto batch=jit.runBatch();
            equal(c,*ref);
            check(jit.nativeInstructions==1 && batch.instructions==1 && batch.dispatches==1,"cross-page instruction stays native and returns before successor");
            for(unsigned n=0;n<3;++n){c.ip=start;ref=std::make_unique<CPU>(c);ref->step();jit.runBatch();equal(c,*ref);}
        }
    }
    {
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.s[CS].big=true;for(auto &segment:c.s)segment.limit=UINT32_MAX;c.r[SP]=0x2000;c.ip=0xffe;
        bytes(c,0xffe,{0x39,0x44,0x24,0x24});Jit jit(c,true,8,65536);
        for(unsigned n=0;n<100;++n){c.ip=0xffe;auto ref=std::make_unique<CPU>(c);ref->step();jit.runBatch();equal(c,*ref);}
        check(jit.nativeInstructions==93,"cross-page CMP uses normal warmup rather than short-fragment delay");
    }
    // The measured CMP [ESP+24h],EAX: change either physical code page, then
    // remap only the second page without changing the first page's generation.
    for(unsigned mutation=0;mutation<7;++mutation){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.s[CS].big=true;for(auto &segment:c.s)segment.limit=UINT32_MAX;c.s[SS].big=true;c.ip=0xffe;c.r[SP]=0x20000;c.r[AX]=5;c.flags=0x202;if(mutation==4)c.s[CS].selector=3;paging(c);
        bytes(c,0xffe,{0x39,0x44,0x24,0x24,0x43,0xf4});c.write(0x20024,5,4);c.write(0x20028,6,4);
        Jit jit(c,true,0,65536);jit.run();check(jit.nativeInstructions==1,"initial measured CMP native");c.ip=0xffe;
        if(mutation==0)c.write(0xffe,0x3b,1);
        if(mutation==1)c.write(0x1001,0x28,1);
        if(mutation>=2 && mutation<=4){
            c.mem[0x9000]=0x24;c.mem[0x9001]=0x28;c.mem[0x9002]=0x43;c.mem[0x9003]=0xf4;
            c.write(0x71004,mutation==3?0:mutation==4?0x9003:0x9007,4);c.flushTLB();
            // For CPL3, the existing tail mapping becomes supervisor-only.
        }
        if(mutation==5){c.mem[0xaffe]=0x39;c.mem[0xafff]=0x44;c.write(0x71000,0xa007,4);c.flushTLB();}
        if(mutation==6){c.write(0x71000,0,4);c.flushTLB();}
        const bool faulting=mutation==3 || mutation==4 || mutation==6;
        struct Stop{};unsigned faults[2]={};auto ref=std::make_unique<CPU>(c);
        if(faulting)for(unsigned n=0;n<2;++n)(n?ref.get():&c)->observeFault=[&,n](const Fault&f){check(f.vector==14,"tail fetch page fault");++faults[n];throw Stop{};};
        try{ref->step();}catch(const Stop&){}try{jit.run();}catch(const Stop&){}equal(c,*ref);
        if(faulting)check(faults[0]==1 && faults[1]==1 && c.ip==0xffe && c.cr[2]==(mutation==6?0xffe:0x1000),"tail fault restarts at original IP");
        else {
            if(mutation==2 || mutation==5){ // Cold tail executes once in reference order, then remapped code is recompiled.
                c.ip=0xffe;ref=std::make_unique<CPU>(c);ref->step();jit.run();equal(c,*ref);
            }
            check(jit.nativeInstructions==2,"changed/remapped CMP recompiled natively");
        }
    }
    // C7 fetches the encoded immediate before checking its store operand.
    // A missing immediate page therefore wins over an unusable data segment.
    {
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.s[CS].big=true;for(auto &segment:c.s)segment.limit=UINT32_MAX;c.ip=0xffe;c.r[BX]=0x20000;paging(c);
        bytes(c,0xffe,{0xc7,3,1,0,0,0});Jit jit(c,true,0,65536);jit.run();
        c.ip=0xffe;c.s[DS].unusable=true;c.write(0x71004,0,4);c.flushTLB();c.cr[2]=0x123456;
        struct Stop{};auto ref=std::make_unique<CPU>(c);unsigned faults=0;
        for(auto cpu:{&c,ref.get()})cpu->observeFault=[&](const Fault&f){check(f.vector==14,"immediate fetch fault precedes store operand");++faults;throw Stop{};};
        try{ref->step();}catch(const Stop&){}try{jit.run();}catch(const Stop&){}equal(c,*ref);check(faults==2&&c.cr[2]==0x1000,"missing immediate reports its fetch address");
    }
    // With a present immediate page, its accessed bit is set before the
    // eventual unusable-segment fault. Valid cold tails compile immediately.
    for(bool unusable:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.s[CS].big=true;for(auto &segment:c.s)segment.limit=UINT32_MAX;c.ip=0xffe;c.r[BX]=0x20000;paging(c);
        bytes(c,0xffe,{0xc7,3,1,0,0,0});c.write(0x71004,0x8007,4);c.flushTLB();c.s[DS].unusable=unusable;c.cr[2]=0x123456;
        struct Stop{};auto ref=std::make_unique<CPU>(c);unsigned faults=0;
        if(unusable)for(auto cpu:{&c,ref.get()})cpu->observeFault=[&](const Fault&f){check(f.vector==13,"store segment fault follows complete instruction fetch");++faults;throw Stop{};};
        try{ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}equal(c,*ref);
        if(unusable)check(faults==2 && c.cr[2]==0x123456 && (c.read(0x71004,4)&32),"complete immediate fetch precedes segment validation");
        else {check(jit.nativeInstructions==1,"safe cold immediate tail compiles immediately");c.ip=0xffe;ref=std::make_unique<CPU>(c);ref->step();jit.run();equal(c,*ref);check(jit.nativeInstructions==2,"safe cold decode remains native on retry");}
    }
    // Side-effect-free group-1 prefetch proof rejects operands aliasing the
    // later fetch's page tables, faults and MMIO. Cold ordinary RAM is allowed.
    // Intel SDM Vol. 3A, 4.8/4.10: page-walk accessed-bit writes are observable.
    // https://cdrdv2-public.intel.com/835754/253668-sdm-vol-3a.pdf
    for(unsigned scenario=0;scenario<4;++scenario){
        CPU c(1024*1024);c.s[CS].big=true;paging(c);
        NativeMemoryMove op;op.address32=true;op.width=4;
        op.displacement=scenario==0?0x20000:scenario==1?0x71004:scenario==2?0xa0000:0x70000;
        auto memory=c.mem;auto cr2=c.cr[2];auto epoch=c.tlbEpoch;
        check(jit_detail::canPrefetchImmediateTail(c,op,0x1000)==(scenario==0),"immediate prefetch proves nonalias RAM access");
        check(c.mem==memory && c.cr[2]==cr2 && c.tlbEpoch==epoch,"prefetch proof has no paging side effects");
    }
    for(bool alias:{false,true}){
        CPU c(1024*1024);c.s[CS].big=true;c.ip=0xffe;c.r[BX]=alias?0x71004:0x20000;paging(c);
        bytes(c,0xffe,{0x83,3,7});c.write(0x71004,c.read(0x71004,4)&~32u,4);c.flushTLB();
        auto ref=std::make_unique<CPU>(c);ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(jit.nativeInstructions==(alias?0u:1u),"group-1 code warming preserves operand-before-immediate value");
    }
    // An unrelated unusable segment must not force x87 displacement fetches
    // into the interpreter. EM/TS still wins before a missing ModR/M page.
    for(bool ts:{false,true}){
        CPU c(1024*1024);c.s[CS].big=true;c.s[GS].unusable=true;c.ip=0xfff;paging(c);
        bytes(c,0xfff,{0xd9,5,0,0,2,0});c.write(0x20000,0x3fc00000,4);c.flushTLB();
        if(ts){c.cr[0]|=8;c.write(0x71004,0,4);c.flushTLB();}
        auto ref=std::make_unique<CPU>(c);struct Stop{};unsigned faults=0;
        if(ts)for(auto cpu:{&c,ref.get()})cpu->observeFault=[&](const Fault&f){check(f.vector==7,"NM precedes ModRM fetch");++faults;throw Stop{};};
        try{ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}equal(c,*ref);
        if(ts)check(faults==2 && jit.nativeInstructions==0,"x87 cold tail retains NM priority");
        else check(jit.nativeInstructions==1 && std::equal(c.fp,c.fp+8,ref->fp),"x87 cold tail compiles without unrelated segment assumptions");
    }
    // Tail bytes provided by video callbacks must never be decoded from RAM.
    for(bool paged:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.s[CS].big=true;for(auto &segment:c.s)segment.limit=UINT32_MAX;c.flags=0x202;
        if(paged){paging(c);c.write(0x71004,0xa0007,4);c.flushTLB();c.ip=0xfff;}
        else c.ip=0x9ffff;
        c.write(c.ip,0xb8,1);auto ref=std::make_unique<CPU>(c);unsigned reads[2]={};
        for(unsigned n=0;n<2;++n)(n?ref.get():&c)->videoRead=[&,n](uint32_t a){++reads[n];return uint8_t(0x12345678u>>((a&3)*8));};
        ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(c.r[AX]==0x12345678 && reads[0]==4 && reads[1]==4 && jit.nativeInstructions==0,"callback-backed tail uses exact reference fetches");
    }
    {
        auto p=std::make_unique<CPU>(4096);auto&c=*p;c.s[CS].big=true;for(auto &segment:c.s)segment.limit=UINT32_MAX;c.ip=0xfff;c.mem[0xfff]=0xb8;
        auto ref=std::make_unique<CPU>(c);ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(c.r[AX]==UINT32_MAX && jit.nativeInstructions==0,"tail beyond RAM returns reference FF bytes");
    }
    // The full A0000..BFFFF video window ends at C0000, where ROM/RAM fetches resume.
    {
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.s[CS].big=true;for(auto &segment:c.s)segment.limit=UINT32_MAX;c.ip=0xbffff;
        c.mem[0xbffff]=0xb8;c.write(0xc0000,0x12345678,4);auto ref=std::make_unique<CPU>(c);unsigned reads[2]={};
        for(unsigned n=0;n<2;++n)(n?ref.get():&c)->videoRead=[&,n](uint32_t){++reads[n];return uint8_t(0xb8);};
        ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(c.r[AX]==0x12345678 && reads[0]==1 && reads[1]==1 && jit.nativeInstructions==0,"callback-backed head uses exact reference fetches");
    }
    // Crossing AFFFF/B0000 or B7FFF/B8000 remains entirely device-backed.
    for (uint32_t start : {0xaffffu,0xb7fffu}) {
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.s[CS].big=true;for(auto &segment:c.s)segment.limit=UINT32_MAX;c.ip=start;
        auto ref=std::make_unique<CPU>(c);unsigned reads[2]={};
        for(unsigned n=0;n<2;++n)(n?ref.get():&c)->videoRead=[&,n](uint32_t a){
            ++reads[n];return a==start?uint8_t(0xb8):uint8_t(0x12345678u>>((a-start-1)*8));
        };
        ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(c.r[AX]==0x12345678 && reads[0]==5 && reads[1]==5 && jit.nativeInstructions==0,
              "CGA video-window boundaries preserve all fetch callbacks");
    }
    // A completed 16-bit instruction may wrap IP, but its own encoded bytes
    // cannot straddle FFFFh. Intel 80386 PRM 14.7 requires #GP, not a wrapped
    // immediate fetch; retain precise fault delivery when translation declines.
    for(bool a20:{false,true}){
        auto p=std::make_unique<CPU>(2*1024*1024);auto&c=*p;c.ip=0xffff;c.a20=a20;c.s[CS].base=0xffff0;c.flags=0x202;
        bytes(c,0xffff,{0xb8,0x34,0x12,0x43,0xf4});auto ref=std::make_unique<CPU>(c);
        struct Stop{};unsigned faults=0;
        for(auto cpu:{&c,ref.get()})cpu->observeFault=[&](const Fault&f){check(f.vector==13,"encoded IP wrap raises GP");++faults;throw Stop{};};
        try{ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}equal(c,*ref);
        check(faults==2 && jit.nativeInstructions==0 && c.ip==0xffff,"encoded fetch wrap remains restartable");
    }
#endif
    std::cout<<"Passed "<<checks<<" cross-page JIT checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
