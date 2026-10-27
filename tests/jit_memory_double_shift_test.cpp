#include "jit.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v,const char*m){++checks;if(!v)throw std::runtime_error(m);}
static void equal(const CPU&a,const CPU&b){check(std::equal(a.r,a.r+8,b.r),"double-shift GPRs");
    check(a.ip==b.ip && a.flags==b.flags && a.instructions==b.instructions && a.cr[2]==b.cr[2],"double-shift flags/IP/count/CR2");
    check(a.mem==b.mem,"double-shift RAM/page effects");}
static void paging(CPU&c){c.write(0x80000,0x81007,4);for(unsigned n=0;n<256;++n)c.write(0x81000+n*4,(n<<12)|7,4);
    c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();}
int main(){try{
    for(bool code32:{false,true})for(bool wide:{false,true})for(bool address32:{false,true})
    for(bool left:{false,true})for(bool immediate:{false,true})for(bool paged:{false,true})for(bool warm:{false,true})
    for(unsigned src:{unsigned(AX),unsigned(CX),unsigned(DX),unsigned(BX),unsigned(SI)})
    for(unsigned count:{0u,1u,15u,16u,17u,31u,32u,33u,255u}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=code32;c.s[FS].base=0x20000;
        c.r[AX]=0xff;c.r[BX]=0x1000;c.r[SI]=0x20;c.r[DX]=0x87654321;c.r[CX]=0x12340000|count;c.flags=0xa02;
        unsigned w=wide?4:2,address=address32?0x21053:0x21033;c.write(address,0x81ab7f00,w);
        std::vector<uint8_t> code={0x04,1,0x64};if(wide!=code32)code.push_back(0x66);if(address32!=code32)code.push_back(0x67);
        code.insert(code.end(),{0x0f,uint8_t((left?0xa4:0xac)+!immediate),uint8_t(0x40|(src<<3)|(address32?4:0))});
        if(address32)code.push_back(0x73);code.push_back(0x13);if(immediate)code.push_back(count);
        code.push_back(0x9f);code.push_back(0xf4);std::copy(code.begin(),code.end(),c.mem.begin());if(paged)paging(c);
        if(paged&&warm){c.read(0,1);auto value=c.read(address,w);c.write(address,value,w);}
        auto ref=std::make_unique<CPU>(c);ref->step();ref->step();ref->step();Jit jit(c,true,0,65536);
        while(c.ip<code.size()-1)jit.run();equal(c,*ref);
        if((!paged||warm||!(count&31)))check(jit.nativeInstructions==3 && c.nativeMemoryCalls==0,"guarded double shift stays native");
    }
    // Count zero validates only the segment. It must not read/write devices,
    // walk an unmapped page, dirty a PTE, or invoke a write watch.
    for(bool left:{false,true})for(bool immediate:{false,true})for(bool zero:{false,true})
    for(bool word:{false,true})for(unsigned scenario=0;scenario<7;++scenario){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.r[AX]=0xff;c.r[DX]=0x87654321;
        c.r[CX]=zero?32:17;c.flags=0xa02;unsigned address=0x40000;
        if(scenario==1)address=0xa0000;
        if(scenario==2)c.s[DS].unusable=true;
        if(scenario==3)address=0x40fff;
        if(scenario==4)address=0;
        c.write(address,0x12345678,4);
        std::vector<uint8_t> code={0x04,1};if(word)code.push_back(0x66);
        code.insert(code.end(),{0x0f,uint8_t((left?0xa4:0xac)+!immediate),0x15});
        for(unsigned n=0;n<4;++n)code.push_back(address>>(8*n));if(immediate)code.push_back(zero?32:17);
        auto end=code.size();code.push_back(0xf4);std::copy(code.begin(),code.end(),c.mem.begin());
        if(scenario==0||scenario==5){paging(c);c.write(0x81100,scenario==0?0:0x40005,4);c.flushTLB();}
        if(scenario==5)c.s[CS].selector=3;
        auto ref=std::make_unique<CPU>(c);std::vector<uint64_t> trace[2];bool faults[2]={};struct Stop{};
        for(unsigned k=0;k<2;++k){auto*cpu=k?ref.get():&c;
            cpu->videoRead=[&,k,cpu](uint32_t a){trace[k].insert(trace[k].end(),{a,cpu->flags});return uint8_t(0x81);};
            cpu->videoWrite=[&,k,cpu](uint32_t a,uint8_t v){trace[k].insert(trace[k].end(),{a,v,cpu->flags});};
            if(scenario==6){cpu->watchStart=0;cpu->watchLength=UINT32_MAX;cpu->watchWrite=[&,k,cpu](uint32_t a,uint32_t v,int){trace[k].insert(trace[k].end(),{a,v,cpu->flags});};}
            cpu->observeFault=[&,k](const Fault&){faults[k]=true;throw Stop{};};
        }
        try{ref->step();ref->step();}catch(const Stop&){}
        Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}
        equal(c,*ref);check(faults[0]==faults[1] && trace[0]==trace[1],"double-shift fault/callback ordering");
        if(zero){check(trace[0].empty(),"zero double shift performs no memory callbacks");check(c.ip==(scenario==2?2:end),"zero validates segment at original IP");}
    }
    // Cold write translation aliases the input PTE: capture before D updates.
    for(bool left:{false,true})for(bool word:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.r[BX]=0x9024;c.r[DX]=0x12345678;
        std::vector<uint8_t> code={0x04,1};if(word)code.push_back(0x66);code.insert(code.end(),{0x0f,uint8_t(left?0xa4:0xac),0x13,1,0xf4});
        std::copy(code.begin(),code.end(),c.mem.begin());paging(c);c.write(0x81024,0x81007,4);c.flushTLB();
        auto ref=std::make_unique<CPU>(c);ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
    }
    // Repeated count changes cross zero/overshift cases inside cached and
    // directly linked frames, with LAHF observing every intermediate result.
    for(bool links:{false,true})for(bool paged:{false,true})for(bool left:{false,true})for(bool word:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.flags=0x202;
        c.r[BX]=0x2000;c.r[DX]=0x87654321;c.r[CX]=200;c.write(0x2000,0x12345678,4);
        std::vector<uint8_t> code;if(word)code.push_back(0x66);
        code.insert(code.end(),{0x0f,uint8_t(left?0xa5:0xad),0x13,0x9f,0x49,0x75});
        code.push_back(uint8_t(-int(code.size()+1)));code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());if(paged){paging(c);c.write(0x2000,0x12345678,4);}
        auto ref=std::make_unique<CPU>(c);Jit jit(c,true,0,65536);jit.setDirectLinks(links,0);unsigned batches=0;
        while(!c.halted && batches++<100){auto batch=jit.runBatch();
            check(batch.instructions>0 && batch.instructions<=127,"double-shift batch bound");
            for(unsigned n=0;n<batch.instructions;++n)ref->step();
            equal(c,*ref);
        }
        check(c.halted && c.r[CX]==0 && jit.nativeInstructions>=790,"memory double-shift loop remains native");
        if(links)check(jit.directLinkTransitions>0,"memory double-shift linked loop");
    }
    std::cout<<"Passed "<<checks<<" memory double-shift JIT checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
