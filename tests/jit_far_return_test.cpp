#include "jit.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v,const char*m){++checks;if(!v)throw std::runtime_error(m);}
static void equal(const CPU&a,const CPU&b){check(std::equal(a.r,a.r+8,b.r),"far-return GPRs");
    check(a.ip==b.ip&&a.flags==b.flags&&a.instructions==b.instructions&&a.cr[2]==b.cr[2]&&a.interruptBlockedThrough==b.interruptBlockedThrough,"far-return control state");
    for(unsigned n=0;n<6;++n)check(a.s[n].base==b.s[n].base&&a.s[n].limit==b.s[n].limit&&a.s[n].selector==b.s[n].selector&&a.s[n].big==b.s[n].big&&a.s[n].unusable==b.s[n].unusable,"far-return hidden segments");
    check(a.mem==b.mem,"far-return RAM/page effects");}
static void paging(CPU&c){c.write(0x80000,0x81007,4);for(unsigned n=0;n<256;++n)c.write(0x81000+n*4,(n<<12)|7,4);c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();}
static void descriptor(CPU&c,unsigned sel,unsigned base,bool code,bool wide,unsigned dpl){
    c.write(0x60000+(sel&~7u),0xffff|(base<<16),4);c.write(0x60004+(sel&~7u),(base&0xff000000)|((base>>16)&255)|0x008f9000|(code?0xa00:0x200)|(wide?0x400000:0)|(dpl<<13),4);}
