#include "jit.hpp"
#include "jit_x87.hpp"
#include <algorithm>
#include <bit>
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool value,const char *message){++checks;if(!value)throw std::runtime_error(message);}
static void equal(const CPU&a,const CPU&b){
    check(std::equal(a.r,a.r+8,b.r),"x87 GPRs");
    check(a.ip==b.ip && a.flags==b.flags && a.instructions==b.instructions && a.cr[2]==b.cr[2],"x87 restart state");
    check(std::equal(a.fp,a.fp+8,b.fp) && a.fpControl==b.fpControl && a.fpStatus==b.fpStatus && a.fpTag==b.fpTag,
          "x87 arithmetic/control/status/tags");
    check(a.fpIP==b.fpIP && a.fpData==b.fpData && a.fpCS==b.fpCS && a.fpDS==b.fpDS && a.fpOpcode==b.fpOpcode,"x87 environment");
    check(a.mem==b.mem,"x87 RAM/page effects");
}
// Compare at every native return, including a guard in the middle of a
// longer block. A frame may publish only its completed prefix; register-stack
// rotation, flags and RAM must agree even when the remaining suffix replays.
static void sequences(){
    const double values[]={0.,-0.,1.5,-2.5,2147483647.5,-2147483648.5,
        32767.5,-32768.5,std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::denorm_min()};
    for(unsigned top=0;top<8;++top)for(unsigned rc=0;rc<4;++rc)
    for(double value:values)for(unsigned scenario=0;scenario<3;++scenario){
        CPU c(1024*1024);c.cr[0]=0x11;c.s[CS].big=true;c.s[DS].limit=0xffffffff;
        c.fpStatus=(top<<11)|0x4702;c.fpTag=0x5555;c.fpControl=0x37f|(rc<<10);
        c.flags=0xad7;c.r[AX]=0x12345678;
        for(unsigned i=0;i<8;++i)c.fp[i]=std::bit_cast<uint64_t>(double(i+1));
        auto bits=std::bit_cast<uint64_t>(value);c.write(0x4000,uint32_t(bits),4);c.write(0x4004,uint32_t(bits>>32),4);
        c.write(0x4020,0x3fa00000,4);c.write(0x4024,0xfffffff9,4);c.write(0x4028,0xfff9,2);
        std::vector<uint8_t> code;
        auto reg=[&](unsigned op,unsigned m){code.push_back(op);code.push_back(m);};
        auto mem=[&](unsigned op,unsigned m,unsigned addr){reg(op,m);for(unsigned i=0;i<4;++i)code.push_back(addr>>(8*i));};
        code.insert(code.end(),{0x04,1}); // Live integer flags across all ESCs.
        mem(0xdd,5,0x4000); // FLD m64, preserving NaN payload/signed zero.
        reg(0xd9,0xc0);reg(0xd9,0xc9);reg(0xd9,0xe0);reg(0xd9,0xe1);
        mem(0xdd,0x1d,0x4100); // FSTP m64
        mem(0xdb,0x1d,0x4110); // FISTP m32 under all RC modes, including invalids.
        mem(0xd9,5,0x4020);reg(0xd9,0xc0);mem(0xd8,0x0d,0x4020);
        reg(0xde,0xc1);mem(0xda,5,0x4024);mem(0xde,5,0x4028);
        mem(0xdf,0x1d,0x4118); // FISTP m16
        for(unsigned i=0;i<11;++i)reg(0xd9,0xe8); // Wrap TOP in both directions.
        for(unsigned i=0;i<11;++i)reg(0xdd,0xd8);
        reg(0xd9,0xee);reg(0xd9,0xe4);reg(0xdf,0xe0);reg(0xdd,0xd8);
        // Trigger a side exit after a nonempty FP prefix, then continue with
        // another FP instruction and a GPR write in a separately cached block.
        mem(0xd9,5,scenario==1?0xa0000:scenario==2?0x28fff:0x4020);
        reg(0xd9,0xe0);mem(0xd9,0x1d,0x4120);
        code.insert(code.end(),{0x40,0x41});
        const unsigned end=code.size();code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());
        if(scenario==2){
            c.write(0x80000,0x81007,4);for(unsigned i=0;i<256;++i)c.write(0x81000+4*i,(i<<12)|7,4);
            c.write(0x81000+4*0x29,0,4);c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();
        }
        auto ref=std::make_unique<CPU>(c);struct Stop{};unsigned faults[2]={},reads[2]={};
        for(unsigned i=0;i<2;++i){auto*p=i?ref.get():&c;
            p->observeFault=[&,i](const Fault&f){faults[i]=f.vector+1;throw Stop{};};
            p->videoRead=[&,i](uint32_t){++reads[i];return uint8_t(0x3f);};
        }
        Jit jit(c,true,0,1024*1024,Jit::BlockMode::FollowJumps);
        unsigned calls=0;
        while(c.ip<end && !faults[0]){
            check(++calls<100,"native x87 sequence progresses");
            try{jit.run();}catch(const Stop&){}
            while(ref->instructions<c.instructions && !faults[1])try{ref->step();}catch(const Stop&){}
            try{equal(c,*ref);}catch(...){std::cerr<<"sequence top="<<top<<" rc="<<rc<<" value="<<value<<" scenario="<<scenario<<" IP="<<c.ip<<"/"<<ref->ip<<"\n";throw;}
            check(faults[0]==faults[1] && reads[0]==reads[1],"prefix fault/device order");
        }
        if(scenario==0)check(jit.nativeHelperInstructions==0 && calls==1,"long x87 block has no helper exits");
    }
}
static void edgeRegisters(){
    const uint64_t values[]={0,0x8000000000000000ull,0x7ff0000000000000ull,
        0xfff0000000000000ull,0x7ff8000000000123ull,0x7ff0000000000123ull,
        1,0x8000000000000001ull,std::bit_cast<uint64_t>(-.5),std::bit_cast<uint64_t>(-.1)};
    for(auto value:values)for(unsigned rc=0;rc<4;++rc)for(unsigned top=0;top<8;++top)
    for(unsigned op=0xd8;op<=0xdf;++op)for(unsigned m=0xc0;m<=0xff;++m){
        CPU::DecodedX87 x;x.opcode=op;x.modrm=m;if(!jit_detail::nativeX87Form(x))continue;
        CPU c(65536);c.cr[0]=0x11;c.s[CS].big=true;c.fpStatus=0x4702|(top<<11);
        c.fpTag=0xf9a5;c.fpControl=0x37f|(rc<<10);c.flags=0xad7;
        for(unsigned i=0;i<8;++i)c.fp[i]=i&1?std::bit_cast<uint64_t>(double(i+1)):value;
        c.mem[0]=0x04;c.mem[1]=1;c.mem[2]=op;c.mem[3]=m;c.mem[4]=0xf4;
        auto ref=std::make_unique<CPU>(c);ref->step();ref->step();
        Jit jit(c,true,0,65536);jit.run();
        try{equal(c,*ref);}catch(...){std::cerr<<"edge op="<<std::hex<<op<<" m="<<m<<" bits="<<value<<" rc="<<rc<<" top="<<top<<"\n";throw;}
        check(jit.nativeHelperInstructions==0,"edge operation stays native");
    }
}
static void cachedLoops(){
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})
    for(bool links:{false,true}){
        CPU c(65536);c.cr[0]=0x11;c.s[CS].big=true;c.r[CX]=25;c.r[AX]=0xff;
        c.write(0x4000,0x3fa00000,4);
        const uint8_t code[]={0xd9,0xe8,0x04,1,0xd8,5,0,0x40,0,0,0xd9,0x1d,4,0x40,0,0,
            0x89,0x0d,8,0x40,0,0,0x49,0x75,0xe7,0xf4};
        std::copy(std::begin(code),std::end(code),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);Jit jit(c,true,0,1024*1024,mode);jit.setDirectLinks(links);
        for(unsigned run=0;run<3;++run){
            c.ip=ref->ip=0;c.halted=ref->halted=false;c.r[CX]=ref->r[CX]=25;
            while(!c.halted){jit.runBatch(64);while(ref->instructions<c.instructions)ref->step();equal(c,*ref);}
        }
        check(jit.nativeHelperInstructions==0,"cached x87 loop stays native across GPR/memory/branches");
    }
