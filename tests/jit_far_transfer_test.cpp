#include "jit.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v,const char*m){++checks;if(!v)throw std::runtime_error(m);}
static void equal(const CPU&a,const CPU&b){check(std::equal(a.r,a.r+8,b.r),"far-transfer GPRs");check(a.ip==b.ip&&a.flags==b.flags&&a.instructions==b.instructions&&a.cr[2]==b.cr[2]&&a.systemAccess==b.systemAccess,"far-transfer control");
    for(unsigned n=0;n<6;++n)check(a.s[n].base==b.s[n].base&&a.s[n].limit==b.s[n].limit&&a.s[n].selector==b.s[n].selector&&a.s[n].big==b.s[n].big&&a.s[n].unusable==b.s[n].unusable,"far-transfer segments");check(a.mem==b.mem,"far-transfer RAM/page effects");}
static void descriptor(CPU&c,unsigned sel,unsigned base,bool code,bool wide,unsigned dpl){unsigned table=sel&4?0x61000:0x60000;c.write(table+(sel&~7u),0xffff|(base<<16),4);c.write(table+4+(sel&~7u),(base&0xff000000)|((base>>16)&255)|0x008f9000|(code?0xa00:0x200)|(wide?0x400000:0)|(dpl<<13),4);}
static void paging(CPU&c,bool warm){c.write(0x80000,0x81007,4);for(unsigned n=0;n<256;++n)c.write(0x81000+n*4,(n<<12)|((n>=0x30&&n<0x40)||n==0x60||n==0x61||n==0x70?3:7),4);c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();
    if(warm){++c.systemAccess;for(unsigned n=0;n<256;++n){auto a=n<<12;c.write(a,c.read(a,1),1);}--c.systemAccess;}}
static std::vector<uint8_t> program(bool code32,unsigned w,bool a32,bool indirect,bool call,uint32_t off,uint16_t selector){
    std::vector<uint8_t> code={0x04,1};if((w==4)!=code32)code.push_back(0x66);if(a32!=code32)code.push_back(0x67);
    if(indirect){code.push_back(0x64);code.insert(code.end(),{0xff,uint8_t(0x40|((call?3:5)<<3)|(a32?4:0))});if(a32)code.push_back(0x73);code.push_back(0x13);}
    else{code.push_back(call?0x9a:0xea);for(unsigned n=0;n<w;++n)code.push_back(off>>(8*n));code.push_back(selector);code.push_back(selector>>8);}code.push_back(0xf4);return code;}
