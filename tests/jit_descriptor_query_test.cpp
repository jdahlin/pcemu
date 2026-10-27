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
    check(std::equal(a.r,a.r+8,b.r),"query GPRs");
    check(a.flags==b.flags&&a.ip==b.ip&&a.instructions==b.instructions&&a.cr[2]==b.cr[2]&&a.systemAccess==b.systemAccess,"query control");
    check(a.mem==b.mem,"query RAM/page effects");
}
static void paging(CPU&c,bool user=true){
    c.write(0x80000,0x81007,4);for(unsigned n=0;n<256;++n)c.write(0x81000+n*4,(n<<12)|(user?7:3),4);
    c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();
}
static std::vector<uint8_t> code(bool code32,unsigned w,bool limit,unsigned dst,unsigned src,bool memory=false,bool a32=true){
    std::vector<uint8_t> v={0x04,1};if((w==4)!=code32)v.push_back(0x66);
    if(memory && a32!=code32)v.push_back(0x67);
    if(memory)v.push_back(0x64);
    v.insert(v.end(),{0x0f,uint8_t(limit?3:2),uint8_t((memory?0x40:0xc0)|(dst<<3)|(memory?(a32?4:0):src))});
    if(memory){if(a32)v.push_back(0x73);v.push_back(0x13);}v.push_back(0xf4);return v;
}
int main(){try{
    // Independent Intel SDM protected-mode type table, including not-present
    // descriptors (P is returned as a right, not a query failure condition).
    for(bool limit:{false,true})for(unsigned type=0;type<16;++type)for(bool application:{false,true})
    for(unsigned cpl=0;cpl<4;++cpl)for(unsigned rpl=0;rpl<4;++rpl)for(unsigned dpl=0;dpl<4;++dpl)
    for(bool present:{false,true})for(bool granularity:{false,true}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.s[CS].selector=cpl;c.flags=0xad7;c.r[AX]=0x55;c.r[BX]=8|rpl;c.r[DX]=0xfaceb00c;
        c.gdtBase=0x4000;c.gdtLimit=15;
        const uint32_t low=0xabcdb234,high=0x12050034|(type<<8)|(application?0x1000:0)|(dpl<<13)|(present?0x8000:0)|(granularity?0x800000:0);
        c.write(0x4008,low,4);c.write(0x400c,high,4);auto v=code(true,4,limit,DX,BX);std::copy(v.begin(),v.end(),c.mem.begin());
        const bool system=(0x0a0e & (1u<<type)) || (!limit && (0x1030 & (1u<<type)));
        const bool conforming=application && (type&12)==12;
        const bool valid=(application||system)&&(conforming || (cpl<=dpl&&rpl<=dpl));
        auto native=std::make_unique<CPU>(c);
        c.step();c.step();check(bool(c.flags&ZF)==valid,"SDM query type/visibility table");
        uint32_t expected=limit?((low&65535)|(high&0xf0000)):(high&0xf0ff00);
        if(limit && granularity)expected=(expected<<12)|4095;
        check(c.r[DX]==(valid?expected:0xfaceb00c),"SDM query value/destination retention");
#if defined(__APPLE__) && defined(__aarch64__)
        Jit jit(*native,true,0,65536);jit.run();equal(*native,c);check(jit.nativeInstructions==2,"native SDM query table");
#endif
    }
#if defined(__APPLE__) && defined(__aarch64__)
    // All register aliases, selector widths, code defaults and descriptor type
    // classes compare against the independently checked interpreter semantics.
    for(bool code32:{false,true})for(unsigned w:{2u,4u})for(bool limit:{false,true})
    for(unsigned dst=0;dst<8;++dst)for(unsigned src=0;src<8;++src)for(unsigned kind=0;kind<6;++kind){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=code32;c.flags=0xad7;c.gdtBase=0x4000;c.gdtLimit=15;c.ldtBase=0x5000;c.ldtLimit=15;
        for(unsigned r=0;r<8;++r)c.r[r]=0xfeed0000+r;
        c.r[src]=0xbeef0000|(kind==0?0:kind==1?16:kind==2?12:8);
        const uint32_t hi=kind==3?0x12c08634:kind==4?0x12c08234:0x12c0f234;
        for(unsigned base:{0x4000u,0x5000u}){c.write(base+8,0xabcdb234,4);c.write(base+12,hi,4);}
        auto v=code(code32,w,limit,dst,src);std::copy(v.begin(),v.end(),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);check(jit.nativeInstructions==2,"register query native");
    }
    for(bool code32:{false,true})for(unsigned w:{2u,4u})for(bool limit:{false,true})for(bool a32:{false,true})
    for(unsigned dst=0;dst<8;++dst)for(unsigned mode=0;mode<4;++mode){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=code32;c.flags=0xad7;c.gdtBase=0x4000;c.gdtLimit=15;
        for(unsigned r=0;r<8;++r)c.r[r]=0xfeed0000+r;
        c.r[AX]=0x123456ff;c.r[BX]=0x2000;c.r[SI]=0x20;c.s[FS].base=mode==3?0xa0000:0x10000;
        const auto address=c.s[FS].base+(a32?0x2053:0x2033);c.write(address,8,2);c.write(0x4008,0xabcdb234,4);c.write(0x400c,0x12c0f234,4);
        auto v=code(code32,w,limit,dst,0,true,a32);std::copy(v.begin(),v.end(),c.mem.begin());
        if(mode==1||mode==2){paging(c);if(mode==2){c.read(address,2);c.systemRead(0x4008,4);}}
        std::array<unsigned,2> reads{};auto ref=std::make_unique<CPU>(c);
        if(mode==3){c.videoRead=[&](uint32_t a){++reads[0];return c.mem[a];};ref->videoRead=[&](uint32_t a){++reads[1];return ref->mem[a];};}
        ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(jit.nativeInstructions==(mode==1||mode==3?1u:2u),"memory query native/replay");
        check(reads[0]==reads[1],"query MMIO exact read count");
    }
    // Ring-3 operands obey user permissions; descriptor reads are supervisor
    // accesses even when the descriptor page has no user mapping.
    for(bool limit:{false,true})for(bool memory:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.s[CS].selector=3;c.flags=0xad7;c.gdtBase=0x4000;c.gdtLimit=15;c.r[BX]=memory?0x2000:11;c.r[SI]=0x20;c.r[AX]=0xff;
        c.s[FS].base=0x10000;c.write(0x12053,11,2);c.write(0x4008,0xabcdb234,4);c.write(0x400c,0x12c0f234,4);auto v=code(true,4,limit,DX,BX,memory);std::copy(v.begin(),v.end(),c.mem.begin());
        paging(c);c.write(0x81000+4*4,0x4003,4);c.flushTLB();c.systemRead(0x4008,4);if(memory)c.read(0x12053,2);
        auto ref=std::make_unique<CPU>(c);ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);check(jit.nativeInstructions==2,"ring-3 supervisor descriptor stays native");
    }
    // A descriptor crossing a page or MMIO boundary may require replay after a
    // warm first word. Faults expose prior lazy flags despite a later XOR.
    for(bool limit:{false,true})for(unsigned mode=0;mode<4;++mode){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.flags=0xad7;c.r[AX]=0xff;c.r[BX]=8;c.r[DX]=0xfeedbabe;c.gdtBase=mode==3?0x9fff4:0x4ff4;c.gdtLimit=15;
        c.write(c.gdtBase+8,0xabcdb234,4);c.write(c.gdtBase+12,0x12c0f234,4);auto v=code(true,4,limit,DX,BX);v.insert(v.end()-1,{0x31,0xc9});std::copy(v.begin(),v.end(),c.mem.begin());
        if(mode<3){paging(c);c.systemRead(0x4ffc,4);if(mode==0)c.systemRead(0x5000,4);if(mode==2){c.write(0x81000+5*4,0,4);c.flushTLB();c.systemRead(0x4ffc,4);}}
        struct Stop{};unsigned faults=0;auto ref=std::make_unique<CPU>(c);
        std::vector<std::array<uint64_t,6>> reads[2];
        for(unsigned n=0;n<2;++n){auto cpu=n?ref.get():&c;
            cpu->observeFault=[&](const Fault&f){check(f.vector==14,"query descriptor page fault");++faults;throw Stop{};};
            if(mode==3)cpu->videoRead=[&,cpu,n](uint32_t a){reads[n].push_back({a,cpu->flags,cpu->r[DX],cpu->ip,cpu->instructions,cpu->systemAccess});return cpu->mem[a];};
        }
        ref->step();try{ref->step();if(mode!=2)ref->step();}catch(const Stop&){}
        Jit jit(c,true,0,65536);try{jit.run();if(mode!=2)while(c.instructions<ref->instructions)jit.run();}catch(const Stop&){}equal(c,*ref);
        check(faults==(mode==2?2u:0u),"query fault count");check(reads[0]==reads[1],"descriptor MMIO exact callback state");
    }
    for(bool limit:{false,true})for(bool stackSegment:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.flags=0xad7;c.r[BX]=0x2000;c.r[SI]=0x20;c.r[AX]=0xff;
        c.s[stackSegment?SS:FS].unusable=true;auto v=code(true,4,limit,DX,0,true);if(stackSegment)v[2]=0x36;std::copy(v.begin(),v.end(),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);struct Stop{};unsigned faults=0;
        for(auto cpu:{&c,ref.get()})cpu->observeFault=[&](const Fault&f){check(f.vector==(stackSegment?12:13),"query unusable source segment");++faults;throw Stop{};};
        ref->step();try{ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}equal(c,*ref);check(faults==2,"query source segment fault count");
    }
    for(bool limit:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.s[CS].selector=3;c.s[FS].base=0x10000;c.flags=0xad7;c.r[AX]=0xff;c.r[BX]=0x2000;c.r[SI]=0x20;c.gdtBase=0x4000;c.gdtLimit=15;
        c.write(0x12053,11,2);c.write(0x4008,0xabcdb234,4);c.write(0x400c,0x12c0f234,4);auto v=code(true,4,limit,DX,0,true);std::copy(v.begin(),v.end(),c.mem.begin());
        paging(c);c.write(0x81000+0x12*4,0x12003,4);c.flushTLB();c.systemRead(0x12053,2);c.systemRead(0x4008,4);
        auto ref=std::make_unique<CPU>(c);struct Stop{};unsigned faults=0;
        for(auto cpu:{&c,ref.get()})cpu->observeFault=[&](const Fault&f){check(f.vector==14,"query source user permission fault");++faults;throw Stop{};};
        ref->step();try{ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}equal(c,*ref);check(faults==2&&c.cr[2]==0x12053,"query source permission fault count/address");
    }
    for(unsigned mode=0;mode<2;++mode){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.cr[0]=mode?0x11:0x10;c.flags=0xad7|(mode?0x20000:0);c.s[CS].big=true;c.s[FS].unusable=true;
        auto v=code(true,4,false,DX,0,true);std::copy(v.begin(),v.end(),c.mem.begin());auto ref=std::make_unique<CPU>(c);struct Stop{};unsigned faults=0;
        for(auto cpu:{&c,ref.get()})cpu->observeFault=[&](const Fault&f){check(f.vector==6,"query #UD priority in real/VM86 mode");++faults;throw Stop{};};
        ref->step();try{ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}equal(c,*ref);check(faults==2,"query #UD count");
    }
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})for(bool links:{false,true}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.r[BX]=8;c.gdtBase=0x4000;c.gdtLimit=15;c.write(0x4008,0xabcdb234,4);c.write(0x400c,0x12c0f234,4);
        const uint8_t v[]={0x0f,2,0xd3,0x0f,3,0xc3,0x41,0xeb,0xf7};std::copy(std::begin(v),std::end(v),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);Jit jit(c,true,8,65536,mode);jit.setDirectLinks(links,0);
        for(unsigned n=0;n<30;++n){jit.runBatch(31);while(ref->instructions<c.instructions)ref->step();equal(c,*ref);}
        check(jit.nativeInstructions>30,"query cached normal warmup");
    }
    for(bool prefix:{false,true})for(bool memory:{false,true})for(bool limit:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.r[BX]=memory?0x2000:8;c.r[SI]=0x20;c.s[FS].base=0x10000;c.gdtBase=0x4000;c.gdtLimit=15;
        c.write(0x12053,8,2);c.write(0x4008,0xabcdb234,4);c.write(0x400c,0x12c0f234,4);
        auto v=code(true,4,limit,DX,BX,memory);if(!prefix)v.erase(v.begin(),v.begin()+2);std::copy(v.begin(),v.end(),c.mem.begin());Jit jit(c,true,8,65536);
        for(unsigned n=0;n<30;++n){c.ip=0;auto ref=std::make_unique<CPU>(c);if(prefix)ref->step();ref->step();while(c.instructions<ref->instructions)jit.run();equal(c,*ref);}
        check(jit.nativeInstructions>=20,"singleton/pair query normal-warmup admission");
    }
    // Cached generated code must observe table relocation/limits and descriptor
    // writes dynamically; there is no unversioned descriptor result cache.
    for(bool limit:{false,true}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.r[BX]=8;c.gdtLimit=15;
        auto v=code(true,4,limit,DX,BX);std::copy(v.begin(),v.end(),c.mem.begin());Jit jit(c,true,0,65536);
        for(unsigned n=0;n<40;++n){
            c.ip=0;c.gdtBase=n&1?0x4000:0x5000;c.gdtLimit=n%3?15:7;
            c.write(c.gdtBase+8,0xabcd1234+n,4);c.write(c.gdtBase+12,n%5?0x12c0f234:0x12c08634,4);
            auto ref=std::make_unique<CPU>(c);ref->step();ref->step();jit.run();equal(c,*ref);
        }
    }
#endif
    std::cout<<"PASS "<<checks<<" descriptor query checks\n";
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<" after "<<checks<<" checks\n";return 1;}}
