#include "jit.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace pc;
static unsigned checks;
static void check(bool v,const char*m){++checks;if(!v)throw std::runtime_error(m);}
static void equal(const CPU&a,const CPU&b){check(std::equal(a.r,a.r+8,b.r),"port I/O GPRs");
    check(a.ip==b.ip && a.flags==b.flags && a.instructions==b.instructions && a.cr[2]==b.cr[2] && a.interruptBlockedThrough==b.interruptBlockedThrough && a.systemAccess==b.systemAccess,"port I/O control state");
    for(unsigned n=0;n<6;++n)check(a.s[n].base==b.s[n].base && a.s[n].selector==b.s[n].selector,"port I/O segments");
    check(a.mem==b.mem,"port I/O RAM/page effects");}
static void paging(CPU&c){c.write(0x80000,0x81007,4);for(unsigned n=0;n<256;++n)c.write(0x81000+n*4,(n<<12)|7,4);
    c.cr[0]|=0x80000000;c.cr[3]=0x80000;c.flushTLB();}
int main(){try{
    for(bool code32:{false,true})for(bool wide:{false,true})for(bool addressOverride:{false,true})
    for(unsigned op:{0xe4u,0xe5u,0xe6u,0xe7u,0xecu,0xedu,0xeeu,0xefu})
    for(unsigned permission=0;permission<11;++permission)for(bool paged:{false,true}){
        auto p=std::make_unique<CPU>(1024*1024);auto&c=*p;c.s[CS].big=code32;c.r[AX]=0x8765ff7f;c.r[DX]=0xace0ffff;c.r[BX]=0x12345678;c.flags=0xa03;c.instructions=0x100000005ull;
        if(permission){c.cr[0]=0x11;if(permission!=1)c.s[CS].selector=3;}
        if(permission==2)c.flags|=0x3000;
        if(permission>=6&&permission<=8)c.flags|=0x23000;
        c.tr32=permission>=3 && permission!=5;c.trBase=0x30000;c.trLimit=0x2081;c.write(0x30066,0x80,2);
        if(permission==9)c.trLimit=0x66;
        if(permission==10)c.trLimit=0x207f; // wide DX access crosses port 65535
        if(permission==4||permission==8){c.write(0x3009f,0x80,1);c.write(0x3207f,0x80,1);}
        std::vector<uint8_t> code={0x04,1};if(wide!=code32)code.push_back(0x66);if(addressOverride)code.push_back(0x67);
        code.push_back(op);if(op<0xe8)code.push_back(0xff);auto end=code.size();code.insert(code.end(),{0x43,0xf4});std::copy(code.begin(),code.end(),c.mem.begin());
        if(paged)paging(c);
        auto ref=std::make_unique<CPU>(c);std::vector<uint64_t> trace[2];bool faulted[2]={};struct Stop{};
        const auto width=(op&1)?(wide?4u:2u):1u;
        for(unsigned k=0;k<2;++k){auto*cpu=k?ref.get():&c;
            auto record=[&,k,cpu](uint16_t port,unsigned w,uint32_t value){trace[k].insert(trace[k].end(),{port,w,value,cpu->flags,cpu->ip,cpu->instructions,cpu->r[AX],cpu->r[DX],cpu->systemAccess});
                // Every callback sees canonical prefix state; callback mutations
                // survive because there is no generated frame to write them back.
                cpu->r[BX]^=0x13579bdf;cpu->flags^=DF;};
            cpu->input=[record](uint16_t port,int w){record(port,w,0x1234abcd);return 0x1234abcdu;};
            cpu->output=[record](uint16_t port,uint32_t value,int w){record(port,w,value);};
            cpu->observeFault=[&,k](const Fault&f){check(f.vector==13&&f.error==0,"I/O bitmap denial is GP(0)");faulted[k]=true;throw Stop{};};
        }
        try{ref->step();ref->step();}catch(const Stop&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}
        equal(c,*ref);check(trace[0]==trace[1]&&faulted[0]==faulted[1],"I/O permission/callback equivalence");
        check(jit.nativeInstructions==2,"compiled prefix plus I/O accounted natively");
        if(!faulted[0]){check(c.ip==end&&trace[0].size()==9,"I/O stops at its poll boundary");check(trace[0][4]==end&&trace[0][5]==0x100000007ull,"callback sees end IP and full-width clock");
            check(trace[0][0]==(op<0xe8?0xff:0xffff)&&trace[0][1]==width,"decoded port/width");}
        else check(trace[0].empty(),"denied permission invokes no device callback");
    }
    // Missing devices preserve OUT state and return all ones for IN widths.
    for(unsigned op:{0xe4u,0xe5u,0xe6u,0xe7u,0xecu,0xedu,0xeeu,0xefu})for(bool wide:{false,true}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.s[CS].big=true;c.r[AX]=0x87654321;c.r[DX]=0x1234;c.flags=0xad7;
        std::vector<uint8_t> code;if(!wide)code.push_back(0x66);code.push_back(op);if(op<0xe8)code.push_back(0x40);code.push_back(0xf4);std::copy(code.begin(),code.end(),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);check(jit.nativeInstructions==1,"single decoded I/O native");
    }
    // Device exceptions unwind exclusively through C++ frames. Fault callbacks
    // roll back GPR/segment/flag mutations; ordinary exceptions retain them.
    for(bool input:{false,true})for(bool fault:{false,true}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.s[CS].big=true;c.r[AX]=0xff;c.r[BX]=0x12345678;c.flags=0xa03;
        const uint8_t code[]={0x04,1,uint8_t(input?0xe4:0xe6),0x40,0x43,0xf4};std::copy(std::begin(code),std::end(code),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);unsigned calls[2]={};struct Stop{};
        for(unsigned k=0;k<2;++k){auto*cpu=k?ref.get():&c;auto fail=[&,k,cpu]{++calls[k];cpu->r[BX]=0xdeadbeef;cpu->flags^=DF;cpu->s[DS].base=0x1000;if(fault)throw Fault{13,0,true};throw std::runtime_error("device stop");};
            cpu->input=[fail](uint16_t,int)->uint32_t{fail();return 0;};cpu->output=[fail](uint16_t,uint32_t,int){fail();};cpu->observeFault=[](const Fault&){throw Stop{};};}
        try{ref->step();ref->step();}catch(const Stop&){}catch(const std::runtime_error&){}Jit jit(c,true,0,65536);try{jit.run();}catch(const Stop&){}catch(const std::runtime_error&){}
        equal(c,*ref);check(calls[0]==1&&calls[1]==1,"exception never repeats device callback");check(c.ip==(fault?2u:4u),"device exception restart versus post-fetch IP");
    }
    // A prefix changes the next I/O opcode. Its invalidation exit must never
    // be mistaken for the deferred I/O completion token.
    {
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.s[CS].big=true;const uint8_t code[]={0xc6,5,7,0,0,0,0x90,0xe6,0x40,0xf4};std::copy(std::begin(code),std::end(code),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);ref->step();unsigned calls=0;c.output=[&](uint16_t,uint32_t,int){++calls;};Jit jit(c,true,0,65536);jit.run();equal(c,*ref);check(calls==0&&c.ip==7,"prefix code invalidation suppresses deferred I/O");
    }
    // A denied V86 I/O really enters the guest GP handler through the TSS
    // ring-0 stack, preserving the complete restart frame, not just an observer.
    {
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.gdtBase=0x6000;c.gdtLimit=31;
        c.write(0x6008,0xffff,4);c.write(0x600c,0x00cf9a00,4);c.write(0x6010,0xffff,4);c.write(0x6014,0x00cf9200,4);
        c.cr[0]|=1;c.trSelector=24;c.trBase=0x3000;c.trLimit=0x100;c.tr32=true;
        c.write(0x3004,0x9000,4);c.write(0x3008,16,2);c.write(0x3066,0x80,2);c.write(0x3088,1,1);
        c.idtBase=0x2000;c.idtLimit=2047;c.write(0x2000+13*8,0x00080500,4);c.write(0x2004+13*8,0x8e00,4);
        c.flags=0x20202;for(unsigned n=0;n<6;++n)c.segment(n,(n==CS?1:n==SS?2:n==ES?3:n==DS?4:n==FS?5:6)*0x100);
        c.ip=0x10;c.r[SP]=0x12340800;c.write(0x1010,0x40e6,2);unsigned calls=0;c.output=[&](uint16_t,uint32_t,int){++calls;};
        auto ref=std::make_unique<CPU>(c);ref->step();Jit jit(c,true,0,65536);jit.run();equal(c,*ref);
        check(calls==0&&c.ip==0x500&&c.cpl()==0&&!(c.flags&0x20000)&&c.r[SP]==0x8fd8,"compiled I/O delivers V86 GP on ring-0 stack");
        const uint32_t frame[]={0,0x10,0x100,0x20202,0x12340800,0x200,0x300,0x400,0x500,0x600};
        for(unsigned n=0;n<10;++n)check(c.read(0x8fd8+n*4,4)==frame[n],"compiled I/O exact V86 exception frame");
    }
    // Device callbacks may clear the code cache safely after frame return.
    {
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.s[CS].big=true;c.mem[0]=0xe6;c.mem[1]=0x40;c.mem[2]=0xf4;Jit jit(c,true,0,65536);
        c.output=[&](uint16_t,uint32_t,int){jit.clear();c.r[BX]=0xdeadbeef;};check(jit.run()==1&&c.ip==2&&c.r[BX]==0xdeadbeef,"callback safely clears JIT");
    }
    // Repeated I/O returns to polling each time, with no cached/link bypass.
    for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace})for(bool links:{false,true}){
        auto p=std::make_unique<CPU>(65536);auto&c=*p;c.s[CS].big=true;c.r[CX]=200;c.flags=0x202;const uint8_t code[]={0xe6,0x40,0x49,0x75,0xfb,0xf4};std::copy(std::begin(code),std::end(code),c.mem.begin());
        auto ref=std::make_unique<CPU>(c);unsigned calls[2]={};c.output=[&](uint16_t,uint32_t,int){++calls[0];};ref->output=[&](uint16_t,uint32_t,int){++calls[1];};Jit jit(c,true,0,65536,mode);jit.setDirectLinks(links,0);unsigned batches=0;
        while(!c.halted&&batches++<512){auto before=calls[0];auto batch=jit.runBatch();check(batch.instructions>0&&batch.instructions<=127,"I/O batch bound");check(calls[0]-before<=1,"I/O cannot bypass device poll");for(unsigned n=0;n<batch.instructions;++n)ref->step();equal(c,*ref);}
        check(c.halted&&calls[0]==200&&calls[1]==200&&jit.nativeInstructions>=590,"I/O native loop completion");
    }
    std::cout<<"Passed "<<checks<<" port I/O JIT checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