int main(){try{
    for(unsigned mode=0;mode<5;++mode)for(bool code32:{false,true})for(unsigned w:{2u,4u})
    for(bool stack32:{false,true})for(bool newStack32:{false,true})for(bool outer:{false,true})
    for(unsigned form=0;form<5;++form)for(unsigned returnedFlags:{0u,0xad7u,0x330fd7u})for(unsigned translation=0;translation<3;++translation){
        const bool iret=form==4;const unsigned extra=form==1?1:form==2?3:form==3?65520:0;
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=mode?0x11:0x10;c.flags=0xa03|(mode>=3?0x20000:0)|(mode==3?0x3000:0);
        c.s[CS].big=code32;c.s[CS].selector=mode==2?27:8;c.s[SS].selector=16;c.s[SS].base=0x20000;c.s[SS].big=stack32;
        c.gdtBase=0x60000;c.gdtLimit=63;descriptor(c,8,0x10000,true,!code32,0);descriptor(c,16,0x20000,false,stack32,0);descriptor(c,24,0x10000,true,!code32,3);descriptor(c,32,0x30000,false,newStack32,3);
        c.r[SP]=stack32?0x4000:0xabcdfff0;c.r[AX]=0xff;c.r[DX]=0x87654321;
        std::vector<uint8_t> code={0x04,1};if((w==4)!=code32)code.push_back(0x66);const auto start=code.size()-(code.size()>2?1:0);
        code.push_back(iret?0xcf:form?0xca:0xcb);if(form&&!iret){code.push_back(extra);code.push_back(extra>>8);}code.push_back(0xf4);std::copy(code.begin(),code.end(),c.mem.begin());
        uint32_t sp=c.r[SP];std::vector<uint32_t> reads;
        auto advance=[&](unsigned n){sp=stack32?sp+n:(sp&0xffff0000u)|((sp+n)&65535);};
        auto pushFrame=[&](uint32_t v){auto a=0x20000+(stack32?sp:sp&65535);c.write(a,v,w);reads.push_back(a);advance(w);};
        const uint32_t target=mode>=3?0x300:((outer||mode==2)?27:8);
        pushFrame(w==4?0x87654321:0x4321);pushFrame(target);if(iret)pushFrame(returnedFlags);
        bool vmTarget=iret&&mode==1&&w==4&&(returnedFlags&0x20000);
        if(vmTarget){pushFrame(0x12345678);pushFrame(0x200);pushFrame(0x300);pushFrame(0x400);pushFrame(0x500);pushFrame(0x600);}
        else if(mode>0&&mode<3&&outer&&mode!=2){if(!iret)advance(extra);pushFrame(0x12345678);pushFrame(35);}
        if(translation){paging(c);if(translation==2){c.read(0,1);for(auto a:reads)c.read(a,w);c.read(0x60000,4);}}
        auto ref=std::make_unique<CPU>(c);bool faults[2]={};struct Stop{};
        for(unsigned k=0;k<2;++k)(k?ref.get():&c)->observeFault=[&,k](const Fault&){faults[k]=true;throw Stop{};};
        try{ref->step();ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}
        equal(c,*ref);check(faults[0]==faults[1],"far-return matching fault");
        const bool denied=iret&&mode==4;
        const bool safeFrame=std::all_of(reads.begin(),reads.end(),[&](unsigned a){return (a&4095)+w<=4096;});
        if(!translation||translation==2)check(jit.nativeInstructions==((denied||!safeFrame)?1u:2u),"safe warm complete far-return frame stays native");
        if(denied)check(c.ip==start,"V86 denied IRET restarts at original prefix");
        if(vmTarget)check(c.r[SP]==0x12345678&&(c.flags&0x20000)&&c.s[CS].base==uint16_t(target)*16,"native IRET enters V86 with full ESP");
    }
    // Device/cold/faulting stack reads and descriptor failures decline before
    // any architectural progress. The interpreter retains partial read effects.
    for(bool iret:{false,true})for(unsigned w:{2u,4u})for(unsigned scenario=0;scenario<7;++scenario){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.s[SS].big=true;c.r[SP]=scenario==0?0xa0000:scenario==1?0x40fff:0x40000;c.r[AX]=0xff;c.flags=0xa03;
        c.gdtBase=0x60000;c.gdtLimit=63;descriptor(c,8,0x10000,true,true,0);
        c.write(c.r[SP],0x1234,w);c.write(c.r[SP]+w,8,w);c.write(c.r[SP]+2*w,0x202,w);
        if(scenario==2)c.s[SS].unusable=true;
        if(scenario==3)c.write(0x6000c,0x004f1a00,4); // nonpresent code
        if(scenario==4)c.write(0x6000c,0x004f9200,4); // wrong descriptor type
        std::vector<uint8_t> code={0x04,1};if(w==2)code.push_back(0x66);code.push_back(iret?0xcf:0xcb);code.push_back(0xf4);std::copy(code.begin(),code.end(),c.mem.begin());
        if(scenario>=5){paging(c);c.write(0x81100,scenario==5?0:0x40003,4);if(scenario==6)c.s[CS].selector=3;c.flushTLB();}
        auto ref=std::make_unique<CPU>(c);std::vector<uint64_t> trace[2];bool faults[2]={};struct Stop{};
        for(unsigned k=0;k<2;++k){auto*cpu=k?ref.get():&c;cpu->videoRead=[&,k,cpu](uint32_t a){trace[k].insert(trace[k].end(),{a,cpu->r[SP],cpu->flags,cpu->instructions,cpu->ip});return uint8_t(0);};cpu->observeFault=[&,k](const Fault&){faults[k]=true;throw Stop{};};}
        try{ref->step();ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}
        equal(c,*ref);check(faults[0]==faults[1]&&trace[0]==trace[1],"far-return fault/callback ordering");
    }
    // Repeated dynamic return frames cannot bypass the poll through cached or
    // linked tails, even when their code metadata remains hot.
    for(bool iret:{false,true})for(bool links:{false,true})for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.s[CS].big=true;c.s[SS].big=true;c.mem[0]=iret?0xcf:0xcb;c.mem[1]=0x43;c.mem[2]=0xf4;Jit jit(c,true,0,65536,mode);jit.setDirectLinks(links,0);
        for(unsigned n=0;n<100;++n){c.ip=0;c.s[CS].selector=0;c.s[CS].base=0;c.s[CS].big=true;c.r[SP]=0x2000;c.flags=0x202;c.write(0x2000,1,4);c.write(0x2004,0,4);c.write(0x2008,0x202,4);
            auto ref=std::make_unique<CPU>(c);ref->step();auto batch=jit.runBatch();equal(c,*ref);check(batch.instructions==1&&batch.dispatches==1&&c.r[BX]==0,"far return always stops before next guest instruction");}
        check(jit.nativeInstructions==100,"hot far returns stay native");
    }
    std::cout<<"Passed "<<checks<<" far-return JIT checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
