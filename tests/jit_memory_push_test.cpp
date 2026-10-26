#include "jit.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool value, const char *message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
static void compare(const CPU &a, const CPU &b) {
    check(std::equal(std::begin(a.r),std::end(a.r),std::begin(b.r)), "memory PUSH registers differ");
    check(a.ip==b.ip && a.flags==b.flags && a.instructions==b.instructions, "memory PUSH IP/flags/count differs");
    check(a.mem==b.mem, "memory PUSH RAM or paging side effects differ");
}
int main() {
 try {
    for (bool code32 : {false,true}) for (bool wide : {false,true})
    for (bool addr32 : {false,true}) for (bool stack32 : {false,true})
    for (bool paging : {false,true}) for (int seg : {-1,int(FS),int(GS)})
    for (unsigned form=0;form<10;++form) {
        auto ptr=std::make_unique<CPU>(1024*1024); auto &c=*ptr;
        c.cr[0]=0x11; c.s[CS].big=code32; c.s[CS].limit=UINT32_MAX;
        c.s[SS].big=stack32;
        c.s[SS].base=0x10000; c.s[DS].base=0x20000;
        c.s[FS].base=0x30000; c.s[GS].base=0x40000;
        for (auto &s:c.s) s.limit=UINT32_MAX;
        for (unsigned r=0;r<8;++r)c.r[r]=0x100+r*0x110;
        c.r[SP]=0x8000; c.flags=0x202; c.r[AX]=0xff;
        unsigned w=wide?4:2;
        std::vector<uint8_t> code={0x04,1}; // Prior cached result and lazy flags must survive helpers/replay.
        if (wide!=code32)code.push_back(0x66);
        if (addr32!=code32)code.push_back(0x67);
        if(seg>=0)code.push_back(seg==FS?0x64:0x65);
        code.push_back(0xff);
        unsigned offset=0, selected=DS;
        if (form==8) {
            code.push_back(addr32?0x35:0x36); offset=0x4000;
            for(unsigned i=0;i<(addr32?4u:2u);++i)code.push_back(offset>>(8*i));
        } else if (form==9 && addr32) {
            code.insert(code.end(),{0x34,0x8d}); // [ECX*4+disp32], no base.
            unsigned disp=0x1000; offset=c.r[CX]*4+disp;
            for(unsigned i=0;i<4;++i)code.push_back(disp>>(8*i));
        } else {
            unsigned rm=form&7; code.push_back(0x70|rm);
            int displacement=0x23;
            if(addr32) {
                if(rm==SP) {
                    code.push_back(0x24); // [ESP-w]: source aliases the push slot.
                    displacement=-int(w);
                }
                offset=(rm==AX?0u:c.r[rm])+displacement;
                if(rm==SP||rm==BP)selected=SS;
            } else {
                const int base[]={BX,BX,BP,BP,SI,DI,BP,BX};
                const int index[]={SI,DI,SI,DI,-1,-1,-1,-1};
                offset=c.r[base[rm]]+(index[rm]>=0?c.r[index[rm]]:0)+displacement;
                offset&=65535;
                if(base[rm]==BP)selected=SS;
            }
            code.push_back(uint8_t(displacement));
        }
        if(seg>=0)selected=seg;
        auto pointerAddress=c.s[selected].base+offset;
        const uint32_t value=wide?0xc0012345u:0x9abcu;
        c.write(pointerAddress,value,w);
        code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());
        if(paging) {
            c.write(0x80000,0x81007,4);
            for(unsigned page=0;page<256;++page)c.write(0x81000+page*4,(page<<12)|7,4);
            c.cr[0]|=0x80000000; c.cr[3]=0x80000; c.flushTLB();
        }
        auto reference=std::make_unique<CPU>(c);
        reference->step(); reference->step();
        Jit jit(c,true,0,65536);
        auto count=jit.run();
        if(count!=2 || jit.nativeInstructions!=2) {
            std::cerr << "case code="<<code32<<" wide="<<wide<<" addr="<<addr32<<" stack="<<stack32
                      <<" paged="<<paging<<" seg="<<seg<<" form="<<form<<" count="<<count
                      <<" native="<<jit.nativeInstructions<<" ip="<<c.ip<<" calls="<<c.nativeMemoryCalls<<'\n';
        }
        check(count==2 && jit.nativeInstructions==2, "memory PUSH did not lower with preceding ADD");
        compare(c,*reference);
        if(c.ip!=code.size()-1 || c.r[SP]!=0x8000-w)
            std::cerr<<"source case code="<<code32<<" wide="<<wide<<" addr="<<addr32<<" stack="<<stack32
                     <<" paging="<<paging<<" seg="<<seg<<" form="<<form<<" ip="<<c.ip<<" sp="<<c.r[SP]
                     <<" pointer="<<pointerAddress<<'\n';
        check(c.ip==code.size()-1 && c.r[SP]==0x8000-w && c.read(c.s[SS].base+c.r[SP],w)==value, "memory PUSH source/stack incorrect");
        // Reuse the native block with a changed pointer and fresh translations.
        for(auto *cpu:{&c,reference.get()}) {
            cpu->r[SP]=0x8000;cpu->r[AX]=0xff;cpu->ip=0;
            cpu->write(pointerAddress,wide?0x87654321u:0x4321u,w);cpu->flushTLB();
        }
        reference->step();reference->step();jit.run();compare(c,*reference);
        check(c.read(c.s[SS].base+c.r[SP],w)==(wide?0x87654321u:0x4321u), "memory PUSH cached stale source");
    }
    // A guarded pointer read must not touch MMIO before replay, including when
    // the stack also routes to a device. Interpreter callbacks occur once.
    for(bool devicePointer:{false,true})for(bool deviceStack:{false,true}) {
        CPU c(1024*1024); c.s[CS].big=c.s[SS].big=true;c.s[CS].limit=UINT32_MAX;
        c.r[SP]=deviceStack?0xa0004:0x9000;
        unsigned address=devicePointer?0xa0100:0x4000;
        const uint8_t code[]={0xff,0x35,uint8_t(address),uint8_t(address>>8),uint8_t(address>>16),0,0xf4};
        std::copy(std::begin(code),std::end(code),c.mem.begin());c.write(0x4000,0x500,4);
        unsigned reads=0,writes=0;
        c.videoRead=[&](uint32_t a){++reads;return uint8_t(0x500u>>((a-0xa0100)*8));};
        c.videoWrite=[&](uint32_t,uint8_t){++writes;};
        Jit jit(c,true,0,65536);jit.run();
        check(c.ip==6 && c.instructions==1 && c.r[SP]==(deviceStack?0xa0000u:0x8ffcu),"MMIO memory PUSH state incorrect");
        check(reads==(devicePointer?4u:0u)&&writes==(deviceStack?4u:0u),"MMIO memory PUSH duplicated callbacks");
    }
    for(unsigned failure=0;failure<5;++failure) {
        CPU base(1024*1024);base.s[CS].big=base.s[SS].big=true;
        base.cr[0]=0x11;base.cr[3]=0x80000;base.r[SP]=0x9004;
        base.write(0x80000,0x81007,4);
        for(unsigned page=0;page<256;++page)base.write(0x81000+page*4,(page<<12)|7,4);
        base.mem[0]=0xff;base.mem[1]=0x35;base.mem[2]=0;base.mem[3]=0x40;base.mem[6]=0xf4;
        base.write(0x4000,0x500,4);
        if(failure==0)base.write(0x81010,0,4); // Pointer page absent.
        if(failure==1||failure==2)base.write(0x81024,0,4); // Stack page absent.
        if(failure==2)base.s[CS].limit=0x100; // PUSH data is not a CS target; stack #PF still wins.
        if(failure==3)base.s[SS].unusable=true;
        if(failure==4) {
            base.mem[2]=0xff;base.mem[3]=0x4f; // Pointer crosses into absent page.
            base.write(0x4fff,0x500,4);base.write(0x81014,0,4);
        }
        base.cr[0]|=0x80000000;
        auto a=std::make_unique<CPU>(base),b=std::make_unique<CPU>(base);
        struct Stop {};
        Fault fa{},fb{};bool seenA=false,seenB=false;
        a->observeFault=[&](const Fault &f){fa=f;seenA=true;throw Stop{};};
        b->observeFault=[&](const Fault &f){fb=f;seenB=true;throw Stop{};};
        try {a->step();}catch(const Stop &){}
        Jit jit(*b,true,0,65536);try{jit.run();}catch(const Stop &){}
        check(seenA&&seenB&&fa.vector==fb.vector&&fa.error==fb.error,"memory PUSH fault priority differs");
        check(a->cr[2]==b->cr[2],"memory PUSH replay CR2 differs");
        compare(*a,*b);
    }
    // Code-writing and watched stack stores commit the source once and advance IP.
    for(unsigned sp:{4u,0x9000u}) {
        CPU c(1024*1024);c.s[CS].big=c.s[SS].big=true;c.r[SP]=sp;
        c.mem[0]=0xff;c.mem[1]=0x35;c.mem[2]=0;c.mem[3]=0x40;c.mem[6]=0xf4;
        c.write(0x4000,0x500,4);
        unsigned watched=0;
        if(sp!=4) { c.watchStart=sp-4;c.watchLength=4;c.watchWrite=[&](uint32_t,uint32_t,int){++watched;}; }
        Jit jit(c,true,0,65536);jit.run();
        check(c.ip==6&&c.r[SP]==sp-4&&c.instructions==1,"memory PUSH store-exit lost its IP");
        check(c.read(sp-4,4)==0x500 && watched==(sp==4?0u:1u),"memory PUSH source/watch replay incorrect");
    }
    // Multiple source descriptors and dataflow through cached ESP in one block.
    {
        CPU c(1024*1024);c.s[CS].big=c.s[SS].big=true;c.r[SP]=0x9000;
        const uint8_t code[]={0xff,0x35,0,0x40,0,0, 0xff,0x34,0x24, 0xff,0xf4, 0xf4};
        std::copy(std::begin(code),std::end(code),c.mem.begin());c.write(0x4000,0x12345678,4);
        CPU reference=c;for(unsigned n=0;n<3;++n)reference.step();
        Jit jit(c,true,0,65536);check(jit.run()==3 && jit.nativeInstructions==3,"consecutive PUSH failed to lower");
        compare(c,reference);
        check(c.read(0x8ffc,4)==0x12345678 && c.read(0x8ff8,4)==0x12345678 && c.read(0x8ff4,4)==0x8ff8,
              "PUSH memory/ESP evaluated after decrement");
    }
    std::cout<<"Passed "<<checks<<" memory PUSH checks\n";
 }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
