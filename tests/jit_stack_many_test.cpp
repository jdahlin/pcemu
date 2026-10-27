#include "jit.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v, const char *m) { ++checks; if (!v) throw std::runtime_error(m); }
static void equal(const CPU &a, const CPU &b) {
    check(std::equal(std::begin(a.r),std::end(a.r),std::begin(b.r)), "stack-many registers");
    check(a.mem==b.mem, "stack-many memory and page-table effects");
    check(a.ip==b.ip && a.flags==b.flags && a.instructions==b.instructions && a.cr[2]==b.cr[2], "stack-many restart state");
}
int main() { try {
    for (unsigned mode=0;mode<3;++mode) for (bool code32:{false,true}) for (bool wide:{false,true})
    for (bool stack32:{false,true}) for (bool push:{false,true})
    for (bool paged:{false,true}) for (unsigned scenario=0;scenario<10;++scenario) {
        auto p=std::make_unique<CPU>(1024*1024); auto &c=*p;
        c.cr[0]=mode?0x11:0x10; c.s[CS].big=code32; c.s[SS].big=stack32;
        for(auto &s:c.s)s.limit=UINT32_MAX;
        c.s[SS].base=0x20000;
        c.flags=0x202 | (mode==2 ? 0x20000 : 0); for(unsigned r=0;r<8;++r)c.r[r]=0x12340000+r*0x1123;
        unsigned w=wide?4:2;
        uint32_t sp=0x8040;
        if(scenario==1)sp=0x8035; // Unaligned and two bitmap words.
        if(scenario==2)sp=push?0x8004:0x8ffc; // Page crossing.
        if(scenario==3)sp=push?4:0xfffc; // 16-bit stack wrap.
        if(scenario==4)c.s[SS].base=0xa0000-0x8040; // VGA callbacks.
        if(scenario==5){c.s[SS].base=0;sp=0x40;} // Stack shares a translated code bitmap word below.
        c.r[SP]=(stack32?0:0xbeef0000)|sp;
        std::vector<uint8_t> code={0x04,1};
        if(wide!=code32)code.push_back(0x66);
        code.push_back(push?0x60:0x61);
        // A flag consumer after the operation checks deferred flags survive.
        code.insert(code.end(),{0x14,0,0xf4});
        std::copy(code.begin(),code.end(),c.mem.begin());
        for(unsigned j=0x20000;j<0x31000;++j)c.mem[j]=uint8_t(j*37+13);
        if(paged) {
            c.write(0x80000,0x81007,4);
            for(unsigned page=0;page<256;++page)c.write(0x81000+page*4,(page<<12)|7,4);
            c.cr[0]|=0x80000000; c.cr[3]=0x80000; c.flushTLB();
        }
        if(scenario==9){c.s[SS].base=0x10000;c.r[SP]=(stack32?0:0xbeef0000)|0x50;c.mem[0x10040]=0x90;c.mem[0x10041]=0xf4;}
        if(scenario==6)c.s[SS].unusable=true;
        if(scenario==7 && paged) {
            // Fault part-way through the instruction; earlier register/memory
            // changes must match the interpreter, including PUSH's early SP.
            c.r[SP]=(stack32?0:0xbeef0000)|(push?0x8004:0x8ffc);
            unsigned page=push?0x27:0x29;
            c.write(0x81000+page*4,0,4);c.flushTLB();
        }
        auto ref=std::make_unique<CPU>(c);
        unsigned reads[2]={},writes[2]={};
        struct Stop{}; bool faults[2]={};
        for(unsigned k=0;k<2;++k) {
            auto *cpu=k?ref.get():&c;
            cpu->videoRead=[&,k](uint32_t){++reads[k];return uint8_t(0x73);};
            cpu->videoWrite=[&,k](uint32_t,uint8_t){++writes[k];};
            cpu->observeFault=[&,k](const Fault&){faults[k]=true;throw Stop{};};
            if(scenario==8)cpu->watchWrite=[&,k](uint32_t,uint32_t,int){++writes[k];};
        }
        Jit jit(c,true,0,65536);
        // Warm code/data translations in identical order when safe. Cold TLBs
        // must replay before any access; warm ordinary RAM must stay native.
        if(paged && (scenario==0 || scenario==1))for(auto *cpu:{&c,ref.get()}) {
            cpu->read(0,1);cpu->write(0x28000,0x55,1);
        }
        if(scenario==9) {
            c.ip=0x10040;jit.run();c.ip=0;c.instructions=0;jit.nativeInstructions=0;
        }
        try {for(unsigned i=0;i<3;++i)ref->step();}catch(const Stop&){}
        try {for(unsigned i=0;i<4 && c.ip!=code.size()-1;++i)jit.run();}catch(const Stop&){}
        equal(c,*ref);
        check(faults[0]==faults[1] && reads[0]==reads[1] && writes[0]==writes[1], "stack-many callback/fault count");
        if(scenario==0 || scenario==1)check(jit.nativeInstructions==3 && c.nativeMemoryCalls==0,"warm stack-many must execute entirely native");
        if(scenario==9 && push)check(jit.nativeInstructions==2,"second bitmap word must replay before code write");
    }
    // Exercise repeated native frames, cached batches and direct-link exits,
    // with arithmetic flags live across both stack operations.
    for(bool links:{false,true}) for(bool paging:{false,true}) for(bool word:{false,true}) {
        auto p=std::make_unique<CPU>(1024*1024);auto &c=*p;
        c.cr[0]=0x11;c.s[CS].big=c.s[SS].big=true;c.flags=0x202;
        c.r[SP]=0x28035;c.r[CX]=200;c.r[AX]=0x123456ff;
        std::vector<uint8_t> code={0x04,1};
        if(word)code.push_back(0x66);
        code.push_back(0x60);
        if(word)code.push_back(0x66);
        code.push_back(0x61);
        code.insert(code.end(),{0x14,0,0x49,0x75});
        code.push_back(uint8_t(-int(code.size()+1)));code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());
        if(paging) {
            c.write(0x80000,0x81007,4);
            for(unsigned page=0;page<256;++page)c.write(0x81000+page*4,(page<<12)|7,4);
            c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();
        }
        auto ref=std::make_unique<CPU>(c);
        Jit jit(c,true,0,65536);jit.setDirectLinks(links,0);
        unsigned batches=0;
        while(!c.halted && batches++<100) {
            auto b=jit.runBatch();check(b.instructions>0 && b.instructions<=127,"stack-many batch bound");
            for(unsigned n=0;n<b.instructions;++n)ref->step();
            equal(c,*ref);
        }
        check(c.halted && c.r[CX]==0,"stack-many batch completes");
        check(jit.nativeInstructions>=1190,"stack-many batches remain native");
        if(links)check(jit.directLinkTransitions>0,"stack-many linked frames execute");
    }
    std::cout<<"Passed "<<checks<<" stack-many checks\n";
 } catch(const std::exception &e) { std::cerr<<e.what()<<'\n';return 1; }
}
