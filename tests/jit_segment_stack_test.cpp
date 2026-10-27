#include "jit.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v,const char*m){++checks;if(!v)throw std::runtime_error(m);}
static void equal(const CPU&a,const CPU&b){
    check(std::equal(a.r,a.r+8,b.r),"segment-stack registers");
    check(a.ip==b.ip && a.flags==b.flags && a.instructions==b.instructions && a.cr[2]==b.cr[2] &&
          a.interruptBlockedThrough==b.interruptBlockedThrough,"segment-stack control/flags/shadow");
    check(a.mem==b.mem,"segment-stack RAM/page effects");
    for(unsigned s=0;s<6;++s)check(a.s[s].base==b.s[s].base && a.s[s].limit==b.s[s].limit &&
        a.s[s].selector==b.s[s].selector && a.s[s].big==b.s[s].big && a.s[s].unusable==b.s[s].unusable,"segment-stack hidden state");
}
static void paging(CPU&c){c.write(0x80000,0x81007,4);for(unsigned p=0;p<256;++p)c.write(0x81000+p*4,(p<<12)|7,4);
    c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();}
static void opcode(std::vector<uint8_t>&code,unsigned seg,bool push){
    if(seg<4)code.push_back((seg<<3)|(push?6:7));
    else{code.push_back(0x0f);code.push_back((seg==FS?0xa0:0xa8)|(push?0:1));}
}
int main(){try{
    for(unsigned mode=0;mode<4;++mode)for(bool code32:{false,true})for(bool wide:{false,true})
    for(bool stack32:{false,true})for(bool paged:{false,true})for(bool push:{false,true})
    for(unsigned seg=0;seg<6;++seg)for(unsigned scenario=0;scenario<9;++scenario){
        if(!push && seg==CS)continue;
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;
        c.cr[0]=mode?0x11:0x10;c.s[CS].big=code32;c.s[CS].selector=mode==2?3:0;
        c.s[SS].big=stack32;c.s[SS].base=0x20000;
        for(auto&s:c.s)s.limit=UINT32_MAX;
        c.flags=0x3202|(mode==3?0x20000:0);c.r[AX]=0x123456ff;
        if(push && seg!=CS)c.s[seg].selector=0x9234;
        c.gdtBase=0x10000;c.gdtLimit=0x17;
        c.write(0x10008,0xffff,4);c.write(0x1000c,stack32?0x9200:0x409200,4); // Flip SS.B on POP SS.
        unsigned sp=0x8040;
        if(scenario==2)sp=push?0x8001:0x8fff;
        if(scenario==3)c.s[SS].unusable=true;
        if(scenario==4)c.s[SS].base=0xa0000-sp;
        if(scenario==6)c.write(0x1000c,0,4);
        if(scenario==7)sp=push?1:0xffff;
        if(scenario==8)c.gdtBase=0xa0000;
        c.r[SP]=(stack32?0:0xbeef0000)|sp;c.write(c.s[SS].base+sp,0xabcd0008,4);
        std::vector<uint8_t> code={0x04,1};if(wide!=code32)code.push_back(0x66);
        // Address/segment overrides do not change stack addressing.
        if(scenario==1){code.push_back(0x67);code.push_back(0x64);}
        opcode(code,seg,push);unsigned end=code.size();code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());if(paged)paging(c);
        if(paged && scenario==0){c.read(0,1);c.write(0x28000,0,1);c.systemRead(0x10008,4);}
        auto ref=std::make_unique<CPU>(c);unsigned reads[2]={},writes[2]={};std::vector<uint32_t> observedSP[2];bool faults[2]={};struct Stop{};
        for(unsigned k=0;k<2;++k){auto*cpu=k?ref.get():&c;
            cpu->observeFault=[&,k](const Fault&){faults[k]=true;throw Stop{};};
            cpu->videoRead=[&,k,cpu](uint32_t addr){++reads[k];observedSP[k].push_back(cpu->r[SP]);if(scenario==8){unsigned v=addr<0xa000c?0xffff:0x409200;
                return uint8_t(v>>((addr&3)*8));}return uint8_t((addr&3)==0?8:0);};
            cpu->videoWrite=[&,k,cpu](uint32_t,uint8_t){++writes[k];observedSP[k].push_back(cpu->r[SP]);};
            if(scenario==5)cpu->watchWrite=[&,k,cpu](uint32_t,uint32_t,int){++writes[k];observedSP[k].push_back(cpu->r[SP]);};
        }
        try{ref->step();ref->step();}catch(const Stop&){}
        Jit jit(c,true,0,65536);try{for(unsigned n=0;n<3 && c.ip!=end;++n)jit.run();}catch(const Stop&){}
        equal(c,*ref);check(faults[0]==faults[1] && reads[0]==reads[1] && writes[0]==writes[1],"segment-stack fault/device ordering");
        check(observedSP[0]==observedSP[1],"segment-stack callback sees exact SP ordering");
        if(scenario==0)check(jit.nativeInstructions==2 && c.nativeMemoryCalls==0,"warm segment stack operations lower");
    }
    // POP SS must increment the old SP/ESP before changing the stack width;
    // a cached prefix must be accounted before establishing its shadow.
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})
    for(bool links:{false,true})for(bool old32:{false,true})for(bool paged:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=true;c.s[SS].big=old32;
        c.flags=0x202;c.gdtBase=0x10000;c.gdtLimit=0x17;c.write(0x10008,0xffff,4);c.write(0x1000c,old32?0x9200:0x409200,4);
        c.r[SP]=old32?0x28040:0xbeef8040;c.write(old32?0x28040:0x8040,8,4);
        std::vector<uint8_t> code={0x40,0xeb,1,0x90,0x41,0x17,0x42,0xf4};std::copy(code.begin(),code.end(),c.mem.begin());
        if(paged){paging(c);c.systemRead(0x10008,4);c.read(old32?0x28040:0x8040,4);}
        Jit jit(c,true,0,65536,mode);jit.setDirectLinks(links,0);jit.run();
        if(c.ip==4)jit.run();
        for(unsigned round=0;round<12;++round){
            c.ip=0;c.s[SS].big=old32;c.s[SS].base=0;c.s[SS].unusable=false;
            c.r[SP]=old32?0x28040:0xbeef8040;c.instructions=100+round*100;c.interruptBlockedThrough=0;
            auto ref=std::make_unique<CPU>(c);auto batch=jit.runBatch();
            for(unsigned n=0;n<batch.instructions;++n)ref->step();
            equal(c,*ref);
            check(batch.instructions==5 && c.ip==7 && c.interruptBlockedThrough==104+round*100,
                "POP SS accounted shadow protects following instruction");
            check(c.r[SP]==(old32?0x28044u:0xbeef8044u) && c.s[SS].big!=old32,"POP SS uses old stack width");
        }
    }
    // POP reads a 16-bit selector while operand size still advances SP/ESP.
    for(unsigned seg:{unsigned(DS),unsigned(SS),unsigned(FS),unsigned(GS)}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.cr[0]=0x11;c.s[CS].big=c.s[SS].big=true;
        c.flags=0x202;c.r[AX]=0xff;c.r[SP]=0x28ffe;c.write(0x28ffe,8,2);
        c.gdtBase=0x10000;c.gdtLimit=0x17;c.write(0x10008,0xffff,4);c.write(0x1000c,0x409200,4);
        std::vector<uint8_t> code={0x04,1};opcode(code,seg,false);code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());paging(c);c.write(0x81000+0x29*4,0,4);c.flushTLB();
        auto ref=std::make_unique<CPU>(c);bool faults[2]={};struct Stop{};
        for(unsigned k=0;k<2;++k){auto*cpu=k?ref.get():&c;cpu->observeFault=[&,k,cpu](const Fault&f){
            check(f.vector==14 && cpu->cr[2]==0x29000 && cpu->ip==2,"segment POP high-word stack page fault");faults[k]=true;throw Stop{};};}
        try{ref->step();ref->step();}catch(const Stop&){}
        Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}
        equal(c,*ref);check(!faults[0]&&!faults[1] && c.r[SP]==0x29002 && jit.nativeInstructions==2,"segment POP reads only selector width and advances full operand width");
    }
    std::cout<<"Passed "<<checks<<" segment-stack JIT checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
