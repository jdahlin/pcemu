#include "arm_hyperv_heartbeat.hpp"
#include <algorithm>
#include <sstream>
namespace pc {
namespace {
uint64_t get(std::span<const uint8_t> b, size_t at, unsigned n) {
    uint64_t v=0; for(unsigned i=0;i<n;++i) v|=uint64_t(b[at+i])<<(8*i); return v;
}
void put(std::span<uint8_t> b, size_t at, uint64_t v, unsigned n) {
    for(unsigned i=0;i<n;++i) b[at+i]=uint8_t(v>>(8*i));
}
}
ArmVmbus::Offer ArmHyperVHeartbeat::offer() {
    ArmVmbus::Offer o{};
    o.channel=channelId; o.connection=connectionId; o.flags=0x11;
    o.interfaceId={0x39,0x4f,0x16,0x57,0x15,0x91,0x78,0x4e,0xab,0x55,0x38,0x2f,0x3b,0xd5,0x42,0x2d};
    // Windows ICTransportVMBusClient filters notifications by the heartbeat
    // instance GUID as well as the interface GUID (they are different).
    o.instanceId={0x91,0x9e,0x14,0xfd,0xe0,0x82,0x7d,0x4a,0xaf,0xa6,0x2a,0x41,0x66,0xcb,0xd7,0xc0};
    o.userData[0]=4; // Message-mode named pipe.
    return o;
}
void ArmHyperVHeartbeat::opened(uint64_t now) {
    state=State::SendVersion; responsive=expired=unresponsive=false; framework=version=0; applicationState=0; since=now;
}
void ArmHyperVHeartbeat::closed() {
    state=State::Closed; responsive=expired=unresponsive=false; framework=version=0; applicationState=0;
}
std::vector<uint8_t> ArmHyperVHeartbeat::packet(uint64_t now) {
    if ((state==State::WaitVersion || state==State::WaitHeartbeat) && !expired && now-since>=timeout) {
        ++timeouts; expired=true; responsive=false; unresponsive=true;
        if(state==State::WaitVersion) state=State::Failed;
        else sequence+=2; // Retire the timed-out transaction before retrying.
    }
    const bool negotiate=state==State::SendVersion;
    if(!negotiate && !(state==State::Ready && now-since>=interval) &&
       !(state==State::WaitHeartbeat && expired)) return {};
    const unsigned body=negotiate ? 24 : 40; // Legacy reserved tail is zero; v3 state is at +8.
    std::vector<uint8_t> p((16+8+20+body+7)&~size_t(7));
    put(p,0,6,2); put(p,2,2,2); put(p,4,p.size()/8,2); // In-band, no completion requested.
    put(p,16,1,4); put(p,20,20+body,4); // Message pipe header.
    put(p,24,negotiate ? 0 : framework,2);
    put(p,28,negotiate ? 0 : 1,2);
    put(p,30,negotiate ? 0 : version,2); put(p,34,body,2); p[41]=3; // IC transaction/request.
    if(negotiate) {
        put(p,44,2,2); put(p,46,2,2);
        put(p,52,1,2); put(p,56,3,2); // Framework 1.0, 3.0.
        put(p,60,1,2); put(p,64,3,2); // Heartbeat 1.0, 3.0.
    } else put(p,44,sequence,8);
    return p;
}
void ArmHyperVHeartbeat::sent(uint64_t now) {
    if(state==State::SendVersion) { state=State::WaitVersion; ++negotiations; }
    else if(state==State::Ready || (state==State::WaitHeartbeat && expired)) { state=State::WaitHeartbeat; ++probes; }
    else return;
    since=now; expired=false;
}
void ArmHyperVHeartbeat::receive(std::span<const uint8_t> p,uint64_t now) {
    auto fail=[&] {++rejected;};
    if(p.size()<16 || get(p,0,2)!=6 || get(p,4,2)*8!=p.size()) return fail();
    const auto offset=get(p,2,2)*8;
    if(offset<16 || offset>p.size() || p.size()-offset<28) return fail();
    auto pipe=p.subspan(size_t(offset));
    auto size=get(pipe,4,4);
    if(get(pipe,0,4)!=1 || size<20 || size>pipe.size()-8) return fail();
    auto h=pipe.subspan(8,size_t(size));
    auto bodySize=get(h,10,2), type=get(h,4,2);
    if(bodySize>h.size()-20 || get(h,12,4) || h[16] || h[17]!=5) return fail();
    auto body=h.subspan(20,size_t(bodySize));
    if(state==State::WaitVersion && type==0) {
        if(body.size()<16 || get(body,0,2)!=1 || get(body,2,2)!=1 ||
           get(body,10,2) || get(body,14,2)) return fail();
        auto fw=get(body,8,2), ver=get(body,12,2);
        if((fw!=1 && fw!=3) || (ver!=1 && ver!=3)) return fail();
        framework=uint16_t(fw); version=uint16_t(ver); state=State::Ready;
        // First probe is immediately due. Negotiation alone never proves liveness.
        since=now-interval; sequence+=2; expired=false; return;
    }
    if(state!=State::WaitHeartbeat || type!=1 || body.size()<8 ||
       get(h,0,2)!=framework || get(h,2,2) || get(h,6,2)!=version || get(h,8,2) ||
       get(body,0,8)!=sequence+1) return fail();
    applicationState=version==3 && body.size()>=12 ? uint32_t(get(body,8,4)) : 0;
    ++responses; responsive=true; expired=unresponsive=false; since=now; sequence+=2; state=State::Ready;
}
std::optional<uint64_t> ArmHyperVHeartbeat::delay(uint64_t now) const {
    if(state==State::SendVersion) return 0;
    if(state==State::Ready) return now-since>=interval ? 0 : interval-(now-since);
    if(state==State::WaitHeartbeat && expired) return 0;
    if((state==State::WaitVersion || state==State::WaitHeartbeat) && !expired)
        return now-since>=timeout ? 0 : timeout-(now-since);
    return {};
}
std::string ArmHyperVHeartbeat::report() const {
    const char *name=state==State::Closed ? "closed" : state==State::SendVersion ? "send-version" :
        state==State::WaitVersion ? "wait-version" : state==State::Ready ? "ready" :
        state==State::Failed ? "failed" : unresponsive ? "unresponsive" : "wait-heartbeat";
    std::ostringstream s;
    s << "{\"state\":\"" << name << "\",\"responsive\":" << (responsive ? "true" : "false")
      << ",\"frameworkMajor\":" << framework << ",\"heartbeatMajor\":" << version
      << ",\"applicationState\":" << applicationState << ",\"negotiations\":" << negotiations
      << ",\"probes\":" << probes << ",\"responses\":" << responses
      << ",\"rejectedResponses\":" << rejected << ",\"timeouts\":" << timeouts << '}';
    return s.str();
}
}
