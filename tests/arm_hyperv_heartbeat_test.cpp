#include "arm_hyperv.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
using namespace pc;
namespace {
unsigned checks=0;
void check(bool ok,std::source_location at=std::source_location::current()) {
    ++checks; if(!ok) {std::cerr << "Heartbeat check " << checks << " line " << at.line() << '\n'; std::exit(1);}
}
void put(std::span<uint8_t> b,size_t at,uint64_t v,unsigned n=4) {
    for(unsigned i=0;i<n;++i) b[at+i]=uint8_t(v>>(8*i));
}
uint64_t get(std::span<const uint8_t> b,size_t at,unsigned n=4) {
    uint64_t v=0; for(unsigned i=0;i<n;++i) v|=uint64_t(b[at+i])<<(8*i); return v;
}
std::vector<uint8_t> negotiation(unsigned fw=3,unsigned ver=3) {
    // Independent guest response: pipe DATA, 20-byte IC header, 16-byte selection.
    std::vector<uint8_t> p(64);
    put(p,0,6,2); put(p,2,2,2); put(p,4,8,2); put(p,16,1); put(p,20,36);
    put(p,34,16,2); p[41]=5; put(p,44,1,2); put(p,46,1,2);
    put(p,52,fw,2); put(p,56,ver,2); return p;
}
std::vector<uint8_t> heartbeatReply(std::vector<uint8_t> p) {
    p[41]=5; put(p,44,get(p,44,8)+1,8); put(p,52,1); return p;
}
bool contains(const ArmHyperVHeartbeat &h,const std::string &s) {return h.report().find(s)!=std::string::npos;}
}
int main() {
    ArmHyperVHeartbeat h;
    check(!h.delay(0) && h.packet(0).empty());
    auto offer=h.offer();
    check(offer.channel==1 && offer.connection==16 && offer.flags==0x11 && offer.userData[0]==4);
    const std::array<uint8_t,16> guid={0x39,0x4f,0x16,0x57,0x15,0x91,0x78,0x4e,0xab,0x55,0x38,0x2f,0x3b,0xd5,0x42,0x2d};
    check(offer.interfaceId==guid);
    h.opened(100); auto request=h.packet(100);
    check(request.size()==72 && get(request,0,2)==6 && get(request,2,2)==2 && get(request,4,2)==9);
    check(get(request,16)==1 && get(request,20)==44 && get(request,34,2)==24 && request[41]==3);
    check(get(request,44,2)==2 && get(request,46,2)==2);
    check(get(request,52)==1 && get(request,56)==3 && get(request,60)==1 && get(request,64)==3);
    check(h.packet(101)==request); // Ring-full retries do not advance the state.
    h.receive(negotiation(),100); check(contains(h,"\"rejectedResponses\":1"));
    h.sent(100); check(h.packet(100).empty() && h.delay(100)==h.timeout);
    auto selected=negotiation();
    auto bad=selected; put(bad,52,9,2); h.receive(bad,101); check(contains(h,"wait-version"));
    h.receive(selected,102); check(h.delay(102)==0 && !contains(h,"\"responsive\":true"));
    request=h.packet(102); check(request.size()==88 && get(request,28,2)==1);
    check(get(request,24,2)==3 && get(request,30,2)==3 && get(request,34,2)==40);
    check(get(request,44,8)==2); h.sent(102);
    auto reply=heartbeatReply(request);
    for(auto offset : {size_t(0),size_t(16),size_t(20),size_t(24),size_t(28),size_t(30),size_t(36),size_t(40),size_t(41),size_t(44)}) {
        bad=reply; bad[offset]^=0x80; h.receive(bad,103); check(contains(h,"wait-heartbeat"));
    }
    // Every truncated prefix must be safely rejected without reporting liveness.
    for(size_t n=0;n<reply.size();++n) {
        h.receive(std::span(reply).first(n),103); check(!contains(h,"\"responsive\":true"));
    }
    for(unsigned n : {0u,7u,65535u}) {
        bad=reply; put(bad,34,n,2); h.receive(bad,103); check(!contains(h,"\"responsive\":true"));
    }
    for(size_t n=16;n<reply.size();n+=8) {
        bad.assign(reply.begin(),reply.begin()+n); put(bad,4,n/8,2);
        h.receive(bad,103); check(!contains(h,"\"responsive\":true"));
    }
    h.receive(reply,104); check(contains(h,"\"responses\":1") && contains(h,"\"responsive\":true"));
    check(contains(h,"\"applicationState\":1") && h.delay(104)==h.interval);
    check(h.packet(104+h.interval-1).empty());
    request=h.packet(104+h.interval); check(get(request,44,8)==4); h.sent(104+h.interval);
    h.receive(reply,105+h.interval); check(contains(h,"wait-heartbeat")); // Stale sequence.
    request=h.packet(104+h.interval+h.timeout); check(!request.empty());
    check(contains(h,"unresponsive") && contains(h,"\"timeouts\":1") && h.delay(104+h.interval+h.timeout)==0);
    h.packet(104+h.interval+h.timeout+1); check(contains(h,"\"timeouts\":1"));
    h.sent(104+h.interval+h.timeout+1);
    h.receive(reply,104+h.interval+h.timeout+2); check(contains(h,"unresponsive"));
    h.receive(heartbeatReply(request),104+h.interval+h.timeout+2);
    check(contains(h,"\"responses\":2") && contains(h,"\"responsive\":true"));
    h.closed(); check(contains(h,"closed") && !h.delay(0));
    h.opened(0); h.packet(0); h.sent(0); h.packet(h.timeout);
    check(contains(h,"failed") && !h.delay(h.timeout));
    h.receive(selected,h.timeout+1); check(contains(h,"failed"));
    // Linux retains the original negotiation pipe length and unused version tail.
    h.opened(0); auto linuxReply=h.packet(0); h.sent(0);
    linuxReply[41]=5; put(linuxReply,34,16,2); put(linuxReply,44,1,2); put(linuxReply,46,1,2);
    put(linuxReply,52,3,2); put(linuxReply,56,3,2);
    h.receive(linuxReply,1); check(h.delay(1)==0);
    // Legacy v1 selection and timer wraparound.
    const uint64_t wrap=UINT64_MAX-100;
    h.opened(wrap); h.packet(wrap); h.sent(wrap); h.receive(negotiation(1,1),wrap+1);
    request=h.packet(wrap+1); h.sent(wrap+1); reply=heartbeatReply(request);
    put(reply,34,8,2); put(reply,20,28); // Only sequence is required in legacy replies.
    h.receive(reply,wrap+2); check(h.delay(wrap+2)==h.interval);
    check(h.delay(wrap+2+h.interval)==0 && contains(h,"\"applicationState\":0"));
    // Full Hyper-V/HVC + SynIC + GPADL + ring fixture with the actual host service.
    constexpr uint64_t base=0x140000000;
    uint64_t now=0;
    std::vector<uint8_t> ram(16*4096);
    ArmHyperV hv(true,ram,base,2,[&] {return now;});
    bool rejected=false; try {hv.enableHeartbeat();} catch(const std::runtime_error &) {rejected=true;}
    check(rejected);
    hv.setPrivateInterruptSender([](uint64_t,unsigned) {return true;});
    hv.advertiseSynic(true); hv.enableVmbus(); hv.enableHeartbeat(); hv.enableHeartbeat();
    ArmHyperV::Registers r{};
    r[0]=(1ull<<32)|0x10051; r[1]=UINT64_MAX; r[2]=0xfffffffe; r[3]=0x90002; r[5]=0x1000a000066bb;
    check(hv.call(1,r,0) && r[0]==(1ull<<32));
    auto &synic=hv.synicController();
    check(synic.write(0xa0010,0,1)==0 && synic.write(0xa0002,0,16)==0);
    check(synic.write(0xa0013,0,base+4096+1)==0 && synic.write(0xa0012,0,base+8192+1)==0);
    auto control=[&](unsigned kind,size_t bytes) {std::vector<uint8_t> p(bytes);put(p,0,kind);if(bytes>=12)put(p,8,1);return p;};
    auto post=[&](const std::vector<uint8_t>& p) {
        put(ram,4096+512,0); hv.service(); put(ram,4096+512,0);
        std::fill(ram.begin(),ram.begin()+256,0);put(ram,0,1);put(ram,8,1);put(ram,12,p.size());
        std::copy(p.begin(),p.end(),ram.begin()+16); r={};r[0]=0x5c;r[1]=base;
        check(hv.call(1,r,0) && r[0]==0);hv.service();
    };
    auto contact=control(14,40);put(contact,8,0x60000);post(contact);post(control(3,8));
    check(get(ram,4096+512+16)==1 && get(ram,4096+512+16+184)==1);
    check(ram[4096+512+16+8]==0x39 && get(ram,4096+512+16+60)==4);
    check(get(ram,4096+512+16+56,2)==0x11);
    auto header=control(8,60);put(header,12,42);put(header,16,40,2);put(header,18,1,2);put(header,20,16384);
    for(unsigned i=0;i<4;++i)put(header,28+8*i,(base>>12)+8+i,8);
    post(header);auto open=control(5,148);put(open,12,100);put(open,16,42);put(open,24,2);post(open);
    ArmGpadl guest(ram,base);check(guest.begin(1,42,40,1,std::span(header).subspan(20))==ArmGpadl::Result::Ok);
    ArmVmbusRing tx(guest,1,42,0,8192),rx(guest,1,42,8192,8192);
    bool notify=false;
    check(rx.pop(request,notify)==ArmVmbusRing::Result::Ok && get(request,28,2)==0);
    auto answer=[&](const std::vector<uint8_t>& p) {
        check(tx.push(p,notify)==ArmVmbusRing::Result::Ok);
        r={};r[0]=0x1005d;r[1]=16;check(hv.call(1,r,0) && r[0]==0);hv.service();
    };
    answer(negotiation());hv.service();
    check(rx.pop(request,notify)==ArmVmbusRing::Result::Ok && get(request,28,2)==1);
    check(hv.report().find("\"responsive\":false")!=std::string::npos);
    now=100;answer(heartbeatReply(request));
    check(hv.report().find("\"responsive\":true")!=std::string::npos && hv.serviceDelay100ns()==h.interval);
    // Full host-to-guest ring: retain the same candidate, do not count an unsent probe.
    std::vector<uint8_t> filler(4080); put(filler,0,6,2); put(filler,2,2,2); put(filler,4,510,2);
    check(rx.push(filler,notify)==ArmVmbusRing::Result::Ok);
    now+=h.interval;hv.service();
    check(hv.report().find("\"probes\":1")!=std::string::npos && hv.serviceDelay100ns()==10000);
    check(rx.pop(reply,notify)==ArmVmbusRing::Result::Ok && reply==filler);
    hv.service();check(rx.pop(request,notify)==ArmVmbusRing::Result::Ok && get(request,44,8)==4);
    check(hv.report().find("\"probes\":2")!=std::string::npos);
    now+=h.timeout;hv.service();check(hv.report().find("unresponsive")!=std::string::npos);
    check(rx.pop(request,notify)==ArmVmbusRing::Result::Ok);
    answer(heartbeatReply(request));check(hv.report().find("\"responses\":2")!=std::string::npos);
    post(control(7,12));check(hv.report().find("\"state\":\"closed\"")!=std::string::npos);
    check(!hv.serviceDelay100ns());
    auto stale=heartbeatReply(request);
    post(open);check(rx.pop(request,notify)==ArmVmbusRing::Result::Ok && get(request,28,2)==0);
    answer(negotiation());hv.service();
    check(rx.pop(request,notify)==ArmVmbusRing::Result::Ok && get(request,28,2)==1);
    answer(stale);check(hv.report().find("\"responsive\":false")!=std::string::npos);
    answer(heartbeatReply(request));check(hv.report().find("\"responses\":3")!=std::string::npos);
    post(control(16,8));
    check(hv.report().find("\"state\":\"closed\"")!=std::string::npos && !hv.serviceDelay100ns());
    check(hv.report().find("\"gpadls\":0")!=std::string::npos);
    std::cout << checks << " heartbeat checks passed\n";
}
