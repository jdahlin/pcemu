#include "arm_hyperv.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
using namespace pc;
namespace {
unsigned checks=0;
void check(bool v){++checks;if(!v){std::cerr<<"shutdown integration check "<<checks<<" failed\n";std::exit(1);}}
void put(std::span<uint8_t> p,size_t at,uint64_t v,unsigned n=4){for(unsigned i=0;i<n;++i)p[at+i]=uint8_t(v>>(8*i));}
uint64_t get(std::span<const uint8_t> p,size_t at,unsigned n=4){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(p[at+i])<<(8*i);return v;}
}
int main(){
    using Action=ArmHyperVShutdown::Action;
    constexpr uint64_t base=0x140000000;
    uint64_t now=0;
    std::vector<uint8_t> ram(16*4096);
    ArmHyperV hv(true,ram,base,1,[&]{return now;});
    check(!hv.requestShutdown(Action::PowerOff));
    bool rejected=false;try{hv.enableShutdown();}catch(const std::runtime_error&){rejected=true;}check(rejected);
    hv.setPrivateInterruptSender([](uint64_t,unsigned){return true;});hv.advertiseSynic(true);hv.enableVmbus();hv.enableShutdown();hv.enableShutdown();
    check(!hv.requestShutdown(Action::PowerOff));
    auto& synic=hv.synicController();
    check(synic.write_vp_register(0xa0010,0,1)==0&&synic.write_vp_register(0xa0002,0,16)==0);
    check(synic.write_vp_register(0xa0013,0,base+4096+1)==0&&synic.write_vp_register(0xa0012,0,base+8192+1)==0);
    ArmHyperV::Registers r{};
    r[0]=(1ull<<32)|0x10051;r[1]=UINT64_MAX;r[2]=0xfffffffe;r[3]=0x90002;r[5]=0x1000a000066bb;
    check(hv.call(1,r,0)&&r[0]==(1ull<<32));
    r[0]=(1ull<<32)|0x10051;r[1]=UINT64_MAX;r[2]=0xfffffffe;r[3]=0x90002;r[5]=0x1000a000066bb;
    check(hv.call(1,r,0)&&r[0]==(1ull<<32));
    auto message=[](unsigned type,size_t size){std::vector<uint8_t> p(size);put(p,0,type);if(size>=12)put(p,8,3);return p;};
    auto post=[&](const std::vector<uint8_t>& p){
        put(ram,4096+512,0);hv.service();put(ram,4096+512,0);
        std::fill(ram.begin(),ram.begin()+256,0);put(ram,0,1);put(ram,8,1);put(ram,12,p.size());
        std::copy(p.begin(),p.end(),ram.begin()+16);r={};r[0]=0x5c;r[1]=base;
        check(hv.call(1,r,0)&&r[0]==0);hv.service();
    };
    auto contact=message(14,40);put(contact,8,0x60000);post(contact);post(message(3,8));
    check(get(ram,4096+512+16+184)==3);
    auto header=message(8,60);put(header,12,42);put(header,16,40,2);put(header,18,1,2);put(header,20,16384);
    for(unsigned i=0;i<4;++i)put(header,28+8*i,(base>>12)+8+i,8);
    post(header);auto open=message(5,148);put(open,12,100);put(open,16,42);put(open,24,2);post(open);
    ArmGpadl guest(ram,base);check(guest.begin(3,42,40,1,std::span(header).subspan(20))==ArmGpadl::Result::Ok);
    ArmVmbusRing tx(guest,3,42,0,8192),rx(guest,3,42,8192,8192);
    bool notify=false;std::vector<uint8_t> request;
    check(rx.pop(request,notify)==ArmVmbusRing::Result::Ok&&get(request,28,2)==0);
    auto answer=[&](std::vector<uint8_t> p){p[41]=5;check(tx.push(p,notify)==ArmVmbusRing::Result::Ok);
        r={};r[0]=0x1005d;r[1]=18;check(hv.call(1,r,0)&&r[0]==0);hv.service();};
    check(!hv.requestShutdown(Action::PowerOff)); // Negotiation is not complete.
    put(request,20,36);put(request,34,16,2);put(request,44,1,2);put(request,46,1,2);
    put(request,52,3);put(request,56,3,2);put(request,58,2,2);
    answer(request);hv.service();
    check(rx.pop(request,notify)==ArmVmbusRing::Result::Empty); // No implicit operation.
    check(!hv.serviceDelay100ns());
    std::vector<uint8_t> filler(4080);put(filler,0,6,2);put(filler,2,2,2);put(filler,4,510,2);
    check(rx.push(filler,notify)==ArmVmbusRing::Result::Ok);
    check(hv.requestShutdown(Action::PowerOff));check(!hv.requestShutdown(Action::Reboot));hv.service();
    check(hv.report().find("\"requests\":0")!=std::string::npos&&hv.serviceDelay100ns()==10000);
    check(rx.pop(request,notify)==ArmVmbusRing::Result::Ok&&request==filler);hv.service();
    check(rx.pop(request,notify)==ArmVmbusRing::Result::Ok&&request.size()==2104&&get(request,52)==0);
    check(hv.report().find("\"requests\":1")!=std::string::npos);
    auto denied=request;put(denied,36,0x80004005);answer(denied);
    check(hv.report().find("\"denied\":1")!=std::string::npos);
    check(hv.requestShutdown(Action::Reboot));hv.service();
    check(rx.pop(request,notify)==ArmVmbusRing::Result::Ok&&get(request,52)==2);
    answer(request);check(hv.report().find("\"accepted\":1")!=std::string::npos);
    check(!hv.requestShutdown(Action::PowerOff));hv.service();
    check(rx.pop(request,notify)==ArmVmbusRing::Result::Empty);
    post(message(7,12));check(!hv.requestShutdown(Action::PowerOff)&&!hv.serviceDelay100ns());
    std::cout<<checks<<" shutdown integration checks passed\n";
}
