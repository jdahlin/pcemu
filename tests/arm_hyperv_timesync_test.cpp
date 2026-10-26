#include "arm_hyperv_timesync.hpp"
#include <cstdlib>
#include <iostream>
using namespace pc;
namespace {
unsigned checks=0;
void check(bool v){++checks;if(!v){std::cerr<<"timesync check "<<checks<<" failed\n";std::exit(1);}}
uint64_t get(std::span<const uint8_t> p,size_t at,unsigned n=4){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(p[at+i])<<(8*i);return v;}
void put(std::span<uint8_t> p,size_t at,uint64_t v,unsigned n=4){for(unsigned i=0;i<n;++i)p[at+i]=uint8_t(v>>(8*i));}
bool has(const ArmHyperVTimeSync& t,const std::string& s){return t.report().find(s)!=std::string::npos;}
std::vector<uint8_t> negotiate(ArmHyperVTimeSync& t,unsigned fw=3) {
    t.channel_opened(10);auto p=t.pending_packet(10,0);t.packet_published(10);
    put(p,20,36);put(p,34,16,2);put(p,44,1,2);put(p,46,1,2);
    put(p,52,fw);put(p,56,4);p[41]=5;return p;
}
}
int main(){
    constexpr int64_t epoch=116444736000000000ll;
    check(ArmHyperVTimeSync::unix_100ns_to_filetime(0)==uint64_t(epoch));
    check(ArmHyperVTimeSync::unix_100ns_to_filetime(-epoch)==0);
    check(ArmHyperVTimeSync::unix_100ns_to_filetime(INT64_MAX)==uint64_t(INT64_MAX)+uint64_t(epoch));
    bool rejected=false;try{ArmHyperVTimeSync::unix_100ns_to_filetime(-epoch-1);}catch(const std::invalid_argument&){rejected=true;}check(rejected);
    constexpr uint64_t utc=134373000000000000ull;
    auto offer=ArmHyperVTimeSync::channel_offer();
    const std::array<uint8_t,16> interface={0x30,0xe6,0x27,0x95,0xae,0xd0,0x7b,0x49,0xad,0xce,0xe8,0x0a,0xb0,0x17,0x5c,0xaf};
    const std::array<uint8_t,16> instance={0x17,0xce,0xd1,0x2d,0x9e,0x07,0x3c,0x40,0xb3,0x52,0xa1,0x92,0x1e,0xe2,0x07,0xee};
    check(offer.channel==2&&offer.connection==17&&offer.flags==0x11&&offer.userData[0]==4);
    check(offer.interfaceId==interface&&offer.instanceId==instance);
    for(unsigned fw:{1u,3u}){
        ArmHyperVTimeSync t;check(t.pending_packet(0,utc).empty()&&!t.next_delay_100ns(0));
        t.channel_opened(10);auto n=t.pending_packet(10,utc);
        check(n.size()==64&&get(n,44,2)==2&&get(n,46,2)==1&&get(n,60)==4&&n[41]==3);
        check(t.pending_packet(11,utc)==n); // No publication, no state advancement.
        auto response=negotiate(t,fw);
        for(size_t i=0;i<response.size();++i){auto shortPacket=response;shortPacket.resize(i);t.receive_response(shortPacket,11);check(has(t,"wait-version"));}
        auto unsupported=response;put(unsupported,56,3);t.receive_response(unsupported,11);check(has(t,"wait-version"));
        t.receive_response(response,12);check(t.next_delay_100ns(12)==0);
        auto sync=t.pending_packet(12,utc);check(sync.size()==72&&get(sync,28,2)==4&&get(sync,30,2)==4);
        check(get(sync,44,8)==utc&&get(sync,52,8)==12&&sync[60]==1&&sync[40]==1&&sync[41]==3);
        auto fresh=t.pending_packet(13,utc+1);check(get(fresh,44,8)==utc+1&&get(fresh,52,8)==13);
        t.packet_published(13);check(t.pending_packet(14,utc).empty());
        auto ack=fresh;ack[41]=5;
        auto wrong=ack;wrong[40]++;t.receive_response(wrong,14);check(has(t,"wait-sample"));
        wrong=ack;put(wrong,36,1);t.receive_response(wrong,14);check(has(t,"wait-sample"));
        t.receive_response(ack,14);check(has(t,"\"acknowledgments\":1")&&t.next_delay_100ns(14)==t.SampleInterval100ns);
        t.receive_response(ack,15);check(has(t,"\"acknowledgments\":1")); // Duplicate.
        check(t.pending_packet(14+t.SampleInterval100ns-1,utc).empty());
        auto sample=t.pending_packet(14+t.SampleInterval100ns,utc-100); // Host UTC may step backwards.
        check(sample[60]==2&&sample[40]==2&&get(sample,44,8)==utc-100);
        t.packet_published(14+t.SampleInterval100ns);sample[41]=5;
        t.receive_response(sample,14+t.SampleInterval100ns+t.ResponseTimeout100ns);check(has(t,"failed")&&has(t,"\"timeouts\":1"));
        check(t.pending_packet(14+t.SampleInterval100ns+t.ResponseTimeout100ns+1,utc).empty()&&!t.next_delay_100ns(0));
        t.channel_closed();check(has(t,"closed")&&t.pending_packet(0,utc).empty());
        t.channel_opened(0);check(t.pending_packet(0,utc)[40]==0); // Fresh negotiation after reopen.
    }
    ArmHyperVTimeSync timeout;timeout.channel_opened(0);timeout.packet_published(0);
    check(timeout.next_delay_100ns(timeout.ResponseTimeout100ns)==0);check(timeout.pending_packet(timeout.ResponseTimeout100ns,utc).empty());
    check(!timeout.next_delay_100ns(timeout.ResponseTimeout100ns)&&has(timeout,"\"timeouts\":1"));
    std::cout<<checks<<" timesync checks passed\n";
}
