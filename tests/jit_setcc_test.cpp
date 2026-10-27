#include "jit.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v,const char*m){++checks;if(!v)throw std::runtime_error(m);}
static void equal(const CPU&a,const CPU&b){check(std::equal(a.r,a.r+8,b.r),"SETcc GPRs");
    check(a.flags==b.flags && a.ip==b.ip && a.instructions==b.instructions && a.cr[2]==b.cr[2],"SETcc flags/IP/count/CR2");check(a.mem==b.mem,"SETcc RAM/page effects");}
static unsigned flags(unsigned bits){unsigned f=0x3202;const unsigned positions[]={0,2,4,6,7,11};for(unsigned n=0;n<6;++n)if(bits&(1u<<n))f|=1u<<positions[n];return f;}
static void paging(CPU&c){c.write(0x80000,0x81007,4);for(unsigned n=0;n<256;++n)c.write(0x81000+n*4,(n<<12)|7,4);
    c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();}
int main(){try{
    // All 16 predicates, every arithmetic flag combination, all legacy byte
    // destinations and ignored ModRM.reg fields. Prefixes do not widen SETcc.
    for(bool code32:{false,true})for(unsigned cond=0;cond<16;++cond)for(unsigned f=0;f<64;++f)
    for(unsigned reg=0;reg<8;++reg)for(unsigned group:{0u,7u}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.s[CS].big=code32;c.flags=flags(f);
        for(unsigned n=0;n<8;++n)c.r[n]=0xace081fe + n*0x10001;
        const uint8_t code[]={0x66,0x67,0x0f,uint8_t(0x90+cond),uint8_t(0xc0|(group<<3)|reg),0xf4};
        std::copy(std::begin(code),std::end(code),c.mem.begin());auto ref=std::make_unique<CPU>(c);ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(jit.nativeInstructions==1,"every register condition executes natively");
    }
    for(bool code32:{false,true})for(bool a32:{false,true})for(bool paged:{false,true})for(bool warm:{false,true})
    for(unsigned cond=0;cond<16;++cond)for(unsigned f=0;f<64;++f){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=code32;c.s[FS].base=0x20000;c.flags=flags(f);c.r[BX]=0x1000;c.r[SI]=0x20;
        unsigned address=a32?0x21053:0x21033;c.write(address,0xa5,1);
        std::vector<uint8_t> code={0x64,0x66};if(a32!=code32)code.push_back(0x67);
        code.insert(code.end(),{0x0f,uint8_t(0x90+cond),uint8_t(0x78|(a32?4:0))}); // ignored /7
        if(a32)code.push_back(0x73);code.push_back(0x13);code.push_back(0xf4);std::copy(code.begin(),code.end(),c.mem.begin());
        if(paged)paging(c);if(paged&&warm){c.read(0,1);c.write(address,0xa5,1);}
        auto ref=std::make_unique<CPU>(c);ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(jit.nativeInstructions==1,"guarded memory SETcc completes natively");
        if(!paged||warm)check(c.nativeMemoryCalls==0,"warm SETcc uses inline store");
    }
    // An aliased flag-producing destination must consume the preceding lazy
    // flags, and remain observable even when the following ALU kills all flags.
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})
    for(unsigned cond=0;cond<16;++cond)for(unsigned reg=0;reg<8;++reg)
    for(unsigned value:{0u,1u,0x7fu,0x80u,0xffu})for(bool memory:{false,true}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.s[CS].big=true;c.flags=0xad7;c.r[AX]=0x1234ff00|value;c.r[BX]=0x2000;c.r[DX]=0x87654321;
        std::vector<uint8_t> code={0x04,1,0x0f,uint8_t(0x90+cond),uint8_t(memory?3:0xc0|reg),0x80,0xea,1,0xf4};
        std::copy(code.begin(),code.end(),c.mem.begin());auto ref=std::make_unique<CPU>(c);ref->step();ref->step();ref->step();Jit jit(c,true,0,65536,mode);jit.run();equal(c,*ref);
        check(jit.nativeInstructions==3,"lazy SETcc and dead flag outputs remain native");
    }
    // Video/watch callbacks, unmapped and read-only pages, unusable segments,
    // and code invalidation observe precise prior flags and instruction clocks.
    for(unsigned cond=0;cond<16;++cond)for(unsigned scenario=0;scenario<7;++scenario){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.flags=0xad7;c.r[AX]=0xff;
        unsigned address=scenario==0?0xa0000:scenario==6?9:0x40000;
        std::vector<uint8_t> code={0x04,1,0x0f,uint8_t(0x90+cond),5};for(unsigned n=0;n<4;++n)code.push_back(address>>(8*n));
        code.insert(code.end(),{0xb8,0x12,0x34,0x56,0x78,0xf4});std::copy(code.begin(),code.end(),c.mem.begin());
        if(scenario>=2&&scenario<=4){paging(c);if(scenario==2)c.write(0x81100,0,4);
            if(scenario==3){c.s[CS].selector=3;c.write(0x81100,0x40005,4);}c.flushTLB();}
        if(scenario==4)c.s[DS].unusable=true;
        auto ref=std::make_unique<CPU>(c);std::vector<uint64_t> trace[2];bool faults[2]={};struct Stop{};
        for(unsigned k=0;k<2;++k){auto*cpu=k?ref.get():&c;
            cpu->videoWrite=[&,k,cpu](uint32_t a,uint8_t v){trace[k].insert(trace[k].end(),{a,v,cpu->flags,cpu->instructions,cpu->r[AX]});};
            if(scenario==1){cpu->watchStart=0;cpu->watchLength=UINT32_MAX;cpu->watchWrite=[&,k,cpu](uint32_t a,uint32_t v,int){trace[k].insert(trace[k].end(),{a,v,cpu->flags,cpu->instructions,cpu->r[AX]});};}
            cpu->observeFault=[&,k](const Fault&){faults[k]=true;throw Stop{};};
        }
        try{ref->step();ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}
        // Successful ordinary stores can continue into MOV; use a separate
        // reference step in that case. Watched/device/fault/code paths stop.
        if(c.ip==14 && ref->ip==9)ref->step();
        equal(c,*ref);check(faults[0]==faults[1] && trace[0]==trace[1],"SETcc callbacks/fault ordering");
        if(scenario==6)check(c.ip==9,"SETcc invalidation stops before modified instruction");
    }
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})
    for(bool links:{false,true})for(bool memory:{false,true}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.s[CS].big=true;c.r[AX]=0x1234ff7f;c.r[CX]=200;c.r[BX]=0x2000;c.flags=0xad7;
        std::vector<uint8_t> code={0x04,1,0x0f,0x9c,uint8_t(memory?3:0xc4),0x49,0x75};code.push_back(uint8_t(-int(code.size()+1)));code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());auto ref=std::make_unique<CPU>(c);Jit jit(c,true,0,65536,mode);jit.setDirectLinks(links,0);unsigned batches=0;
        while(!c.halted&&batches++<200){auto batch=jit.runBatch();check(batch.instructions>0&&batch.instructions<=127,"SETcc batch bound");for(unsigned n=0;n<batch.instructions;++n)ref->step();equal(c,*ref);}
        check(c.halted&&c.r[CX]==0&&jit.nativeInstructions>=790,"SETcc loops remain native");
        if(links&&mode!=Jit::BlockMode::Trace)check(jit.directLinkTransitions>0,"SETcc linked execution");
    }
    std::cout<<"Passed "<<checks<<" SETcc JIT checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
