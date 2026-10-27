#include "jit.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v,const char*m){++checks;if(!v)throw std::runtime_error(m);}
static void equal(const CPU&a,const CPU&b){check(std::equal(a.r,a.r+8,b.r),"multiply GPRs");
    check(a.flags==b.flags && a.ip==b.ip && a.instructions==b.instructions && a.cr[2]==b.cr[2],"multiply flags/IP/count/CR2");
    check(a.mem==b.mem,"multiply memory/page effects");}
static void paging(CPU&c){c.write(0x80000,0x81007,4);for(unsigned n=0;n<256;++n)c.write(0x81000+n*4,(n<<12)|7,4);
    c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();}
static void prefixes(std::vector<uint8_t>&code,bool code32,unsigned w,bool a32){
    if(w!=1 && (w==4)!=code32)code.push_back(0x66);if(a32!=code32)code.push_back(0x67);}
int main(){try{
    const std::array<uint32_t,13> values={0,1,2,0x7f,0x80,0xff,0x7fff,0x8000,0xffff,0x7fffffff,0x80000000,0xffffffff,0x87654321};
    // Complete implicit family, including AL/AH, AX/DX aliases, all other
    // registers, signed overflow boundaries, preserved high halves and flags.
    for(bool code32:{false,true})for(unsigned w:{1u,2u,4u})for(bool sign:{false,true})
    for(unsigned reg=0;reg<8;++reg)for(auto left:values)for(auto right:values){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=code32;
        for(unsigned n=0;n<8;++n)c.r[n]=0xace01234+n;c.r[AX]=left;c.set(reg,right,w);
        c.flags=0xa02|((left^right)&(CF|PF|AF|ZF|SF|OF));
        std::vector<uint8_t> code;prefixes(code,code32,w,code32);
        code.insert(code.end(),{uint8_t(w==1?0xf6:0xf7),uint8_t(0xc0|((sign?5:4)<<3)|reg)});
        auto end=code.size();code.push_back(0xf4);std::copy(code.begin(),code.end(),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(c.ip==end && jit.nativeInstructions==1,"all register multiplies execute natively");
    }
    // FS override, independent address/operand widths, SIB and 16-bit indexed
    // addressing; preceding lazy flags and following ADC consume exact carry.
    for(bool code32:{false,true})for(unsigned w:{1u,2u,4u})for(bool sign:{false,true})
    for(bool a32:{false,true})for(bool paged:{false,true})for(bool warm:{false,true})
    for(auto left:values)for(auto right:values){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=code32;c.s[FS].base=0x20000;
        c.r[AX]=left;c.r[DX]=0xace0abcd;c.r[BX]=0x1000;c.r[SI]=0x20;c.flags=0xa03;
        unsigned address=a32?0x21053:0x21033;c.write(address,right,w);
        std::vector<uint8_t> code={0x80,0xc1,0x7f,0x64};prefixes(code,code32,w,a32);
        code.insert(code.end(),{uint8_t(w==1?0xf6:0xf7),uint8_t(0x40|((sign?5:4)<<3)|(a32?4:0))});
        if(a32)code.push_back(0x73);code.push_back(0x13);
        // ADC DL,0 observes multiply CF while other flags are preserved until it.
        code.insert(code.end(),{0x80,0xd2,0,0xf4});std::copy(code.begin(),code.end(),c.mem.begin());
        if(paged)paging(c);if(paged&&warm){c.read(0,1);c.read(address,w);}
        auto ref=std::make_unique<CPU>(c);ref->step();ref->step();ref->step();Jit jit(c,true,0,65536);
        while(c.ip<code.size()-1)jit.run();equal(c,*ref);
        if(!paged||warm)check(jit.nativeInstructions==3 && c.nativeMemoryCalls==0,"warm memory multiply stays native");
    }
    // Source address may depend on either implicit result register. Capture
    // the input before writing AX/DX.
    for(unsigned reg:{unsigned(AX),unsigned(DX)})for(unsigned w:{1u,2u,4u})for(bool sign:{false,true}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.r[AX]=0x2000;c.r[DX]=0x2000;c.flags=0xad7;
        c.write(0x2000,0x87654321,w);std::vector<uint8_t> code;prefixes(code,true,w,true);
        code.insert(code.end(),{uint8_t(w==1?0xf6:0xf7),uint8_t(((sign?5:4)<<3)|reg),0xf4});
        std::copy(code.begin(),code.end(),c.mem.begin());auto ref=std::make_unique<CPU>(c);ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(jit.nativeInstructions==1,"implicit result may alias source address");
    }
    // Every unsafe read replays before any callback. Check exact preceding
    // flags, instruction clock, implicit GPRs and partial cross-page effects.
    for(unsigned w:{1u,2u,4u})for(bool sign:{false,true})for(unsigned scenario=0;scenario<7;++scenario){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.r[AX]=0x89abcdef;c.r[DX]=0x76543210;c.flags=0xa03;
        unsigned address=scenario==0?0xa0000:scenario==1?0x40fff:0x40000;c.write(address,0x87654321,w);
        std::vector<uint8_t> code={0x04,1};prefixes(code,true,w,true);code.insert(code.end(),{uint8_t(w==1?0xf6:0xf7),uint8_t(((sign?5:4)<<3)|5)});
        for(unsigned n=0;n<4;++n)code.push_back(address>>(8*n));code.push_back(0xf4);std::copy(code.begin(),code.end(),c.mem.begin());
        if(scenario>=2&&scenario<=5){paging(c);if(scenario==3)c.write(0x81100,0,4);
            if(scenario==4){c.s[CS].selector=3;c.write(0x81100,0x40003,4);}c.flushTLB();}
        if(scenario==5)c.s[DS].unusable=true;
        auto ref=std::make_unique<CPU>(c);std::vector<uint64_t> trace[2];bool faults[2]={};struct Stop{};
        for(unsigned k=0;k<2;++k){auto*cpu=k?ref.get():&c;
            cpu->videoRead=[&,k,cpu](uint32_t a){trace[k].insert(trace[k].end(),{a,cpu->flags,cpu->instructions,cpu->r[AX],cpu->r[DX]});return uint8_t(0x81);};
            cpu->observeFault=[&,k](const Fault&){faults[k]=true;throw Stop{};};
            if(scenario==6){cpu->watchStart=0;cpu->watchLength=UINT32_MAX;cpu->watchWrite=[&,k](uint32_t a,uint32_t v,int){trace[k].insert(trace[k].end(),{a,v});};}
        }
        try{ref->step();ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}
        equal(c,*ref);check(faults[0]==faults[1] && trace[0]==trace[1],"multiply precise faults/device callbacks");
    }
    // A memory operand aliases its own PTE. Read-only MUL must match reference
    // A-bit ordering without dirtying translation or repeating the read.
    for(unsigned w:{1u,2u,4u})for(bool sign:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.r[AX]=0x87654321;c.r[BX]=0x9024;c.flags=0xa03;
        std::vector<uint8_t> code={0x04,1};prefixes(code,true,w,true);code.insert(code.end(),{uint8_t(w==1?0xf6:0xf7),uint8_t(((sign?5:4)<<3)|3),0xf4});
        std::copy(code.begin(),code.end(),c.mem.begin());paging(c);c.write(0x81024,0x81007,4);c.flushTLB();
        auto ref=std::make_unique<CPU>(c);ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
    }
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})
    for(bool links:{false,true})for(bool memory:{false,true})for(unsigned w:{1u,2u,4u})for(bool sign:{false,true}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.r[AX]=0x87654321;c.r[BX]=0x2000;c.r[CX]=200;c.write(0x2000,0x87654321,w);
        std::vector<uint8_t> code;prefixes(code,true,w,true);code.insert(code.end(),{uint8_t(w==1?0xf6:0xf7),uint8_t((memory?0:0xc0)|((sign?5:4)<<3)|3),0x9f,0x49,0x75});
        code.push_back(uint8_t(-int(code.size()+1)));code.push_back(0xf4);std::copy(code.begin(),code.end(),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);Jit jit(c,true,0,65536,mode);jit.setDirectLinks(links,0);unsigned batches=0;
        while(!c.halted && batches++<100){auto batch=jit.runBatch();check(batch.instructions>0 && batch.instructions<=127,"multiply batch bound");
            for(unsigned n=0;n<batch.instructions;++n)ref->step();equal(c,*ref);}
        check(c.halted && c.r[CX]==0 && jit.nativeInstructions>=790,"multiply loops execute natively");
        // Trace mode unrolls this four-instruction loop and can complete via
        // side exits without a link transition. Basic/follow must exercise links.
        if(links && mode!=Jit::BlockMode::Trace)check(jit.directLinkTransitions>0,"multiply linked loop");
    }
    std::cout<<"Passed "<<checks<<" implicit multiply JIT checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
