#include "arm_hyperv.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
using namespace pc;
namespace {
unsigned checks=0;
void check(bool v){++checks;if(!v){std::cerr<<"KVP integration check "<<checks<<" failed\n";std::exit(1);}}
void put(std::span<uint8_t> p,size_t at,uint64_t v,unsigned n=4){for(unsigned i=0;i<n;++i)p[at+i]=uint8_t(v>>(8*i));}
uint64_t get(std::span<const uint8_t> p,size_t at,unsigned n=4){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(p[at+i])<<(8*i);return v;}
}
int main(){
    constexpr uint64_t base=0x140000000;
    uint64_t now=0;
    std::vector<uint8_t> ram(16*4096);
    ArmHyperV hv(true,ram,base,1,[&]{return now;});
    bool rejected=false;try{hv.enableKvp();}catch(const std::runtime_error&){rejected=true;}check(rejected);
    hv.setPrivateInterruptSender([](uint64_t,unsigned){return true;});hv.advertiseSynic(true);hv.enableVmbus();
    hv.enableKvp();hv.enableKvp();
    hv.service();check(!hv.enumerateKvp(ArmHyperVKvpCodec::Pool::Auto,0));
    check(!hv.takeKvpResult());
    auto& synic=hv.synicController();
    check(synic.write_vp_register(0xa0010,0,1)==0&&synic.write_vp_register(0xa0002,0,16)==0);
    check(synic.write_vp_register(0xa0013,0,base+4096+1)==0&&synic.write_vp_register(0xa0012,0,base+8192+1)==0);
    ArmHyperV::Registers r{};
    r[0]=(1ull<<32)|0x10051;r[1]=UINT64_MAX;r[2]=0xfffffffe;r[3]=0x90002;r[5]=0x1000a000066bb;
    check(hv.call(1,r,0)&&r[0]==(1ull<<32));
    auto message=[](unsigned type,size_t size){std::vector<uint8_t> p(size);put(p,0,type);if(size>=12)put(p,8,4);return p;};
    auto post=[&](const std::vector<uint8_t>& p){
        put(ram,4096+512,0);hv.service();put(ram,4096+512,0);
        std::fill(ram.begin(),ram.begin()+256,0);put(ram,0,1);put(ram,8,1);put(ram,12,p.size());
        std::copy(p.begin(),p.end(),ram.begin()+16);r={};r[0]=0x5c;r[1]=base;
        check(hv.call(1,r,0)&&r[0]==0);hv.service();
    };
    auto contact=message(14,40);put(contact,8,0x60000);post(contact);post(message(3,8));
    check(get(ram,4096+512+16+184)==4);
    auto header=message(8,60);put(header,12,42);put(header,16,40,2);put(header,18,1,2);put(header,20,16384);
    for(unsigned i=0;i<4;++i)put(header,28+8*i,(base>>12)+8+i,8);
    post(header);auto open=message(5,148);put(open,12,100);put(open,16,42);put(open,24,2);post(open);
    ArmGpadl guest(ram,base);check(guest.begin(4,42,40,1,std::span(header).subspan(20))==ArmGpadl::Result::Ok);
    ArmVmbusRing tx(guest,4,42,0,8192),rx(guest,4,42,8192,8192);
    bool notify=false;std::vector<uint8_t> request;
    check(rx.pop(request,notify)==ArmVmbusRing::Result::Ok&&get(request,28,2)==0);
    auto answer=[&](std::vector<uint8_t> p){p[41]=5;check(tx.push(p,notify)==ArmVmbusRing::Result::Ok);
        r={};r[0]=0x1005d;r[1]=19;check(hv.call(1,r,0)&&r[0]==0);hv.service();};
    put(request,20,36);put(request,34,16,2);put(request,44,1,2);put(request,46,1,2);put(request,52,3);put(request,56,4);
    answer(request);hv.service();
    check(rx.pop(request,notify)==ArmVmbusRing::Result::Empty);
    check(!hv.serviceDelay100ns());
    std::vector<uint8_t> filler(4080);put(filler,0,6,2);put(filler,2,2,2);put(filler,4,510,2);
    check(rx.push(filler,notify)==ArmVmbusRing::Result::Ok);
    check(hv.enumerateKvp(ArmHyperVKvpCodec::Pool::Auto,7));hv.service();
    check(hv.report().find("\"requests\":0")!=std::string::npos);
    check(hv.serviceDelay100ns()==10000);
    check(rx.pop(request,notify)==ArmVmbusRing::Result::Ok && request==filler);
    hv.service();check(rx.pop(request,notify)==ArmVmbusRing::Result::Ok && get(request,28,2)==2);
    check(request[44]==3 && request[45]==2 && get(request,48)==7);
    check(!hv.enumerateKvp(ArmHyperVKvpCodec::Pool::Auto,8));
    ArmHyperVKvpCodec::Value value;value.text=u"Windows 11";
    auto body=ArmHyperVKvpCodec::request(ArmHyperVKvpCodec::Operation::Set,ArmHyperVKvpCodec::Pool::Auto,0,u"OSName",&value);
    std::copy(body.begin()+4,body.begin()+2576,request.begin()+52);answer(request);
    check(!hv.enumerateKvp(ArmHyperVKvpCodec::Pool::Auto,8));
    auto result=hv.takeKvpResult();
    check(result && result->status==0 && result->index==7 && result->record && result->record->key==u"OSName" && result->record->value.text==u"Windows 11");
    check(!hv.takeKvpResult() && !hv.serviceDelay100ns());
    check(hv.enumerateKvp(ArmHyperVKvpCodec::Pool::Auto,8));hv.service();
    check(rx.pop(request,notify)==ArmVmbusRing::Result::Ok);put(request,36,0x80070103);answer(request);
    result=hv.takeKvpResult();check(result && result->status==0x80070103 && !result->record);
    check(!hv.setKvp(ArmHyperVKvpCodec::Pool::Auto,u"ProbeKey",value));
    check(hv.setKvp(ArmHyperVKvpCodec::Pool::External,u"ProbeKey",value));hv.service();
    check(rx.pop(request,notify)==ArmVmbusRing::Result::Ok && request[44]==1);answer(request);
    result=hv.takeKvpResult();check(result && result->operation==ArmHyperVKvpCodec::Operation::Set && result->status==0);
    check(hv.getKvp(ArmHyperVKvpCodec::Pool::External,u"ProbeKey"));hv.service();
    check(rx.pop(request,notify)==ArmVmbusRing::Result::Ok && request[44]==0);
    body=ArmHyperVKvpCodec::request(ArmHyperVKvpCodec::Operation::Set,ArmHyperVKvpCodec::Pool::External,0,u"ProbeKey",&value);body[0]=0;
    std::copy(body.begin(),body.end(),request.begin()+44);answer(request);
    result=hv.takeKvpResult();check(result && result->operation==ArmHyperVKvpCodec::Operation::Get && result->record && result->record->value.text==value.text);
    check(hv.deleteKvp(ArmHyperVKvpCodec::Pool::External,u"ProbeKey"));hv.service();
    check(rx.pop(request,notify)==ArmVmbusRing::Result::Ok && request[44]==2);answer(request);
    result=hv.takeKvpResult();check(result && result->operation==ArmHyperVKvpCodec::Operation::Delete && result->status==0);
    check(hv.enumerateKvp(ArmHyperVKvpCodec::Pool::Guest,0));hv.service();
    now+=ArmHyperVKvp::timeout;hv.service();check(!hv.enumerateKvp(ArmHyperVKvpCodec::Pool::Auto,0));
    check(hv.report().find("\"timeouts\":1")!=std::string::npos && !hv.serviceDelay100ns());
    post(message(7,12));check(!hv.serviceDelay100ns() && !hv.takeKvpResult());
    check(hv.report().find("\"state\":\"closed\"")!=std::string::npos);
    std::cout<<checks<<" KVP integration checks passed\n";
}