int main(){try{
    for(unsigned mode=0;mode<4;++mode)for(bool code32:{false,true})for(unsigned w:{2u,4u})for(bool stack32:{false,true})
    for(bool a32:{false,true})for(bool indirect:{false,true})for(bool call:{false,true})for(bool ldt:{false,true})for(unsigned translation=0;translation<3;++translation){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=mode?0x11:0x10;c.flags=0xa03|(mode==3?0x20000:0);c.s[CS].big=code32;c.s[CS].selector=mode==2?11:8;c.s[SS].base=0x20000;c.s[SS].big=stack32;c.s[SS].selector=16;c.s[FS].base=0x10000;c.r[AX]=0xff;c.r[BX]=0x2000;c.r[SI]=0x20;c.r[SP]=stack32?0x4000:0xabcd0000;
        c.gdtBase=0x60000;c.gdtLimit=63;c.ldtBase=0x61000;c.ldtLimit=63;const uint16_t selector=mode==3?0x300:uint16_t(8|(ldt?4:0)|(mode==2?3:0));descriptor(c,selector,0x10000,true,!code32,mode==2?3:0);
        const auto off=w==4 && mode!=0 && mode!=3?0x87654321u:0x4321u;auto address=a32?0x12053:0x12033;c.write(address,off,w);c.write(address+w,selector,2);
        auto code=program(code32,w,a32,indirect,call,off,selector);auto end=code.size()-1;std::copy(code.begin(),code.end(),c.mem.begin());if(translation)paging(c,translation==2);
        auto ref=std::make_unique<CPU>(c);ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        if(!translation||translation==2)check(jit.nativeInstructions==2,"warm direct/indirect CALL/JMP stays native");
        check(c.ip==off&&c.s[CS].selector==selector,"far-transfer destination");if(call)check(c.read(c.s[SS].base+c.get(SP,stack32?4:2),w)==(end&(w==4?UINT32_MAX:65535u)),"far CALL pushes decoded end IP");
    }
    // Every supported call-gate width, both TSS formats, maximum parameter
    // count and supervisor stacks entered while the old CS still has CPL3.
    for(bool code32:{false,true})for(unsigned w:{2u,4u})for(unsigned gw:{2u,4u})for(bool oldStack32:{false,true})
    for(bool newStack32:{false,true})for(bool tr32:{false,true})for(unsigned count:{0u,1u,2u,31u})
    for(unsigned oldLevel:{0u,3u})for(bool indirect:{false,true})for(unsigned translation=0;translation<3;++translation){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.flags=0xa03;c.s[CS].big=code32;c.s[CS].selector=32|oldLevel;c.s[SS].base=0x20000;c.s[SS].selector=40|oldLevel;c.s[SS].big=oldStack32;c.s[FS].base=0x10000;c.r[AX]=0xff;c.r[BX]=0x2000;c.r[SI]=0x20;
        c.r[SP]=!oldStack32&&(!newStack32||tr32)?0xabcd4000:0x4000;c.gdtBase=0x60000;c.gdtLimit=63;descriptor(c,8,0x10000,true,!code32,0);descriptor(c,16,0x30000,false,newStack32,0);
        c.write(0x60018,0x00080500,4);c.write(0x6001c,(gw==4?0x1234ec00:0xe400)|count,4);c.trSelector=0x30;c.trBase=0x70000;c.tr32=tr32;
        const auto newSP=tr32&&!newStack32?0xface8000u:0x8000u;c.write(0x70000+(tr32?4:2),newSP,tr32?4:2);c.write(0x70000+(tr32?8:4),16,2);
        const auto oldSP=c.get(SP,gw);for(unsigned n=0;n<count;++n)c.write(0x20000+((oldSP+n*gw)&(oldStack32?UINT32_MAX:65535u)),0xaabb0000+n,gw);
        c.write(0x12053,0x9876,w);c.write(0x12053+w,0x1b,2);auto code=program(code32,w,true,indirect,true,0x9876,0x1b);const auto end=code.size()-1;std::copy(code.begin(),code.end(),c.mem.begin());if(translation)paging(c,translation==2);
        auto ref=std::make_unique<CPU>(c);ref->step();ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        if(!translation||translation==2)check(jit.nativeInstructions==2,"warm call gate with copied parameters stays native");
        check(c.ip==(gw==4?0x12340500u:0x500u)&&c.s[CS].selector==8,"call-gate target ignores instruction offset");
        auto stackBase=c.s[SS].base+c.get(SP,c.s[SS].big?4:2);check(c.read(stackBase,gw)==(end&(gw==4?UINT32_MAX:65535u)),"call-gate operand width selects return IP");
        if(oldLevel==3&&count)check(c.read(stackBase+2*gw,gw)==(0xaabb0000u&(gw==4?UINT32_MAX:65535u)),"call gate copies parameters in reference order");
    }
    // Unsafe pointer/stack/device/descriptor paths retain interpreter ordering,
    // including stack RAM written before a later descriptor fault rolls SP back.
    for(bool indirect:{false,true})for(bool call:{false,true})for(unsigned scenario=0;scenario<7;++scenario){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.flags=0xa03;c.s[CS].big=true;c.s[CS].selector=8;c.s[SS].big=true;c.r[SP]=scenario==0?0xa0008:scenario==6?0x60010:0x40000;c.r[BX]=0x2000;c.r[SI]=0x20;c.r[AX]=0xff;c.s[FS].base=scenario==1?0xa0000-0x2053:0x10000;
        c.gdtBase=0x60000;c.gdtLimit=63;descriptor(c,8,0x10000,true,true,0);auto pointer=c.s[FS].base+0x2053;c.write(pointer,0x1234,4);c.write(pointer+4,8,2);
        if(scenario==2)c.write(0x6000c,0x004f1a00,4);if(scenario==3)c.write(0x6000c,0x004f9200,4);
        auto code=program(true,4,true,indirect,call,0x1234,8);std::copy(code.begin(),code.end(),c.mem.begin());
        if(scenario==5){paging(c,false);c.write(0x810fc,0,4);c.flushTLB();}
        auto ref=std::make_unique<CPU>(c);std::vector<uint64_t> trace[2];bool faults[2]={};struct Stop{};
        for(unsigned k=0;k<2;++k){auto*cpu=k?ref.get():&c;cpu->observeFault=[&,k](const Fault&){faults[k]=true;throw Stop{};};
            cpu->videoRead=[&,k,cpu](uint32_t a){trace[k].insert(trace[k].end(),{a,cpu->r[SP],cpu->flags,cpu->instructions,cpu->ip});const uint8_t pointer[]={0x34,0x12,0,0,8,0};return a>=0xa0000&&a<0xa0006?pointer[a-0xa0000]:uint8_t(0);};
            cpu->videoWrite=[&,k,cpu](uint32_t a,uint8_t v){trace[k].insert(trace[k].end(),{a,v,cpu->r[SP],cpu->flags,cpu->instructions,cpu->ip});};
            if(scenario==4){cpu->watchStart=0;cpu->watchLength=UINT32_MAX;cpu->watchWrite=[&,k,cpu](uint32_t a,uint32_t v,int w){trace[k].insert(trace[k].end(),{a,v,unsigned(w),cpu->r[SP],cpu->flags,cpu->instructions,cpu->ip});};}}
        try{ref->step();ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}equal(c,*ref);check(trace[0]==trace[1]&&faults[0]==faults[1],"far transfer replay callback/fault ordering");
        if(scenario==6&&call)check(faults[0]&&c.read(0x6000c,4)==8,"stack/descriptor alias uses post-push descriptor and preserves writes on fault");
    }
    // A terminal CALL may overwrite its fetched instruction. Mark the same
    // bytes in the reference so exact generation increments can be compared.
    {
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.s[CS].selector=8;c.s[SS].big=true;c.r[SP]=8;c.r[AX]=0xff;c.flags=0x202;c.gdtBase=0x60000;c.gdtLimit=63;descriptor(c,8,0x10000,true,true,0);
        const uint8_t code[]={0x04,1,0x9a,0x34,0x12,0,0,8,0,0xf4};std::copy(std::begin(code),std::end(code),c.mem.begin());auto ref=std::make_unique<CPU>(c);for(unsigned n=0;n<9;++n)ref->markCode(n);ref->step();ref->step();
        Jit jit(c,true,0,65536);jit.run();equal(c,*ref);check(jit.nativeInstructions==2&&c.ip==0x1234&&c.r[SP]==0,"terminal code-writing CALL remains native");check(c.generation==ref->generation&&c.generation[0]==2,"terminal CALL preserves precise code generation updates");
    }
    // A gate writes the complete ring-0 frame before its final CS load faults.
    // Native preflight must replay that sequence, not erase its RAM side effects.
    {
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.flags=0xa03;c.s[CS].big=true;c.s[CS].selector=0x23;c.s[SS].base=0x20000;c.s[SS].selector=0x2b;c.s[SS].big=true;c.r[SP]=0x4000;
        c.gdtBase=0x60000;c.gdtLimit=63;descriptor(c,8,0x10000,false,true,0);descriptor(c,16,0x30000,false,true,0);c.write(0x60018,0x00080500,4);c.write(0x6001c,0xec02,4);
        c.trSelector=0x30;c.tr32=true;c.trBase=0x70000;c.write(0x70004,0x8000,4);c.write(0x70008,16,2);c.write(0x24000,0x11223344,4);c.write(0x24004,0x55667788,4);
        const uint8_t code[]={0x9a,0,0,0,0,0x1b,0};std::copy(std::begin(code),std::end(code),c.mem.begin());auto ref=std::make_unique<CPU>(c);struct Stop{};unsigned faults[2]={};
        for(unsigned k=0;k<2;++k)(k?ref.get():&c)->observeFault=[&,k](const Fault&f){check(f.vector==13&&f.error==8,"gate CS fault reports target selector");++faults[k];throw Stop{};};
        try{ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}equal(c,*ref);check(faults[0]==1&&faults[1]==1&&c.r[SP]==0x4000&&c.s[SS].selector==0x2b,"gate fault restores old stack context");
        const uint32_t frame[]={7,0x23,0x11223344,0x55667788,0x4000,0x2b};for(unsigned n=0;n<6;++n)check(c.read(0x37fe8+n*4,4)==frame[n],"gate frame writes remain after CS fault");
    }
    // An indirect pointer can depend on SP and be overwritten by its own pushes.
    for(unsigned w:{2u,4u}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.s[CS].big=true;c.s[SS].big=true;c.r[SP]=0x2008;c.flags=0x202;std::vector<uint8_t> code;if(w==2)code.push_back(0x66);code.insert(code.end(),{0xff,0x5c,0x24,uint8_t(-int(2*w)),0xf4});std::copy(code.begin(),code.end(),c.mem.begin());c.write(c.r[SP]-2*w,0x1234,w);c.write(c.r[SP]-w,0x100,2);
        auto ref=std::make_unique<CPU>(c);ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);check(jit.nativeInstructions==1,"SP-based overwritten far pointer captured before pushes");
    }
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})for(bool links:{false,true}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.s[CS].big=true;c.s[SS].big=true;const uint8_t code[]={0x9a,7,0,0,0,0,0,0x43,0xf4};std::copy(std::begin(code),std::end(code),c.mem.begin());Jit jit(c,true,0,65536,mode);jit.setDirectLinks(links,0);
        for(unsigned n=0;n<100;++n){c.ip=0;c.s[CS].selector=0;c.s[CS].base=0;c.s[CS].big=true;c.r[SP]=0x2000;c.flags=0x202;auto ref=std::make_unique<CPU>(c);ref->step();auto batch=jit.runBatch();equal(c,*ref);check(batch.instructions==1&&batch.dispatches==1&&c.r[BX]==0,"far CALL keeps the poll before target execution");}check(jit.nativeInstructions==100,"hot far calls stay native");
    }
    // CWSDPMI reuses 16-bit code with both SS.B values. A terminal far
    // transfer owns a deferred native entry (ordinary fn is null); its prefix
    // still embeds the stack address width and must rebuild when SS.B changes.
    // Intel SDM Vol. 3A, 3.4.5.1 D/B: SS.B selects SP/ESP independently of CS.D.
    // https://cdrdv2-public.intel.com/835754/253668-sdm-vol-3a.pdf
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})
    for(bool links:{false,true})for(bool initial32:{false,true})for(bool paged:{false,true}) {
        auto p=std::make_unique<CPU>(1024*1024);auto &c=*p;
        c.cr[0]=0x11;c.s[CS].selector=8;c.s[SS].big=initial32;
        c.gdtBase=0x60000;c.gdtLimit=63;descriptor(c,8,0,true,false,0);
        // PUSH byte +33; POP AX; JMP FAR 0008:0100.
        const uint8_t code[]={0x6a,0x33,0x58,0xea,0,1,8,0};
        std::copy(std::begin(code),std::end(code),c.mem.begin());
        if(paged)paging(c,true);
        Jit jit(c,true,0,65536,mode);jit.setDirectLinks(links,0);
        for(unsigned round=0;round<4;++round) {
            c.ip=0;c.r[SP]=0x28040;c.s[SS].big=initial32 ^ bool(round&1);
            c.r[AX]=0xabcd0000;c.flags=0x202;
            auto ref=std::make_unique<CPU>(c);
            for(unsigned n=0;n<3;++n)ref->step();
            auto before=jit.nativeInstructions;jit.run();equal(c,*ref);
            check(jit.nativeInstructions-before==3,"deferred prefix remains native after SS.B change");
            check(c.ip==0x100 && c.r[SP]==0x28040 && c.r[AX]==0xabcd0033,
                  "deferred boundary uses current stack address width");
        }
    }
    std::cout<<"Passed "<<checks<<" far-transfer JIT checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
