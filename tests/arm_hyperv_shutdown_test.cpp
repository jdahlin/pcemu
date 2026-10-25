#include "arm_hyperv_shutdown.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
using namespace pc;
namespace {
unsigned checks=0;
void check(bool v){++checks;if(!v){std::cerr<<"shutdown check "<<checks<<" failed\n";std::exit(1);}}
uint64_t get(std::span<const uint8_t> p,size_t at,unsigned n=4){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(p[at+i])<<(8*i);return v;}
void put(std::span<uint8_t> p,size_t at,uint64_t v,unsigned n=4){for(unsigned i=0;i<n;++i)p[at+i]=uint8_t(v>>(8*i));}
bool has(const ArmHyperVShutdown& s,const std::string& text){return s.report().find(text)!=std::string::npos;}
std::vector<uint8_t> negotiate(ArmHyperVShutdown& s,unsigned fw){
    s.opened(10);auto p=s.packet(10);s.sent(10);
    put(p,20,36);put(p,34,16,2);put(p,44,1,2);put(p,46,1,2);put(p,52,fw);put(p,56,3,2);put(p,58,2,2);p[41]=5;return p;
}
}
int main(){
    using Action=ArmHyperVShutdown::Action;
    const auto offer=ArmHyperVShutdown::offer();
    const std::array<uint8_t,16> interface={0x31,0x60,0x0b,0x0e,0x13,0x52,0x34,0x49,0x81,0x8b,0x38,0xd9,0x0c,0xed,0x39,0xdb};
    const std::array<uint8_t,16> instance={0xf7,0x0f,0x65,0xb6,0xbc,0x33,0x40,0x48,0x80,0x48,0xe0,0x67,0x67,0x86,0xf3,0x93};
    check(offer.channel==3&&offer.connection==18&&offer.flags==0x11&&offer.userData[0]==4);
    check(offer.interfaceId==interface&&offer.instanceId==instance);
    for(unsigned fw:{1u,3u}){
        ArmHyperVShutdown s;check(!s.request(Action::PowerOff)&&s.packet(0).empty()&&!s.delay(0));
        auto version=negotiate(s,fw);check(version.size()==64);
        for(size_t n=0;n<version.size();++n){auto cut=version;cut.resize(n);s.receive(cut,11);check(has(s,"wait-version"));}
        auto wrong=version;put(wrong,58,1,2);s.receive(wrong,11);check(has(s,"wait-version"));
        s.receive(version,12);check(has(s,"ready")&&s.packet(12).empty()&&!s.delay(12));
        check(!s.request(static_cast<Action>(99)));
        check(s.request(Action::PowerOff));check(!s.request(Action::Reboot));
        auto request=s.packet(13);check(request.size()==2104&&get(request,34,2)==2060&&get(request,28,2)==3);
        check(get(request,30,2)==3&&get(request,32,2)==2&&request[40]==1&&request[41]==3);
        check(get(request,44)==0x80000000&&get(request,48)==0&&get(request,52)==0);
        check(std::all_of(request.begin()+56,request.end(),[](auto b){return b==0;}));
        check(s.packet(14)==request&&has(s,"\"requests\":0")); // Backpressure cannot publish twice.
        s.sent(14);check(s.packet(15).empty()&&has(s,"\"requests\":1"));
        auto ack=request;ack[41]=5;wrong=ack;wrong[40]++;s.receive(wrong,15);check(has(s,"wait-response"));
        for(size_t n=0;n<44;++n){auto cut=ack;cut.resize(n);s.receive(cut,15);check(has(s,"wait-response"));}
        put(ack,36,0x80004005);s.receive(ack,16);check(has(s,"ready")&&has(s,"\"denied\":1"));
        check(s.request(Action::Reboot,true,60));request=s.packet(17);
        check(get(request,48)==60&&get(request,52)==3&&request[40]==2);
        s.sent(17);ack=request;ack[41]=5;s.receive(ack,18);check(has(s,"accepted")&&has(s,"\"accepted\":1"));
        check(!s.request(Action::PowerOff)&&s.packet(19).empty()&&!s.delay(19));
        s.receive(ack,19);check(has(s,"\"accepted\":1"));
        s.closed();check(has(s,"closed")&&!s.request(Action::PowerOff));
        version=negotiate(s,fw);s.receive(version,12);check(s.request(Action::Hibernate));
        request=s.packet(13);check(get(request,52)==4);s.sent(13);
        ack=request;ack[41]=5;s.receive(ack,13+s.responseTimeout);
        check(has(s,"failed")&&has(s,"\"timeouts\":1")&&s.packet(14+s.responseTimeout).empty());
        check(!s.request(Action::Hibernate)); // An uncertain power operation is never retried automatically.
    }
    ArmHyperVShutdown timeout;timeout.opened(0);timeout.sent(0);
    check(timeout.delay(timeout.responseTimeout)==0&&timeout.packet(timeout.responseTimeout).empty());
    check(!timeout.delay(0)&&has(timeout,"\"timeouts\":1"));
    std::cout<<checks<<" shutdown checks passed\n";
}
