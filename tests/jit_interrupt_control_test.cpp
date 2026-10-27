#include "jit.hpp"
#include "machine.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v,const char*m){++checks;if(!v)throw std::runtime_error(m);}
static void equal(const CPU&a,const CPU&b){
    check(std::equal(a.r,a.r+8,b.r),"interrupt-control registers");
    check(a.ip==b.ip && a.flags==b.flags && a.cr[2]==b.cr[2] && a.instructions==b.instructions &&
          a.interruptBlockedThrough==b.interruptBlockedThrough,"interrupt-control flags/count/shadow");
    check(a.mem==b.mem,"interrupt-control memory");
}
static void paging(CPU&c){c.write(0x80000,0x81007,4);for(unsigned p=0;p<256;++p)c.write(0x81000+p*4,(p<<12)|7,4);
    c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();}
int main(){try{
    // Privilege checks use runtime IOPL, independently of operand/address size.
    for(unsigned mode=0;mode<6;++mode)for(unsigned iopl=0;iopl<4;++iopl)
    for(bool code32:{false,true})for(bool sti:{false,true})for(bool initialIF:{false,true})
    for(bool paged:{false,true})for(unsigned prefixes=0;prefixes<4;++prefixes){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;
        c.cr[0]=mode?0x11:0x10;c.s[CS].big=code32;c.s[CS].selector=mode && mode<5?mode-1:0;
        c.flags=0x110002|(iopl<<12)|(initialIF?IF:0)|(mode==5?0x20000:0);c.r[AX]=0x123456ff;
        c.cr[2]=0x12345678;c.instructions=(uint64_t(1)<<33)+123;c.interruptBlockedThrough=7;
        std::vector<uint8_t> code={0x04,1};
        if(prefixes&1)code.push_back(0x66);
        if(prefixes&2){code.push_back(0x67);code.push_back(0x64);}
        code.push_back(sti?0xfb:0xfa);code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());if(paged)paging(c);
        auto ref=std::make_unique<CPU>(c);bool faults[2]={};struct Stop{};
        for(unsigned k=0;k<2;++k){auto*cpu=k?ref.get():&c;cpu->observeFault=[&,k,cpu](const Fault&f){
            check(f.vector==13 && f.error==0 && f.hasError && cpu->ip==2,"CLI/STI privilege fault restarts at prefixes");
            faults[k]=true;throw Stop{};};}
        try{ref->step();ref->step();}catch(const Stop&){}
        Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}
        equal(c,*ref);unsigned cpl=mode==5?3:mode?mode-1:0;
        check(faults[0]==(cpl>iopl) && faults[0]==faults[1],"CLI/STI CPL versus IOPL");
        check(jit.nativeInstructions==(cpl>iopl?1u:2u),"CLI/STI allowed native, denied interpreter replay");
    }
    // A later flag writer must not erase the pre-fault arithmetic image.
    for(bool sti:{false,true})for(bool allowed:{false,true}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.s[CS].selector=3;
        c.r[AX]=0xff;c.flags=0x202|(allowed?0x3000:0);
        std::vector<uint8_t> code={0x04,1,uint8_t(sti?0xfb:0xfa),0x04,2,0xf4};
        std::copy(code.begin(),code.end(),c.mem.begin());auto ref=std::make_unique<CPU>(c);struct Stop{};
        for(auto*cpu:{&c,ref.get()})cpu->observeFault=[](const Fault&){throw Stop{};};
        try{ref->step();ref->step();ref->step();}catch(const Stop&){}
        Jit jit(c,true,0,65536);try{while(c.ip<5)jit.run();}catch(const Stop&){}
        equal(c,*ref);
    }
    // Reuse the same native block across allowed/denied IOPL changes.
    for(bool vm:{false,true})for(bool sti:{false,true}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.s[CS].selector=3;
        std::vector<uint8_t> code={0x04,1,uint8_t(sti?0xfb:0xfa),0xf4};std::copy(code.begin(),code.end(),c.mem.begin());
        Jit jit(c,true,0,65536);
        for(unsigned iopl:{3u,0u,2u,3u}){
            c.ip=0;c.r[AX]=0xff;c.flags=0x202|(iopl<<12)|(vm?0x20000:0);c.interruptBlockedThrough=0;
            auto ref=std::make_unique<CPU>(c);struct Stop{};bool faults[2]={};
            for(unsigned k=0;k<2;++k){auto*cpu=k?ref.get():&c;cpu->observeFault=[&,k](const Fault&){faults[k]=true;throw Stop{};};}
            try{ref->step();ref->step();}catch(const Stop&){}
            try{jit.run();}catch(const Stop&){}
            equal(c,*ref);check(faults[0]==faults[1],"cached runtime IOPL guard");
        }
        check(jit.cacheHits>=2,"CLI/STI cache reused");
    }
    // STI's shadow starts after an accounted prefix and protects exactly one
    // following instruction. Exercise repeated IF=0/1 cases and 64-bit counts.
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})
    for(bool links:{false,true})for(bool paged:{false,true})for(bool initialIF:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.flags=0x202;
        std::vector<uint8_t> code={0x40,0xeb,1,0x90,0x41,0xfb,0x42,0xf4};std::copy(code.begin(),code.end(),c.mem.begin());
        if(paged)paging(c);
        Jit jit(c,true,0,65536,mode);jit.setDirectLinks(links,0);jit.run();if(c.ip==4)jit.run();
        for(unsigned round=0;round<20;++round){
            c.ip=0;c.flags=2|(initialIF?IF:0);c.instructions=(uint64_t(1)<<34)+100+round*100;c.interruptBlockedThrough=0;
            auto ref=std::make_unique<CPU>(c);auto batch=jit.runBatch();
            for(unsigned n=0;n<batch.instructions;++n)ref->step();
            equal(c,*ref);
            check(batch.instructions==5 && c.ip==7 && c.interruptBlockedThrough==(uint64_t(1)<<34)+104+round*100,
                  "STI shadow protects next instruction after cached prefix");
        }
    }
    // CLI can continue through cached/linked frames while changing only IF.
    for(bool links:{false,true})for(bool paged:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.flags=0x203;c.r[CX]=200;
        std::vector<uint8_t> code={0x40,0xfa,0x49,0x75,0xfb,0xf4};std::copy(code.begin(),code.end(),c.mem.begin());
        if(paged)paging(c);
        auto ref=std::make_unique<CPU>(c);Jit jit(c,true,0,65536);jit.setDirectLinks(links,0);
        unsigned batches=0;while(!c.halted && batches++<100){auto batch=jit.runBatch();
            check(batch.instructions>0 && batch.instructions<=127,"CLI batch bound");
            for(unsigned n=0;n<batch.instructions;++n)ref->step();
            equal(c,*ref);
        }
        check(c.halted && !(c.flags&IF) && (c.flags&CF) && c.r[CX]==0,"CLI loop changes only IF");
        check(jit.nativeInstructions>=790,"CLI loop native coverage");if(links)check(jit.directLinkTransitions>0,"CLI linked loop");
    }
    // Verify the actual board/PIC gate, not only the shadow counter: pending
    // IRQ0 remains blocked at STI, then arrives after the protected instruction.
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})
    for(bool links:{false,true})for(bool batched:{false,true})for(bool sti:{false,true}){
        Machine m("","",1024*1024);auto&c=m.cpu;
        c.cr[0]=0x10;c.flags=sti?2:0x202;c.s[CS].base=c.s[SS].base=0;c.s[CS].selector=0;
        c.ip=0x1000;c.r[SP]=0x9000;
        std::vector<uint8_t> code={0x83,0xc0,1,uint8_t(sti?0xfb:0xfa),0x43,0xf4};
        std::copy(code.begin(),code.end(),c.mem.begin()+0x1000);c.write(8*4,0x2000,4);m.pic[0].mask=0xfe;
        Jit jit(c,true,0,65536,mode);jit.setDirectLinks(links,0);
        if(batched)jit.runBatch(2);else jit.run();
        check(jit.nativeInstructions>=2,"board CLI/STI executed natively");
        m.pic[0].raise_interrupt(0);auto before=c.ip;m.poll();
        check(c.ip==before && (m.pic[0].irr&1),"board pending IRQ blocked by IF/shadow");
        if(sti){
            check(c.r[BX]==0 && c.ip==0x1004,"STI ends before protected instruction");
            if(batched)jit.runBatch(1);else jit.run();
            check(c.r[BX]==1,"STI protected instruction executed");m.poll();
            check(c.ip==0x2000 && !(m.pic[0].irr&1) && (m.pic[0].isr&1),"pending IRQ delivered after STI shadow");
            check(c.r[SP]==0x8ffa && !(c.flags&IF),"IRQ frame after STI shadow");
        }else check(!(c.flags&IF) && !(m.pic[0].isr&1),"CLI suppresses pending IRQ");
    }
    std::cout<<"Passed "<<checks<<" interrupt-control JIT checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
