#include "arm_vmbus.hpp"
#include "arm_hyperv.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
using namespace pc;
namespace {
unsigned checks = 0;
void check(bool ok) { ++checks; if (!ok) { std::cerr << "channel check " << checks << " failed\n"; std::exit(1); } }
void put(std::span<uint8_t> b, size_t at, uint64_t v, unsigned n=4) {
    for (unsigned i=0; i<n; ++i) b[at+i]=uint8_t(v>>(8*i));
}
uint64_t get(std::span<const uint8_t> b, size_t at, unsigned n=4) {
    uint64_t v=0; for (unsigned i=0; i<n; ++i) v|=uint64_t(b[at+i])<<(8*i); return v;
}
std::vector<uint8_t> msg(unsigned kind, size_t bytes=8, unsigned channel=7) {
    std::vector<uint8_t> v(bytes); put(v,0,kind); if(bytes>=12) put(v,8,channel); return v;
}
}
int main() {
    constexpr uint64_t base=0x140000000;
    std::vector<uint8_t> ram(16*4096);
    ArmVmbus bus(ram,base,2);
    std::vector<std::vector<uint8_t>> replies;
    bool block=false, eventFail=false;
    unsigned events=0, calls=0, closes=0;
    bus.setSender([&](unsigned vp,unsigned sint,std::span<const uint8_t> p) {
        check(vp==0 && sint==2); if(block) return uint16_t(0x13);
        replies.emplace_back(p.begin(),p.end()); return uint16_t(0);
    });
    ArmVmbus::Offer offer{};
    offer.channel=7; offer.connection=16; offer.interfaceId[0]=1; offer.instanceId[0]=2;
    auto echo=[&](std::span<const uint8_t> p) { ++calls; return std::vector<uint8_t>(p.begin(),p.end()); };
    check(!bus.addChannel(offer,echo)); // Notification delivery must exist.
    bus.setEventSender([&](unsigned vp,unsigned sint,unsigned flag) {
        check(vp==1 && sint==2 && flag==7); ++events;
        return uint16_t(eventFail ? 0x71 : 0);
    });
    check(bus.addChannel(offer,echo,[&] {++closes;}));
    check(!bus.addChannel(offer,echo));
    auto second=offer; second.channel=8; second.connection=17;
    check(!bus.addChannel(second,echo)); // Same GUID pair.
    second.instanceId[0]=3; check(bus.addChannel(second,echo));
    auto contact=msg(14,40); put(contact,8,0x60000);
    check(bus.receive(1,contact)==0); bus.service();
    check(replies.size()==1 && replies[0].size()==20 && replies[0][8]==1);
    check(!bus.addChannel(offer,echo));
    auto header=msg(8,60); put(header,12,42); put(header,16,40,2); put(header,18,1,2);
    put(header,20,16384); // Byte count and zero offset, then four noncontiguous PFNs.
    const unsigned pages[]={8,3,10,5};
    for(unsigned i=0;i<4;++i) put(header,28+8*i,(base>>12)+pages[i],8);
    check(bus.receive(1,header)==5); // Cannot use an unoffered channel.
    check(bus.receive(1,msg(3))==0); bus.service();
    check(replies.size()==4 && replies[1].size()==196);
    check(get(replies[1],0)==1 && get(replies[1],184)==7 && get(replies[1],190,2)==1);
    check(get(replies[1],192)==16 && replies[1][8]==1 && replies[1][24]==2);
    check(get(replies[2],184)==8 && get(replies[3],0)==4);
    auto fragment=header; fragment.resize(36);
    check(bus.receive(1,fragment)==0); bus.service(); check(replies.size()==4);
    auto body=msg(9,40); put(body,8,0xaabbccdd); put(body,12,42);
    std::copy(header.begin()+36,header.end(),body.begin()+16);
    check(bus.receive(1,body)==0); bus.service();
    check(get(replies.back(),0)==10 && get(replies.back(),12)==42 && get(replies.back(),16)==0);
    check(bus.receive(1,body)==0x15);
    auto open=msg(5,148); put(open,12,100); put(open,16,42); put(open,20,2); put(open,24,2);
    check(bus.receive(1,open)==0); bus.service(); check(get(replies.back(),16)==0xc000000d);
    put(open,20,1); put(open,24,1);
    check(bus.receive(1,open)==0); bus.service(); check(get(replies.back(),16)==0xc000000d);
    put(open,24,2); check(bus.receive(1,open)==0); bus.service();
    check(get(replies.back(),0)==6 && get(replies.back(),12)==100 && get(replies.back(),16)==0);
    check(bus.receive(1,open)==0); bus.service(); check(get(replies.back(),16)==0xc000000d);
    ArmGpadl guest(ram,base);
    check(guest.begin(7,42,40,1,std::span(header).subspan(20))==ArmGpadl::Result::Ok);
    ArmVmbusRing outbound(guest,7,42,0,8192), inbound(guest,7,42,8192,8192);
    auto packet=msg(6,24); put(packet,0,6,2); put(packet,2,2,2); put(packet,4,3,2);
    put(packet,8,123,8); put(packet,16,0x12345678,8);
    bool notify=false;
    eventFail=true;
    check(outbound.push(packet,notify)==ArmVmbusRing::Result::Ok && notify);
    check(bus.signalChannel(7)==0); bus.service();
    check(calls==1 && events==1 && bus.needsService());
    std::vector<uint8_t> received;
    check(inbound.pop(received,notify)==ArmVmbusRing::Result::Ok && received==packet);
    eventFail=false; bus.service(); check(calls==1 && events==2 && !bus.needsService());
    // One service call may consume at most sixteen packets from each channel.
    for(unsigned i=0;i<40;++i) {
        put(packet,8,i,8); check(outbound.push(packet,notify)==ArmVmbusRing::Result::Ok);
    }
    check(bus.signalChannel(7)==0); bus.service(); check(calls==17 && bus.needsService());
    bus.service(); check(calls==33 && bus.needsService()); bus.service(); check(calls==41);
    for(unsigned i=0;i<40;++i) {
        check(inbound.pop(received,notify)==ArmVmbusRing::Result::Ok && get(received,8,8)==i);
    }
    check(inbound.pop(received,notify)==ArmVmbusRing::Result::Empty);
    // A full response ring must retain one response and stop reading further requests.
    auto large=packet; large.resize(4080); put(large,4,510,2);
    for(unsigned i=0;i<3;++i) {
        put(large,8,200+i,8);
        check(outbound.push(large,notify)==ArmVmbusRing::Result::Ok);
        check(bus.signalChannel(7)==0); bus.service();
    }
    check(calls==43); // First response queued, second retained, third still in RX.
    for(unsigned i=0;i<3;++i) {
        check(inbound.pop(received,notify)==ArmVmbusRing::Result::Ok && get(received,8,8)==200+i);
        bus.service();
    }
    check(calls==44 && !bus.needsService());
    auto teardown=msg(11,16); put(teardown,12,42);
    check(bus.receive(1,teardown)==0x15);
    auto wrong=teardown; put(wrong,8,8); check(bus.receive(1,wrong)==5);
    check(bus.receive(1,msg(7,12))==0 && closes==1);
    check(bus.receive(1,msg(7,12))==0 && closes==1);
    check(bus.signalChannel(7)==0x15);
    check(bus.receive(1,teardown)==0); bus.service();
    check(replies.back().size()==12 && get(replies.back(),0)==12 && get(replies.back(),8)==42);
    check(bus.receive(1,teardown)==0); bus.service(); // Idempotent unknown teardown.
    check(bus.receive(1,open)==0); bus.service(); check(get(replies.back(),16)==0xc000000d);
    // Reused mappings cannot retain old packets: guest initializes both ring headers.
    std::fill(ram.begin(),ram.end(),0);
    check(bus.receive(1,header)==0); bus.service();
    check(bus.receive(1,open)==0); bus.service(); check(get(replies.back(),16)==0);
    // Invalid ring indices close the channel without calling its backend.
    put(ram,8*4096,4096); check(bus.signalChannel(7)==0); bus.service();
    check(closes==2 && calls==44 && bus.report().find("\"channelFaults\":1")!=std::string::npos);
    check(bus.receive(1,teardown)==0); bus.service();
    std::fill(ram.begin(),ram.end(),0);
    check(bus.receive(1,header)==0); bus.service();
    auto silentOpen=open; put(silentOpen,20,UINT32_MAX);
    check(bus.receive(1,silentOpen)==0); bus.service(); check(get(replies.back(),16)==0);
    auto eventCount=events;
    check(outbound.push(packet,notify)==ArmVmbusRing::Result::Ok);
    check(bus.signalChannel(7)==0); bus.service();
    check(inbound.pop(received,notify)==ArmVmbusRing::Result::Ok && received==packet);
    check(events==eventCount); // Disabled interrupt target does not suppress ring traffic.
    check(bus.receive(1,msg(16))==0); bus.service(); check(closes==3);
    check(bus.report().find("\"gpadls\":0")!=std::string::npos);
    check(bus.receive(1,body)==0x15);
    check(bus.receive(1,contact)==0); bus.service();
    check(bus.receive(1,header)==5); // Recontact must reoffer before accepting memory.
    // Queue backpressure is atomic: a rejected offer request adds no partial offers.
    block=true;
    for(unsigned i=0;i<5;++i) check(bus.receive(1,msg(3))==0);
    check(bus.receive(1,msg(3))==0x13);
    auto before=replies.size(); bus.service(); check(replies.size()==before && bus.needsService());
    block=false; bus.service(); check(replies.size()==before+15 && !bus.needsService());
    // End-to-end host registration, real PostMessage/SignalEvent and SynIC bitmap.
    std::vector<uint8_t> integratedRam(16*4096);
    ArmHyperV hv(true,integratedRam,base,2);
    check(!hv.offerVmbusChannel(offer,echo));
    hv.setPrivateInterruptSender([&](uint64_t mask,unsigned vector) {
        check((mask==1 || mask==2) && vector==16); return true;
    });
    hv.advertiseSynic(true); hv.enableVmbus();
    check(hv.offerVmbusChannel(offer,echo));
    check(!hv.removeConnection(16));
    auto bad=offer; bad.channel=0; bad.connection=18;
    check(!hv.offerVmbusChannel(bad,echo));
    check(hv.bindEventConnection(18,1,[](unsigned,uint16_t) {return uint16_t(0);}));
    check(hv.removeConnection(18)); // Failed registration released its temporary binding.
    ArmHyperV::Registers r{};
    r[0]=(1ull<<32)|0x10051; r[1]=UINT64_MAX; r[2]=0xfffffffe;
    r[3]=0x90002; r[5]=0x1000a000066bb;
    check(hv.call(1,r,0) && r[0]==(1ull<<32));
    auto &synic=hv.synicController();
    for(unsigned vp=0;vp<2;++vp) {
        check(synic.write(0xa0010,vp,1)==0);
        check(synic.write(0xa0002,vp,16)==0);
    }
    check(synic.write(0xa0013,0,base+4096+1)==0);
    check(synic.write(0xa0012,1,base+8192+1)==0);
    auto post=[&](const std::vector<uint8_t>& payload) {
        put(integratedRam,4096+2*256,0); // Guest acknowledges preceding control reply.
        std::fill(integratedRam.begin(),integratedRam.begin()+256,0);
        put(integratedRam,0,1); put(integratedRam,8,1); put(integratedRam,12,payload.size());
        std::copy(payload.begin(),payload.end(),integratedRam.begin()+16);
        r={}; r[0]=0x5c; r[1]=base;
        check(hv.call(1,r,0) && r[0]==0); hv.service();
    };
    post(contact); check(get(integratedRam,4096+512+16)==15);
    post(msg(3)); check(get(integratedRam,4096+512+16)==1);
    // Acknowledge offer and AllOffersDelivered before creating the mapping.
    put(integratedRam,4096+512,0); hv.service();
    put(integratedRam,4096+512,0); hv.service();
    post(header); check(get(integratedRam,4096+512+16)==10);
    post(open); check(get(integratedRam,4096+512+32)==0);
    ArmGpadl integratedGuest(integratedRam,base);
    check(integratedGuest.begin(7,42,40,1,std::span(header).subspan(20))==ArmGpadl::Result::Ok);
    ArmVmbusRing guestTx(integratedGuest,7,42,0,8192), guestRx(integratedGuest,7,42,8192,8192);
    check(guestTx.push(packet,notify)==ArmVmbusRing::Result::Ok);
    auto signal=[&](uint64_t input) {
        r={}; r[0]=0x1005d; r[1]=input; check(hv.call(1,r,1)); return r[0];
    };
    check(signal(16|(1ull<<32))==5); // Dedicated port permits only relative flag zero.
    check(signal(19)==0x12); // Unknown connection.
    check(signal(16)==0); hv.service();
    check(guestRx.pop(received,notify)==ArmVmbusRing::Result::Ok && received==packet);
    check((integratedRam[8192+512] & (1u<<7))!=0);
    post(msg(7,12)); check(signal(16)==0x15);
    std::cout << checks << " VMBus channel checks passed\n";
}