#if defined(__APPLE__) && defined(__aarch64__)
    // Exercise the generated frame's AAPCS64 low-64-bit d8..d15 preservation.
    // In particular, save/restore only the FP registers used by this block.
    CPU c(65536);c.cr[0]=0x11;c.s[CS].big=true;
    c.mem[0]=0xd9;c.mem[1]=0xe8;c.mem[2]=0xd9;c.mem[3]=0xc9;c.mem[4]=0xf4;
    Jit jit(c,true,0,65536);
    uint64_t before[]={1,2,3,4,5,6,7,8},after[8]={};
    asm volatile("ldp d8,d9,[%0]\nldp d10,d11,[%0,#16]\nldp d12,d13,[%0,#32]\nldp d14,d15,[%0,#48]"
        ::"r"(before):"d8","d9","d10","d11","d12","d13","d14","d15","memory");
    jit.run();
    asm volatile("stp d8,d9,[%0]\nstp d10,d11,[%0,#16]\nstp d12,d13,[%0,#32]\nstp d14,d15,[%0,#48]"
        ::"r"(after):"memory");
    check(std::equal(std::begin(before),std::end(before),std::begin(after)),"native frame preserves host FP registers");
#endif
}
int main(){try{
    sequences();
    edgeRegisters();
    cachedLoops();
    // All implemented register forms and memory groups use decoded operations.
    // Faults leave generated host frames before architectural delivery; EM/TS
    // precedes unusable segments, page faults and device operand callbacks.
    for(bool code32:{false,true})for(bool wide:{false,true})for(bool address32:{false,true})
    for(unsigned scenario=0;scenario<6;++scenario)for(unsigned op=0xd8;op<=0xdf;++op)
    for(unsigned form=0;form<72;++form){
        auto p=std::make_unique<CPU>(1024*1024);auto &c=*p;
        c.cr[0]=0x11|(scenario==1?4:scenario==2?8:0);c.s[CS].big=code32;c.s[SS].big=true;
        c.r[AX]=0xff;c.flags=0x202;c.fpStatus=3<<11;c.fpTag=0;c.fpControl=0x77f;
        for(unsigned n=0;n<8;++n)c.fp[n]=std::bit_cast<uint64_t>(double(n+1)+.25);
        unsigned m=form<64?0xc0+form:((form-64)<<3)|(address32?5:6);
        unsigned target=scenario==3?0x28fff:scenario==4?0xa0000:0x24000;
        c.write(target,0x3fc00000,4);c.write(target+4,0x3ff80000,4);
        std::vector<uint8_t> code={0x04,1};
        if(wide!=code32)code.push_back(0x66);
        if(address32!=code32)code.push_back(0x67);
        code.push_back(op);code.push_back(m);
        if(form>=64)for(unsigned n=0;n<(address32?4u:2u);++n)code.push_back(target>>(8*n));
        const auto end=code.size();code.push_back(0xf4);
        std::copy(code.begin(),code.end(),c.mem.begin());
        if(scenario==3){
            c.write(0x80000,0x81007,4);
            for(unsigned n=0;n<256;++n)c.write(0x81000+4*n,(n<<12)|7,4);
            c.write(0x81000+4*0x29,0,4);c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();
        }
        if(scenario==5)c.s[DS].unusable=true;
        auto ref=std::make_unique<CPU>(c);unsigned reads[2]={},writes[2]={},faults[2]={};struct Stop{};
        for(unsigned n=0;n<2;++n){auto *cpu=n?ref.get():&c;
            cpu->observeFault=[&,n](const Fault&f){faults[n]=f.vector+1;throw Stop{};};
            cpu->videoRead=[&,n](uint32_t){++reads[n];return uint8_t(0);};
            cpu->videoWrite=[&,n](uint32_t,uint8_t){++writes[n];};
        }
        try{ref->step();ref->step();}catch(const Stop&){}
        Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}
        try{equal(c,*ref);}catch(...){std::cerr<<"op="<<std::hex<<op<<" m="<<m<<" scenario="<<scenario<<" code32="<<code32<<" wide="<<wide<<" addr32="<<address32<<" IP="<<c.ip<<"/"<<ref->ip<<" flags="<<c.flags<<"/"<<ref->flags<<" count="<<c.instructions<<"/"<<ref->instructions<<" CR2="<<c.cr[2]<<"/"<<ref->cr[2]<<"\n";throw;}
        check(reads[0]==reads[1] && writes[0]==writes[1] && faults[0]==faults[1],"x87 fault/device ordering");
        CPU::DecodedX87 operation;operation.opcode=op;operation.modrm=m;
        const bool native=!(c.cr[0]&12)&&jit_detail::nativeX87Form(operation);
        check(jit.nativeInstructions==2 && (native?jit.nativeHelperInstructions<=1:jit.nativeHelperInstructions==1),"native or decoded x87 work");
        if(native && form<64)check(jit.nativeHelperInstructions==0,"register x87 stays in native block");
        if(!faults[0])check(c.ip==end,"decoded x87 end IP");
    }
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace}) {
        CPU c(65536);c.s[CS].big=true;c.s[CS].base=0x1000;
        const uint8_t code[]={0xb8,1,0,0,0,0x40,0x41,0x42,0xf4};
        std::copy(std::begin(code),std::end(code),c.mem.begin()+0x1000);
        Jit jit(c,true,0,65536,mode);jit.setObservationPoints({0x1006});jit.run();
        check(c.ip==6 && c.r[AX]==2 && c.r[CX]==0 && jit.nativeInstructions==2,"exact native observation boundary");
        jit.run();check(c.r[CX]==1 && c.r[DX]==1,"observation does not skip target instruction");
    }
    // Warming both the fence entry and its predecessors must not let the
    // cached batching path execute through an observation boundary.
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})
    for(bool warm:{false,true}) {
        CPU c(65536);c.cr[0]=0x11;c.s[CS].big=true;
        c.mem[0]=0xeb;c.mem[1]=6;c.mem[8]=0xeb;c.mem[9]=6;c.mem[16]=0x40;c.mem[17]=0xf4;
        Jit jit(c,true,0,65536,mode);jit.setObservationPoints({16});
        if(warm)for(unsigned ip:{0u,8u,16u}){c.ip=ip;jit.run();}
        c.ip=0;c.r[AX]=0;c.halted=false;auto before=jit.nativeInstructions;
        auto batch=jit.runBatch(64);
        check(c.ip==16 && c.r[AX]==0 && batch.instructions==2 && jit.nativeInstructions-before==2,
              "cold/cached batching stops before observation point");
        jit.runBatch(64);check(c.r[AX]==1 && c.halted,"next batch executes observed entry once");
        bool rejected=false;try{jit.setDirectLinks(true);}catch(const std::invalid_argument&){rejected=true;}
        check(rejected,"links cannot be enabled after observation points");
    }
    std::cout<<"Passed "<<checks<<" decoded x87 JIT checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
