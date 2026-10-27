// Execute externally generated architectural vectors or a standalone test ROM.
// This runner has no host BIOS callbacks and never substitutes expected CPU state.
#include "cpu.hpp"
#include "jit.hpp"
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
using namespace pc;
static uint32_t word(std::istream &in) {
    unsigned char b[4];if(!in.read(reinterpret_cast<char *>(b),4))throw std::runtime_error("Truncated vector file");
    return uint32_t(b[0])|uint32_t(b[1])<<8|uint32_t(b[2])<<16|uint32_t(b[3])<<24;
}
static std::string string(std::istream &in) {
    auto n=word(in);if(n>4096)throw std::runtime_error("Vector name too long");
    std::string s(n,'\0');if(!in.read(s.data(),n))throw std::runtime_error("Truncated vector name");return s;
}
using Memory=std::map<uint32_t,uint8_t>;
static Memory memory(std::istream &in) {
    auto n=word(in);if(n>1048576)throw std::runtime_error("Oversized sparse memory");
    Memory result;for(unsigned i=0;i<n;++i){auto a=word(in);char b;if(a>=16*1024*1024||!in.get(b))throw std::runtime_error("Invalid memory byte");result[a]=uint8_t(b);}return result;
}
static constexpr std::array<int,8> registers{AX,BX,CX,DX,SI,DI,BP,SP};
static constexpr std::array<int,6> segments{CS,DS,ES,FS,GS,SS};
static constexpr std::array<const char *,20> names{"cr0","cr3","eax","ebx","ecx","edx","esi","edi","ebp","esp","cs","ds","es","fs","gs","ss","eip","eflags","dr6","dr7"};
static void load(CPU &c,const std::array<uint32_t,20> &r) {
    c.cr[0]=r[0];c.cr[3]=r[1];
    for(unsigned i=0;i<8;++i)c.r[registers[i]]=r[2+i];
    for(unsigned i=0;i<6;++i)c.s[segments[i]]={r[10+i]<<4,65535,uint16_t(r[10+i]),false,false};
    c.ip=r[16];c.flags=r[17];c.dr[6]=r[18];c.dr[7]=r[19];
}
static std::array<uint32_t,20> save(const CPU &c) {
    std::array<uint32_t,20> r{};r[0]=c.cr[0];r[1]=c.cr[3];
    for(unsigned i=0;i<8;++i)r[2+i]=c.r[registers[i]];
    for(unsigned i=0;i<6;++i)r[10+i]=c.s[segments[i]].selector;
    r[16]=c.ip;r[17]=c.flags;r[18]=c.dr[6];r[19]=c.dr[7];return r;
}
// MOO records include the terminating hardware HLT. Execute the instruction,
// then account for its injected terminator without fetching random branch RAM.
static std::string execute(const std::array<uint32_t,20> &initial,const std::array<uint32_t,20> &expected,
                           const Memory &before,const Memory &after,uint32_t mask,uint32_t exception,
                           uint32_t flagAddress,bool native,uint64_t &nativeCount) {
    CPU c;load(c,initial);for(auto [a,b]:before)c.mem[a]=b;
    int observed=-1;c.observeFault=[&](const Fault &f){observed=f.vector;};
    c.input=[](uint16_t,int){return 0u;};c.output=[](uint16_t,uint32_t,int){};
    Jit jit(c,native,0,1024*1024);
    if(native&&!jit.enabled())throw std::runtime_error("Native JIT unavailable; select interpreter explicitly");
    if(native)jit.run();else c.step();
    nativeCount+=jit.nativeInstructions;
    if(!c.halted)c.ip=(c.ip+1)&65535;
    auto actual=save(c);std::ostringstream errors;
    if(observed!=(exception==0xffffffffu?-1:int(exception)))errors<<"exception="<<observed<<" expected="<<int32_t(exception)<<' ';
    for(unsigned i=0;i<20;++i){uint32_t m=i==17?mask:0xffffffffu;
        if((actual[i]^expected[i])&m)errors<<names[i]<<"="<<std::hex<<actual[i]<<" expected="<<expected[i]<<" mask="<<m<<' ';}
    for(auto [a,b]:after){uint8_t m=255;
        if(flagAddress!=0xffffffffu && a>=flagAddress && a<flagAddress+2)m=mask>>((a-flagAddress)*8);
        if((c.mem[a]^b)&m){errors<<"ram["<<std::hex<<a<<"]="<<unsigned(c.mem[a])<<" expected="<<unsigned(b)<<' ';break;}}
    return errors.str();
}
static int vectors(const std::string &path,const std::string &backend) {
    std::ifstream in(path,std::ios::binary);char magic[8];if(!in.read(magic,8)||std::string(magic,8)!=std::string("PCX86V1\0",8))throw std::runtime_error("Invalid vector header");
    auto count=word(in);if(!count||count>1000000)throw std::runtime_error("Invalid vector count");
    unsigned failed=0;uint64_t nativeCount=0;
    for(unsigned n=0;n<count;++n){auto name=string(in);auto mask=word(in),exception=word(in),flagAddress=word(in);
        std::array<uint32_t,20> initial{},expected{};for(auto &v:initial)v=word(in);for(auto &v:expected)v=word(in);
        auto before=memory(in),after=memory(in);
        for(bool native:{false,true}){if((backend=="interpreter"&&native)||(backend=="jit"&&!native))continue;
            std::string error;
            try {error=execute(initial,expected,before,after,mask,exception,flagAddress,native,nativeCount);}
            catch(const std::exception &e){error=std::string("execution error: ")+e.what();}
            catch(const Fault &f){error="unhandled CPU fault "+std::to_string(f.vector);}
            if(!error.empty()){++failed;if(failed<=20)std::cout<<"FAIL "<<(native?"jit ":"interpreter ")<<name<<' '<<error<<'\n';}}
    }
    if(in.peek()!=std::char_traits<char>::eof())throw std::runtime_error("Trailing vector bytes");
    std::cout<<"RESULT vectors="<<count<<" executions="<<count*(backend=="both"?2:1)<<" failures="<<failed<<" nativeInstructions="<<nativeCount<<'\n';
    return failed?1:0;
}
static int rom(const std::string &path,bool native,const std::string &output) {
    CPU c;std::ifstream file(path,std::ios::binary);std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),{});
    if(data.size()!=65536)throw std::runtime_error("Expected a 64 KiB test ROM");
    std::copy(data.begin(),data.end(),c.mem.begin()+0xf0000);c.s[CS]={0xf0000,65535,0xf000,false,false};c.ip=0xfff0;
    std::ofstream text(output);unsigned post=0;uint64_t faults=0;
    c.observeFault=[&](const Fault &){++faults;};
    c.input=[](uint16_t,int){return 0u;};
    c.output=[&](uint16_t p,uint32_t v,int){if(p==0x190){post=v&255;std::cout<<"POST "<<std::hex<<post<<std::dec<<'\n'<<std::flush;}if(p==0xe9)text.put(char(v));};
    Jit jit(c,native,0,16*1024*1024);if(native&&!jit.enabled())throw std::runtime_error("Native JIT unavailable");
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(120);
    while(!c.halted&&c.instructions<500000000){if(native)jit.run();else c.step();if((c.instructions&65535)<128&&std::chrono::steady_clock::now()>deadline)throw std::runtime_error("ROM timed out");}
    std::cout<<"RESULT post="<<post<<" halted="<<c.halted<<" instructions="<<c.instructions<<" faults="<<faults<<" location="<<c.location()<<'\n';
    return c.halted&&post==255?0:1;
}
int main(int argc,char **argv) {
    try {if(argc<4)throw std::runtime_error("Usage: x86-external-runner vectors FILE interpreter|jit|both OR rom FILE interpreter|jit OUTPUT");
        const std::string backend=argv[3];
        if(backend!="interpreter"&&backend!="jit"&&backend!="both")throw std::runtime_error("Invalid backend");
        if(std::string(argv[1])=="vectors"&&argc==4)return vectors(argv[2],backend);
        if(std::string(argv[1])=="rom"&&argc==5&&backend!="both")return rom(argv[2],std::string(argv[3])=="jit",argv[4]);
        throw std::runtime_error("Unknown runner mode");
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 2;}
}
