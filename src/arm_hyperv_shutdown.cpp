#include "arm_hyperv_shutdown.hpp"
#include <sstream>
namespace pc {
namespace {
uint64_t get(std::span<const uint8_t> p,size_t at,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(p[at+i])<<(8*i);return v;}
void put(std::span<uint8_t> p,size_t at,uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)p[at+i]=uint8_t(v>>(8*i));}
}
ArmVmbus::Offer ArmHyperVShutdown::offer(){
    ArmVmbus::Offer o{};o.channel=channelId;o.connection=connectionId;o.flags=0x11;o.userData[0]=4;
    o.interfaceId={0x31,0x60,0x0b,0x0e,0x13,0x52,0x34,0x49,0x81,0x8b,0x38,0xd9,0x0c,0xed,0x39,0xdb};
    o.instanceId={0xf7,0x0f,0x65,0xb6,0xbc,0x33,0x40,0x48,0x80,0x48,0xe0,0x67,0x67,0x86,0xf3,0x93};return o;
}
void ArmHyperVShutdown::opened(uint64_t now){state=State::SendVersion;framework=0;transaction=0;flags=guestTimeout=lastStatus=0;since=now;}
void ArmHyperVShutdown::closed(){state=State::Closed;framework=0;flags=guestTimeout=0;}
bool ArmHyperVShutdown::request(Action action,bool force,uint32_t seconds){
    if(state!=State::Ready)return false;
    uint32_t mode=0;
    switch(action){case Action::PowerOff:break;case Action::Reboot:mode=2;break;case Action::Hibernate:mode=4;break;default:return false;}
    flags=mode|uint32_t(force);guestTimeout=seconds;state=State::SendRequest;return true;
}
std::vector<uint8_t> ArmHyperVShutdown::packet(uint64_t now){
    if((state==State::WaitVersion||state==State::WaitResponse)&&now-since>=responseTimeout){state=State::Failed;++timeouts;}
    const bool negotiate=state==State::SendVersion;
    if(!negotiate&&state!=State::SendRequest)return {};
    const unsigned body=negotiate?20:2060;
    std::vector<uint8_t> p((44+body+7)&~size_t(7));
    put(p,0,6,2);put(p,2,2,2);put(p,4,p.size()/8,2);put(p,16,1,4);put(p,20,20+body,4);
    put(p,24,negotiate?0:framework,2);put(p,28,negotiate?0:3,2);
    put(p,30,negotiate?0:3,2);put(p,32,negotiate?0:2,2);put(p,34,body,2);p[40]=transaction;p[41]=3;
    if(negotiate){put(p,44,2,2);put(p,46,1,2);put(p,52,1,2);put(p,56,3,2);put(p,60,3,2);put(p,62,2,2);}
    else{put(p,44,0x80000000,4);put(p,48,guestTimeout,4);put(p,52,flags,4);}
    return p;
}
void ArmHyperVShutdown::sent(uint64_t now){
    if(state==State::SendVersion){state=State::WaitVersion;++negotiations;}
    else if(state==State::SendRequest){state=State::WaitResponse;++requests;}
    else return;
    since=now;
}
void ArmHyperVShutdown::receive(std::span<const uint8_t> p,uint64_t now){
    auto fail=[&]{++rejected;};
    if((state==State::WaitVersion||state==State::WaitResponse)&&now-since>=responseTimeout){state=State::Failed;++timeouts;}
    if(p.size()<16||get(p,0,2)!=6||get(p,4,2)*8!=p.size())return fail();
    auto offset=get(p,2,2)*8;if(offset<16||offset>p.size()||p.size()-offset<28)return fail();
    auto pipe=p.subspan(size_t(offset));auto size=get(pipe,4,4);
    if(get(pipe,0,4)!=1||size<20||size>pipe.size()-8)return fail();
    auto h=pipe.subspan(8,size_t(size));auto bytes=get(h,10,2);
    if(bytes>h.size()-20||h[16]!=transaction||h[17]!=5)return fail();
    auto body=h.subspan(20,size_t(bytes));
    if(state==State::WaitVersion&&get(h,4,2)==0){
        if(get(h,12,4)||body.size()<16||get(body,0,2)!=1||get(body,2,2)!=1||get(body,10,2)||get(body,12,2)!=3||get(body,14,2)!=2)return fail();
        auto fw=get(body,8,2);if(fw!=1&&fw!=3)return fail();
        framework=uint16_t(fw);++transaction;state=State::Ready;return;
    }
    if(state!=State::WaitResponse||get(h,0,2)!=framework||get(h,2,2)||get(h,4,2)!=3||get(h,6,2)!=3||get(h,8,2)!=2)return fail();
    lastStatus=uint32_t(get(h,12,4));++transaction;
    if(lastStatus){++denied;state=State::Ready;}else{++accepted;state=State::Accepted;}
}
std::optional<uint64_t> ArmHyperVShutdown::delay(uint64_t now) const{
    if(state==State::SendVersion||state==State::SendRequest)return 0;
    if(state==State::WaitVersion||state==State::WaitResponse)return now-since>=responseTimeout?0:responseTimeout-(now-since);
    return {};
}
std::string ArmHyperVShutdown::report() const{
    const char* name=state==State::Closed?"closed":state==State::SendVersion?"send-version":state==State::WaitVersion?"wait-version":state==State::Ready?"ready":state==State::SendRequest?"send-request":state==State::WaitResponse?"wait-response":state==State::Accepted?"accepted":"failed";
    std::ostringstream s;s<<"{\"state\":\""<<name<<"\",\"frameworkMajor\":"<<framework<<",\"shutdownMajor\":"<<(framework?3:0)<<",\"shutdownMinor\":"<<(framework?2:0)
      <<",\"requestFlags\":"<<flags<<",\"guestTimeoutSeconds\":"<<guestTimeout<<",\"negotiations\":"<<negotiations<<",\"requests\":"<<requests<<",\"accepted\":"<<accepted<<",\"denied\":"<<denied<<",\"lastStatus\":"<<lastStatus<<",\"rejectedResponses\":"<<rejected<<",\"timeouts\":"<<timeouts<<"}";return s.str();
}
}
