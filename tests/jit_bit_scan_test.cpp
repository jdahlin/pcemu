#include "jit.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v,const char *s){++checks;if(!v)throw std::runtime_error(s);}
static void equal(const CPU&a,const CPU&b){
    check(std::equal(a.r,a.r+8,b.r),"bit scan GPRs");
    check(a.flags==b.flags&&a.ip==b.ip&&a.instructions==b.instructions&&a.cr[2]==b.cr[2],"bit scan control");
    check(a.mem==b.mem,"bit scan RAM/page effects");
}
int main(){
#if defined(__APPLE__) && defined(__aarch64__)
    // Every register alias, operand size/code default, bit position and zero.
    for(bool code32:{false,true})for(unsigned w:{2u,4u})for(bool reverse:{false,true})
    for(unsigned dst=0;dst<8;++dst)for(unsigned src=0;src<8;++src)for(unsigned bit=0;bit<=w*8;++bit){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.s[CS].big=code32;c.flags=0xad7;
        for(unsigned r=0;r<8;++r)c.r[r]=0xdead9000+r;
        c.r[src]=(w==2?0xbeef0000:0)|(bit==w*8?0:1u<<bit);
        std::vector<uint8_t> v={0x04,1}; // Prior lazy flags must be retained except ZF.
        if((w==4)!=code32)v.push_back(0x66);
        v.insert(v.end(),{0x0f,uint8_t(reverse?0xbd:0xbc),uint8_t(0xc0|(dst<<3)|src),0xf4});
        std::copy(v.begin(),v.end(),c.mem.begin());auto ref=std::make_unique<CPU>(c);
        ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(jit.nativeInstructions==2,"register bit scan stays native");
    }
    // Memory addressing, cold versus warm paging and pre-access MMIO replay.
    for(bool code32:{false,true})for(unsigned w:{2u,4u})for(bool a32:{false,true})
    for(bool reverse:{false,true})for(unsigned dst=0;dst<8;++dst)for(unsigned mode=0;mode<4;++mode)
    for(uint32_t value:{0u,1u,0x8000u,0x80000000u,0xffffffffu}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.s[CS].big=code32;c.flags=0xad7;
        for(unsigned r=0;r<8;++r)c.r[r]=0xfeed0000+r;
        c.r[BX]=0x2000;c.r[SI]=0x20;c.s[FS].base=mode==3?0xa0000:0x10000;
        const unsigned address=c.s[FS].base+(a32?0x2053:0x2033);c.write(address,value,w);
        std::vector<uint8_t> v={0x04,1};if((w==4)!=code32)v.push_back(0x66);if(a32!=code32)v.push_back(0x67);
        v.insert(v.end(),{0x64,0x0f,uint8_t(reverse?0xbd:0xbc),uint8_t(0x40|(dst<<3)|(a32?4:0))});
        if(a32)v.push_back(0x73);v.insert(v.end(),{0x13,0xf4});std::copy(v.begin(),v.end(),c.mem.begin());
        if(mode==1||mode==2){c.write(0x80000,0x81007,4);for(unsigned n=0;n<256;++n)c.write(0x81000+4*n,(n<<12)|7,4);c.cr[3]=0x80000;c.cr[0]=0x80000011;c.flushTLB();if(mode==2)c.read(address,w);}
        unsigned reads=0;if(mode==3)c.videoRead=[&](uint32_t a){++reads;return c.mem[a];};
        auto ref=std::make_unique<CPU>(c);if(mode==3)ref->videoRead=[&](uint32_t a){++reads;return ref->mem[a];};
        ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(jit.nativeInstructions==(mode==1||mode==3?1u:2u),"memory bit scan native/replay");
        if(mode==3)check(reads==2*w,"MMIO read exactly once per engine");
    }
    // A later full flag writer must not erase the restart flags at a fault.
    // Cross-page reads replay and retain the interpreter's page-table effects.
    for(bool reverse:{false,true})for(unsigned w:{2u,4u})for(bool missing:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.s[CS].big=true;c.flags=0xad7;c.r[AX]=0x123456ff;c.r[BX]=0x2fff;c.r[DX]=0xaabbccdd;
        c.write(0x2fff,0x80000000,w);std::vector<uint8_t> v={0x04,1};if(w==2)v.push_back(0x66);
        v.insert(v.end(),{0x0f,uint8_t(reverse?0xbd:0xbc),0x13,0x31,0xc9,0xf4});std::copy(v.begin(),v.end(),c.mem.begin());
        c.write(0x80000,0x81007,4);for(unsigned n=0;n<256;++n)c.write(0x81000+4*n,(n<<12)|7,4);
        if(missing)c.write(0x81000+3*4,0,4);c.cr[3]=0x80000;c.cr[0]=0x80000011;c.flushTLB();
        struct Stop{};unsigned faults=0;auto ref=std::make_unique<CPU>(c);
        for(auto cpu:{&c,ref.get()})cpu->observeFault=[&](const Fault&f){check(f.vector==14,"bit scan page fault");++faults;throw Stop{};};
        ref->step();try{ref->step();if(!missing)ref->step();}catch(const Stop&){}
        Jit jit(c,true,0,65536);try{jit.run();if(!missing)while(c.instructions<ref->instructions)jit.run();}catch(const Stop&){}equal(c,*ref);
        check(faults==(missing?2u:0u),"precise bit scan fault count");
    }
    // Normal warmup admission for singleton scans, zero aliases, cached blocks
    // and direct links in all three block modes.
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})for(bool links:{false,true})for(bool memory:{false,true}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.s[CS].big=true;c.r[BX]=0x2000;c.r[AX]=0x100;c.write(0x2000,0x8000,4);
        const uint8_t v[]={0x0f,0xbc,uint8_t(memory?0x03:0xc0),0xeb,0xfb};std::copy(std::begin(v),std::end(v),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);Jit jit(c,true,8,65536,mode);jit.setDirectLinks(links,0);
        for(unsigned n=0;n<30;++n){jit.runBatch(31);while(ref->instructions<c.instructions)ref->step();equal(c,*ref);}
        check(jit.nativeInstructions>20,"normal-warmup short bit scan admission");
    }
#endif
    std::cout<<"PASS "<<checks<<" bit scan checks\n";
}
