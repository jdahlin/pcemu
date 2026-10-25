#include "arm_hyperv_timesync.hpp"
#include <sstream>
#include <stdexcept>
namespace pc {
namespace {
uint64_t get(std::span<const uint8_t> p,size_t at,unsigned n) {
    uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(p[at+i])<<(8*i);return v;
}
void put(std::span<uint8_t> p,size_t at,uint64_t v,unsigned n) {
    for(unsigned i=0;i<n;++i)p[at+i]=uint8_t(v>>(8*i));
}
}
uint64_t ArmHyperVTimeSync::utcFromUnix100ns(int64_t ticks) {
    constexpr int64_t epoch=116444736000000000ll;
    if(ticks < -epoch) throw std::invalid_argument("UTC precedes the FILETIME epoch");
    return ticks<0 ? uint64_t(epoch)-uint64_t(-ticks) : uint64_t(epoch)+uint64_t(ticks);
}
ArmVmbus::Offer ArmHyperVTimeSync::offer() {
    ArmVmbus::Offer o{};o.channel=channelId;o.connection=connectionId;o.flags=0x11;o.userData[0]=4;
    o.interfaceId={0x30,0xe6,0x27,0x95,0xae,0xd0,0x7b,0x49,0xad,0xce,0xe8,0x0a,0xb0,0x17,0x5c,0xaf};
    o.instanceId={0x17,0xce,0xd1,0x2d,0x9e,0x07,0x3c,0x40,0xb3,0x52,0xa1,0x92,0x1e,0xe2,0x07,0xee};
    return o;
}
void ArmHyperVTimeSync::opened(uint64_t now) {
    state=State::SendVersion;framework=0;transaction=0;initial=true;since=now;
}
void ArmHyperVTimeSync::closed() {state=State::Closed;framework=0;}
std::vector<uint8_t> ArmHyperVTimeSync::packet(uint64_t now,uint64_t utc) {
    if((state==State::WaitVersion || state==State::WaitSample) && now-since>=timeout) {
        state=State::Failed;++timeouts;
    }
    bool negotiate=state==State::SendVersion;
    if(!negotiate && !(state==State::Ready && (initial || now-since>=interval)))return {};
    unsigned body=negotiate?20:24;
    std::vector<uint8_t> p((44+body+7)&~size_t(7));
    put(p,0,6,2);put(p,2,2,2);put(p,4,p.size()/8,2);
    put(p,16,1,4);put(p,20,20+body,4);
    put(p,24,negotiate?0:framework,2);put(p,28,negotiate?0:4,2);
    put(p,30,negotiate?0:4,2);put(p,34,body,2);p[40]=transaction;p[41]=3;
    if(negotiate) {
        put(p,44,2,2);put(p,46,1,2);put(p,52,1,2);put(p,56,3,2);put(p,60,4,2);
    } else {
        put(p,44,utc,8);put(p,52,now,8);p[60]=initial?1:2;
        // NTP leap indicator and stratum remain unspecified (zero).
    }
    return p;
}
void ArmHyperVTimeSync::sent(uint64_t now) {
    if(state==State::SendVersion){state=State::WaitVersion;++negotiations;}
    else if(state==State::Ready && (initial || now-since>=interval)){state=State::WaitSample;++samples;}
    else return;
    since=now;
}
void ArmHyperVTimeSync::receive(std::span<const uint8_t> p,uint64_t now) {
    auto fail=[&]{++rejected;};
    if((state==State::WaitVersion || state==State::WaitSample) && now-since>=timeout) {
        state=State::Failed;++timeouts;
    }
    if(p.size()<16 || get(p,0,2)!=6 || get(p,4,2)*8!=p.size())return fail();
    auto offset=get(p,2,2)*8;
    if(offset<16 || offset>p.size() || p.size()-offset<28)return fail();
    auto pipe=p.subspan(size_t(offset));auto size=get(pipe,4,4);
    if(get(pipe,0,4)!=1 || size<20 || size>pipe.size()-8)return fail();
    auto h=pipe.subspan(8,size_t(size));auto bodySize=get(h,10,2);
    if(bodySize>h.size()-20 || get(h,12,4) || h[16]!=transaction || h[17]!=5)return fail();
    auto body=h.subspan(20,size_t(bodySize));
    if(state==State::WaitVersion && get(h,4,2)==0) {
        if(body.size()<16 || get(body,0,2)!=1 || get(body,2,2)!=1 ||
           get(body,10,2) || get(body,12,2)!=4 || get(body,14,2))return fail();
        auto fw=get(body,8,2);if(fw!=1 && fw!=3)return fail();
        framework=uint16_t(fw);state=State::Ready;++transaction;since=now;return;
    }
    if(state!=State::WaitSample || now-since>=timeout || get(h,4,2)!=4 ||
       get(h,0,2)!=framework || get(h,2,2) || get(h,6,2)!=4 || get(h,8,2))return fail();
    ++acks;++transaction;initial=false;since=now;state=State::Ready;
}
std::optional<uint64_t> ArmHyperVTimeSync::delay(uint64_t now) const {
    if(state==State::SendVersion || (state==State::Ready && initial))return 0;
    if(state==State::Ready)return now-since>=interval?0:interval-(now-since);
    if(state==State::WaitVersion || state==State::WaitSample)return now-since>=timeout?0:timeout-(now-since);
    return {};
}
std::string ArmHyperVTimeSync::report() const {
    const char* name=state==State::Closed?"closed":state==State::SendVersion?"send-version":
        state==State::WaitVersion?"wait-version":state==State::Ready?"ready":state==State::WaitSample?"wait-sample":"failed";
    std::ostringstream s;s<<"{\"state\":\""<<name<<"\",\"frameworkMajor\":"<<framework
      <<",\"timesyncMajor\":"<<(framework?4:0)<<",\"negotiations\":"<<negotiations
      <<",\"samples\":"<<samples<<",\"acknowledgments\":"<<acks
      <<",\"rejectedResponses\":"<<rejected<<",\"timeouts\":"<<timeouts<<"}";return s.str();
}
}
