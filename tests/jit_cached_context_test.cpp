#include "jit.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v,const char*s){++checks;if(!v)throw std::runtime_error(s);}
static void equal(const CPU&a,const CPU&b){
    check(std::equal(a.r,a.r+8,b.r),"cached context GPRs");
    check(a.flags==b.flags&&a.ip==b.ip&&a.instructions==b.instructions&&a.cr[2]==b.cr[2]&&a.halted==b.halted,"cached context control");
    check(a.mem==b.mem,"cached context RAM/page effects");
}
static void program(CPU&c,unsigned base=0){
    const uint8_t a[]={0x40,0xff,0xe3},b[]={0x41,0xff,0xe2};
    std::copy(std::begin(a),std::end(a),c.mem.begin()+base);
    std::copy(std::begin(b),std::end(b),c.mem.begin()+base+0x1000);
}
static void paging(CPU&c){
    c.write(0x80000,0x81007,4);for(unsigned n=0;n<256;++n)c.write(0x81000+n*4,(n<<12)|7,4);
    c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();
}
int main(){try{
#if defined(__APPLE__) && defined(__aarch64__)
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})for(bool links:{false,true})for(bool paged:{false,true})for(unsigned mutation=0;mutation<17;++mutation){
        if(mutation==16&&!paged)continue;
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.flags=0xad7;c.r[BX]=0x1000;c.r[DX]=0;program(c);if(paged)paging(c);
        auto ref=std::make_unique<CPU>(c);Jit jit(c,true,0,65536,mode);jit.setDirectLinks(links,0);
        auto advance=[&]{auto part=jit.runBatch(64);for(unsigned n=0;n<part.instructions;++n)ref->step();equal(c,*ref);return part;};
        for(unsigned n=0;n<8;++n){auto part=advance();check(part.dispatches>=8,"cached context warmup exercises multiple transitions");}
        if(mutation==0){
            // Snapshot restore can rewind generation counters: host identity
            // must invalidate an unchanged root's stale compiled successor.
            c.mem[0x1000]=ref->mem[0x1000]=0x46;++c.executionEpoch;
        }
        if(mutation==1){auto replacement=c.mem;c.mem.swap(replacement);std::fill(replacement.begin(),replacement.end(),0xf4);}
        if(mutation==2){auto replacement=c.codeUsed;c.codeUsed.swap(replacement);}
        if(mutation==3){auto replacement=c.generation;c.generation.swap(replacement);}
        if(mutation==4){c.mem.resize(2*1024*1024);ref->mem.resize(2*1024*1024);}
        if(mutation==5){program(c,0x20000);program(*ref,0x20000);c.s[CS].base=ref->s[CS].base=0x20000;}
        if(mutation==6)c.s[CS].selector=ref->s[CS].selector=3;
        if(mutation==7)c.s[CS].big=ref->s[CS].big=false;
        if(mutation==8){c.cr[0]^=0x10000;ref->cr[0]^=0x10000;}
        if(mutation==9){c.write(0x82000,0x81007,4);ref->write(0x82000,0x81007,4);c.cr[3]=ref->cr[3]=0x82000;c.flushTLB();ref->flushTLB();}
        if(mutation==10)c.a20=ref->a20=false;
        if(mutation==11)c.s[SS].big=ref->s[SS].big=true;
        if(mutation==12){c.watchWrite=[](uint32_t,uint32_t,int){throw std::runtime_error("unexpected watched register-only write");};ref->watchWrite=c.watchWrite;}
        if(mutation==13){c.flags|=0x20000;ref->flags|=0x20000;}
        if(mutation==14){c.flushTLB();ref->flushTLB();}
        if(mutation==15){c.write(0,0x47,1);ref->write(0,0x47,1);}
        if(mutation==16){for(auto cpu:{&c,ref.get()}){cpu->write(0x2000,0x46,1);cpu->write(0x2001,0xff,1);cpu->write(0x2002,0xe2,1);cpu->write(0x81004,0x2007,4);cpu->flushTLB();}}
        for(unsigned n=0;n<8;++n)advance();
        jit.clear();advance();
    }
    // A register-only root can validate a new context before reaching a cached
    // successor whose stack addressing was compiled for the previous SS.B.
    for(bool paged:{false,true})for(bool links:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.s[SS].base=0x30000;c.r[SP]=0x18000;c.r[BX]=0x1000;c.r[DX]=0;c.flags=2;program(c);
        const uint8_t stack[]={0x50,0x5e,0xff,0xe2};std::copy(std::begin(stack),std::end(stack),c.mem.begin()+0x1000);if(paged)paging(c);
        auto ref=std::make_unique<CPU>(c);Jit jit(c,true,0,65536);jit.setDirectLinks(links,0);
        auto advance=[&]{auto part=jit.runBatch(64);for(unsigned n=0;n<part.instructions;++n)ref->step();equal(c,*ref);};
        for(unsigned n=0;n<8;++n)advance();c.s[SS].big=ref->s[SS].big=true;for(unsigned n=0;n<8;++n)advance();
    }
    // A replaced code bitmap must be reloaded by an already-compiled store
    // successor before it starts writing code rather than ordinary data.
    for(bool paged:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.r[BX]=0x1000;c.r[DX]=0;c.r[DI]=0x3000;c.flags=2;program(c);
        const uint8_t store[]={0xc6,0x07,0x48,0xff,0xe2};std::copy(std::begin(store),std::end(store),c.mem.begin()+0x1000);if(paged)paging(c);
        auto ref=std::make_unique<CPU>(c);Jit jit(c,true,0,65536);
        auto advance=[&]{auto part=jit.runBatch(64);for(unsigned n=0;n<part.instructions;++n)ref->step();equal(c,*ref);};
        for(unsigned n=0;n<8;++n)advance();auto old=c.codeUsed;c.codeUsed.swap(old);std::fill(old.begin(),old.end(),0);c.r[DI]=ref->r[DI]=0;
        for(unsigned n=0;n<8;++n)advance();check(c.mem[0]==0x48,"cached store used replacement code bitmap");
    }
    // The old allocation remains poisoned and live while cached memory code
    // resumes: accidentally retaining an embedded old RAM base changes EAX.
    for(bool paged:{false,true})for(bool links:{false,true})for(bool shrink:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.r[BX]=0x1000;c.r[DX]=0;c.r[SI]=0x3000;c.flags=2;
        const uint8_t a[]={0x8b,0x06,0xff,0xe3},b[]={0x03,0x46,4,0xff,0xe2};std::copy(std::begin(a),std::end(a),c.mem.begin());std::copy(std::begin(b),std::end(b),c.mem.begin()+0x1000);c.write(0x3000,7,4);c.write(0x3004,9,4);if(paged)paging(c);
        auto ref=std::make_unique<CPU>(c);Jit jit(c,true,0,65536);jit.setDirectLinks(links,0);
        auto advance=[&]{auto part=jit.runBatch(64);for(unsigned n=0;n<part.instructions;++n)ref->step();equal(c,*ref);};
        for(unsigned n=0;n<8;++n)advance();std::vector<uint8_t> old;
        if(shrink){auto pointer=c.mem.data();c.mem.resize(0x2000);ref->mem.resize(0x2000);check(c.mem.data()==pointer,"RAM size guard exercised without a pointer change");}
        else{old=c.mem;c.mem.swap(old);std::fill(old.begin(),old.end(),0xa5);c.write(0x3000,12345,4);ref->write(0x3000,12345,4);}
        for(unsigned n=0;n<8;++n)advance();if(!shrink)check(c.r[AX]==12354,"cached memory transition revalidated replacement RAM");
    }
#endif
    std::cout<<"PASS "<<checks<<" cached context checks\n";
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<" after "<<checks<<" checks\n";return 1;}}
