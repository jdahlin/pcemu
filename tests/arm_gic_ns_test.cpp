#include "arm_gic_ns.hpp"
#include <cstdlib>
#include <iostream>
#include <map>
#include <source_location>
#include <stdexcept>
using namespace pc;
namespace {
unsigned checks=0;
void check(bool value,std::source_location at=std::source_location::current()) {
    ++checks;if(!value){std::cerr<<"GIC view check "<<checks<<" line "<<at.line()<<'\n';std::exit(1);}
}
template<class F> void invalid(F fn) {bool rejected=false;try{fn();}catch(const std::invalid_argument&){rejected=true;}check(rejected);}
}
int main() {
    std::map<uint32_t,uint64_t> words{{0,0x50},{4,31},{8,0x1234},{0xffe8,0x30}};
    unsigned reads=0,writes=0;uint32_t lastReg=0;uint64_t lastValue=0;
    auto read=[&](uint32_t at) {
        ++reads; if(at>=0x100&&at<0x400) at&=~0x80u;
        return words[at];
    };
    auto write=[&](uint32_t at,uint64_t value) {
        ++writes;lastReg=at;lastValue=value;
        if(at>=0x100&&at<0x400) {
            auto &word=words[at&~0x80u];if(at&0x80)word&=~value;else word|=value;
        } else words[at]=value;
    };
    invalid([&]{ArmGicNonsecureView missing({},write);});
    ArmGicNonsecureView view(read,write);
    check(view.interrupt_count()==1020);
    check(view.read_register(4,4)==0x41f && view.read_register(0,4)==0x10);
    check(view.read_register(8,4)==0x1234 && view.read_register(0xffe8,4)==0x30);
    view.write_register(0,UINT64_MAX,4);check(words[0]==0x52 && view.read_register(0,4)==0x12);
    view.write_register(0,1,4);check(words[0]==0x50); // Group0/DS cannot be enabled or changed.
    auto beforeWrites=writes,beforeReads=reads;
    for(unsigned at : {0x80u,0xfcu,0x800u,0xc00u,0xc04u,0xd00u,0xe00u,0xf00u,0x6000u,0x60f8u,0x7fe0u}) {
        check(view.read_register(at,4)==0);view.write_register(at,UINT64_MAX,4);
    }
    check(writes==beforeWrites && reads==beforeReads);
    for(unsigned at : {0u,4u,8u,0x104u,0xc08u,0xffe8u}) {
        check(view.read_register(at,2)==0);view.write_register(at,UINT64_MAX,2);
    }
    check(writes==beforeWrites && reads==beforeReads);
    view.write_register(0x104,0x10000,4);view.write_register(0x104,2,4);
    check(view.read_register(0x104,4)==0x10002);
    beforeReads=reads;view.write_register(0x184,2,4);
    check(reads==beforeReads && lastReg==0x184 && lastValue==2);
    check(view.read_register(0x104,4)==0x10000);
    for(unsigned base : {0x200u,0x300u}) {
        view.write_register(base+4,0xa5,4);view.write_register(base+0x84,0x81,4);
        check(view.read_register(base+4,4)==0x24);
    }
    for(unsigned base : {0x100u,0x180u,0x200u,0x280u,0x300u,0x380u}) {
        beforeWrites=writes;check(view.read_register(base,4)==0);view.write_register(base,UINT64_MAX,4);
        check(writes==beforeWrites);
        view.write_register(base+0x7c,UINT64_MAX,4);check(lastValue==0x0fffffff);
    }
    words[0x420]=0x11223344;
    check(view.read_register(0x420,1)==0x44 && view.read_register(0x423,1)==0x11);
    view.write_register(0x421,0x1aa,1);check(words[0x420]==0x1122aa44);
    view.write_register(0x420,0x9988776655443322ull,4);check(view.read_register(0x420,4)==0x55443322);
    check(view.read_register(0x7fb,1)==0 && view.read_register(0x7fc,1)==0);
    view.write_register(0xcfc,UINT64_MAX,4);check(lastReg==0xcfc && lastValue==0xffffff);
    words[0xcfc]=UINT32_MAX;check(view.read_register(0xcfc,4)==0xffffff);
    view.write_register(0x6180,0x123456789abcdef0ull,8);
    check(view.read_register(0x6180,4)==0x9abcdef0 && view.read_register(0x6184,4)==0x12345678);
    view.write_register(0x6184,0x88776655,4);check(view.read_register(0x6180,8)==0x887766559abcdef0ull);
    view.write_register(0x6180,0xffeeddcc,4);check(view.read_register(0x6180,8)==0x88776655ffeeddccull);
    beforeWrites=writes;check(view.read_register(0x7fe0,8)==0);view.write_register(0x7fe0,UINT64_MAX,8);check(writes==beforeWrites);
    // Every implemented SPI retains independent bitmap state and route halves.
    for(unsigned irq=32;irq<1020;++irq) {
        auto offset=0x100+4*(irq/32),bit=1u<<(irq%32);
        view.write_register(offset,bit,4);check(view.read_register(offset,4)&bit);
        view.write_register(offset+0x80,bit,4);check(!(view.read_register(offset,4)&bit));
        auto route=0x6000+8*irq;view.write_register(route,irq,4);view.write_register(route+4,irq^0x55,4);
        check(view.read_register(route,8)==(uint64_t(irq^0x55)<<32|irq));
    }
    // No backend access may occur for malformed transactions.
    beforeReads=reads;beforeWrites=writes;
    for(auto width : {0u,3u,16u}) invalid([&]{view.read_register(0,width);});
    for(auto offset : {1ull,3ull,0xffffull,0x10000ull,UINT64_MAX}) {
        invalid([&]{view.read_register(offset,4);});invalid([&]{view.write_register(offset,1,4);});
    }
    check(reads==beforeReads && writes==beforeWrites);
    // Backend failures are surfaced, never silently converted to guest success.
    bool fail=false;
    ArmGicNonsecureView failing([&](uint32_t at)->uint64_t {
        if(fail)throw std::runtime_error("native read failed");return at==4 ? 31 : 0x50;
    },[](uint32_t,uint64_t){throw std::runtime_error("native write failed");});
    bool caught=false;try{failing.write_register(0x104,1,4);}catch(const std::runtime_error&){caught=true;}check(caught);
    fail=true;caught=false;try{failing.read_register(0x104,4);}catch(const std::runtime_error&){caught=true;}check(caught);
    for(auto type : {0x400u,0x41fu}) {
        caught=false;try{ArmGicNonsecureView bad([&](uint32_t at){return at==4 ? type : 0x50u;},write);}
        catch(const std::runtime_error&){caught=true;}check(caught);
    }
    words[4]=1;ArmGicNonsecureView small(read,write);check(small.interrupt_count()==64);
    beforeWrites=writes;check(small.read_register(0x108,4)==0);small.write_register(0x108,1,4);check(writes==beforeWrites);
    std::cout<<checks<<" GIC nonsecure-view checks passed\n";
}
