#include "jit.hpp"
#include "jit_runtime.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v,const char*m){++checks;if(!v)throw std::runtime_error(m);}
static void equal(const CPU&a,const CPU&b){check(std::equal(a.r,a.r+8,b.r),"string GPRs");
    check(a.ip==b.ip && a.flags==b.flags && a.instructions==b.instructions && a.cr[2]==b.cr[2],"string IP/flags/count/CR2");
    check(a.mem==b.mem,"string RAM/page effects");
    const auto &x=a.repeatResume,&y=b.repeatResume;check(x.valid==y.valid,"string repeat continuation validity");
    if(x.valid)check(x.selector==y.selector && x.base==y.base && x.ip==y.ip && x.initialFlags==y.initialFlags && x.lastFlags==y.lastFlags &&
        x.ax==y.ax && x.cx==y.cx && x.si==y.si && x.di==y.di,"string continuation state");}
static void paging(CPU&c){c.write(0x80000,0x81007,4);for(unsigned n=0;n<256;++n)c.write(0x81000+n*4,(n<<12)|7,4);
    c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();}
static uint32_t get(const CPU&c,uint32_t p,unsigned w){uint32_t v=0;std::memcpy(&v,c.mem.data()+p,w);return v;}
static void put(CPU&c,uint32_t p,uint32_t v,unsigned w){std::memcpy(c.mem.data()+p,&v,w);}
int main(){try{
    for(bool code32:{false,true})for(bool address32:{false,true})for(unsigned width:{1u,2u,4u})
    for(unsigned kind:{0xa4u,0xa6u,0xaau,0xacu,0xaeu})for(unsigned repeat:{0u,0xf2u,0xf3u})
    for(bool backwards:{false,true})for(bool paged:{false,true})for(bool warm:{false,true})
    for(unsigned count:{0u,1u,3u,4095u,4096u,4097u,8197u}){
        // Non-REP LODS/STOS retain their existing inline lowering.
        if(!repeat && (kind==0xaa||kind==0xac))continue;
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=code32;
        c.s[DS].base=0x10000;c.s[ES].base=0x20000;c.flags=0xa03|(backwards?DF:0);c.r[AX]=0x87654321;
        c.r[SI]=address32?0x4000:0xbeef4000;c.r[DI]=address32?0x8000:0xcafe8000;c.r[CX]=address32?count:0x12340000|count;
        for(unsigned n=0x8000;n<0x50000;++n)c.mem[n]=uint8_t(n*37+(n>>8));
        unsigned mask=address32?UINT32_MAX:65535u,delta=backwards?uint32_t(-int(width)):width;
        // REPE compares equal throughout; REPNE compares unequal throughout.
        if(kind==0xa6||kind==0xae)for(unsigned i=0;i<(repeat?count:1);++i){
            unsigned si=(0x4000+delta*i)&mask,di=(0x8000+delta*i)&mask;
            uint32_t value=kind==0xa6?get(c,c.s[DS].base+si,width):c.get(AX,width);
            put(c,c.s[ES].base+di,repeat==0xf2?~value:value,width);
        }
        std::vector<uint8_t> code;if(repeat)code.push_back(repeat);
        if(width!=1 && (width==4)!=code32)code.push_back(0x66);if(address32!=code32)code.push_back(0x67);
        code.push_back(kind+(width!=1));unsigned end=code.size();code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());if(paged)paging(c);
        if(paged&&warm)for(unsigned page=0;page<128;++page){auto value=c.read(page*4096,1);c.write(page*4096,value,1);}
        auto ref=std::make_unique<CPU>(c);Jit jit(c,true,0,65536);unsigned calls=0;
        do{ref->step();auto n=jit.run();check(n==1,"string reference batch counts once");equal(c,*ref);check(++calls<10,"string batch makes progress");}while(c.ip!=end);
        if(!paged||warm)check(jit.nativeInstructions==calls,"warm string batches stay native");
        check(c.instructions==(repeat&&count>4096?(count+4095)/4096:1),"REP keeps reference 4096-element scheduling");
    }
    // Physical overlap must propagate in architectural element order, not
    // silently acquire memmove semantics. Exercise both overlap directions.
    for(bool backwards:{false,true})for(unsigned width:{1u,2u,4u})for(int distance:{-8,-3,-1,0,1,3,8}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.flags=0x202|(backwards?DF:0);
        c.r[SI]=0x10080;c.r[DI]=c.r[SI]+distance;c.r[CX]=200;
        for(unsigned n=0xf000;n<0x11000;++n)c.mem[n]=uint8_t(n*7+(n>>8));
        std::vector<uint8_t> code={0xf3};if(width==2)code.push_back(0x66);code.push_back(width==1?0xa4:0xa5);code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());auto ref=std::make_unique<CPU>(c);ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
    }
    // Early comparison termination must not fault on speculative later pages.
    for(unsigned kind:{0xa6u,0xaeu})for(unsigned repeat:{0xf2u,0xf3u})for(unsigned stop:{0u,1u,127u}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.flags=0xa03;c.r[AX]=0x55;
        c.r[SI]=0x10000;c.r[DI]=0x20000;c.r[CX]=4097;
        std::fill(c.mem.begin()+0x10000,c.mem.begin()+0x12000,0x55);
        std::fill(c.mem.begin()+0x20000,c.mem.begin()+0x22000,repeat==0xf3?0x55:0x66);
        c.mem[0x20000+stop]=repeat==0xf3?0x66:0x55;
        c.mem[0]=repeat;c.mem[1]=kind;c.mem[2]=0xf4;paging(c);c.write(0x81000+0x21*4,0,4);c.flushTLB();
        auto ref=std::make_unique<CPU>(c);ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(c.ip==2 && c.r[CX]==4097-stop-1,"comparison stop precedes later page fault");
    }
    // A completed native chunk followed by a partial fault must retain the
    // ORIGINAL pre-REP flags across helper/interpreter scheduling boundaries.
    for(unsigned kind:{0xa6u,0xaeu}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.flags=0xa03;c.r[AX]=0x55;
        c.r[SI]=0x10005;c.r[DI]=0x20005;c.r[CX]=8200;
        std::fill(c.mem.begin()+0x10000,c.mem.begin()+0x13000,0x55);std::fill(c.mem.begin()+0x20000,c.mem.begin()+0x23000,0x55);
        c.mem[0]=0xf3;c.mem[1]=kind;c.mem[2]=0xf4;paging(c);
        unsigned faultPage=kind==0xa6?0x12:0x22;c.write(0x81000+faultPage*4,0,4);c.flushTLB();
        for(unsigned page:{0u,0x10u,0x11u,0x20u,0x21u})c.read(page*4096,1);
        auto ref=std::make_unique<CPU>(c);Jit jit(c,true,0,65536);ref->step();jit.run();equal(c,*ref);
        check(jit.nativeInstructions==1 && c.repeatResume.valid && c.r[CX]==4104,"first REP chunk native with original-flags continuation");
        struct Stop{};bool faults[2]={};for(unsigned k=0;k<2;++k){auto*cpu=k?ref.get():&c;cpu->observeFault=[&,k,cpu,faultPage](const Fault&f){
            check(f.vector==14 && f.partialRepeat && cpu->flags==0xa03 && cpu->ip==0 && cpu->cr[2]==faultPage*4096,"partial REP fault restores original flags");faults[k]=true;throw Stop{};};}
        try{ref->step();}catch(const Stop&){}try{jit.run();}catch(const Stop&){}equal(c,*ref);check(faults[0]&&faults[1],"partial REP fault delivered once");
    }
    // Device/watch/code paths decline before callbacks. Compare the exact
    // visible instruction clock, flags and partially advanced indices.
    for(unsigned kind:{0xa4u,0xa6u,0xaau,0xacu,0xaeu})for(unsigned scenario=0;scenario<5;++scenario){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.flags=0xa03;
        c.r[AX]=0x55;c.r[SI]=0x10000;c.r[DI]=0x20000;c.r[CX]=3;c.r[DX]=UINT32_MAX;
        if(scenario==0)c.r[SI]=0xa0000;if(scenario==1)c.r[DI]=0xa0000;
        if(scenario==3)c.s[ES].unusable=true;if(scenario==4)c.r[DI]=0;
        std::vector<uint8_t> code={0x83,0xc2,1,0xf3,uint8_t(kind),0xf4};std::copy(code.begin(),code.end(),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);std::vector<uint64_t> trace[2];bool faults[2]={};struct Stop{};
        for(unsigned k=0;k<2;++k){auto*cpu=k?ref.get():&c;
            auto observe=[&,k,cpu](uint32_t a,uint32_t v){trace[k].insert(trace[k].end(),{a,v,cpu->instructions,cpu->flags,cpu->r[SI],cpu->r[DI],cpu->r[CX]});};
            cpu->videoRead=[observe](uint32_t a){observe(a,0x55);return uint8_t(0x55);};
            cpu->videoWrite=[observe](uint32_t a,uint8_t v){observe(a,v);};
            if(scenario==2){cpu->watchStart=0;cpu->watchLength=UINT32_MAX;cpu->watchWrite=[observe](uint32_t a,uint32_t v,int){observe(a,v);};}
            cpu->observeFault=[&,k](const Fault&){faults[k]=true;throw Stop{};};
        }
        try{ref->step();ref->step();}catch(const Stop&){}
        Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}equal(c,*ref);
        check(trace[0]==trace[1] && faults[0]==faults[1],"string callback/fault order and clock");
    }
    // REP must return to the poll after precisely one reference-sized batch,
    // whether complete, empty or requiring another chunk. No following INC.
    for(bool links:{false,true})for(unsigned count:{0u,3u,4096u,4097u}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.flags=0x202;
        c.r[SI]=0x10000;c.r[DI]=0x20000;c.r[CX]=count;c.r[DX]=0;
        const uint8_t code[]={0x83,0xc0,1,0xf3,0xa4,0x42,0xf4};std::copy(std::begin(code),std::end(code),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);ref->step();ref->step();Jit jit(c,true,0,65536);jit.setDirectLinks(links,0);
        auto batch=jit.runBatch();equal(c,*ref);check(batch.instructions==2 && c.r[DX]==0,"REP device-poll boundary precedes following instruction");
    }
    // A20 folding is physical; source/destination logical indices remain wide.
    for(bool backwards:{false,true}){
        auto p=std::make_unique<CPU>(2*1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.a20=false;c.flags=0x202|(backwards?DF:0);
        c.r[SI]=0x100100;c.r[DI]=0x120100;c.r[CX]=256;
        for(unsigned n=0;n<c.mem.size();++n)c.mem[n]=uint8_t(n*31+(n>>8));
        const uint8_t code[]={0xf3,0xa4,0xf4};std::copy(std::begin(code),std::end(code),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);check(jit.nativeInstructions==1,"REP A20 fold native");
    }
    std::cout<<"Passed "<<checks<<" guarded string JIT checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
