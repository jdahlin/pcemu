#include "arm_hyperv_kvp.hpp"
#include <algorithm>
#include <sstream>
#include <utility>
namespace pc {
namespace {
uint64_t get(std::span<const uint8_t> p,size_t at,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(p[at+i])<<(8*i);return v;}
void put(std::span<uint8_t> p,size_t at,uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)p[at+i]=uint8_t(v>>(8*i));}
}
ArmVmbus::Offer ArmHyperVKvp::offer(){
 ArmVmbus::Offer o{};o.channel=channelId;o.connection=connectionId;o.flags=0x11;o.userData[0]=4;
 o.interfaceId={0xe7,0xf4,0xa0,0xa9,0x45,0x5a,0x96,0x4d,0xb8,0x27,0x8a,0x84,0x1e,0x8c,0x03,0xe6};
 o.instanceId={0x19,0xf9,0x2f,0x24,0xdb,0x07,0x80,0x41,0x9c,0x2e,0xb8,0x6c,0xb6,0x8c,0x8c,0x55};return o;
}
void ArmHyperVKvp::opened(uint64_t now){state=State::SendVersion;framework=0;transaction=0;since=now;result.reset();lastStatus=0;}
void ArmHyperVKvp::closed(){state=State::Closed;framework=0;result.reset();}
bool ArmHyperVKvp::enumerate(ArmHyperVKvpCodec::Pool requested,uint32_t at){
 if(state!=State::Ready || result || unsigned(requested)>3)return false;
 pool=requested;index=at;state=State::SendQuery;return true;
}
std::optional<ArmHyperVKvp::Result> ArmHyperVKvp::takeResult(){return std::exchange(result,{});}
void ArmHyperVKvp::expire(uint64_t now){
 if((state==State::WaitVersion || state==State::WaitResponse) && now-since>=timeout){state=State::Failed;++timeouts;}
}
std::vector<uint8_t> ArmHyperVKvp::packet(uint64_t now){
 expire(now);bool negotiation=state==State::SendVersion;
 if(!negotiation && state!=State::SendQuery)return {};
 auto body=negotiation?std::vector<uint8_t>(20):ArmHyperVKvpCodec::request(ArmHyperVKvpCodec::Operation::Enumerate,pool,index);
 if(negotiation){put(body,0,2,2);put(body,2,1,2);put(body,8,1,2);put(body,12,3,2);put(body,16,4,2);}
 std::vector<uint8_t> p((44+body.size()+7)&~size_t(7));
 put(p,0,6,2);put(p,2,2,2);put(p,4,p.size()/8,2);
 put(p,16,1,4);put(p,20,20+body.size(),4);
 put(p,24,negotiation?0:framework,2);put(p,28,negotiation?0:2,2);put(p,30,negotiation?0:4,2);
 put(p,34,body.size(),2);p[40]=transaction;p[41]=3;
 std::copy(body.begin(),body.end(),p.begin()+44);return p;
}
void ArmHyperVKvp::sent(uint64_t now){
 if(state==State::SendVersion){state=State::WaitVersion;++negotiations;}
 else if(state==State::SendQuery){state=State::WaitResponse;++requests;}
 else return;
 since=now;
}
void ArmHyperVKvp::receive(std::span<const uint8_t> p,uint64_t now){
 expire(now);auto fail=[&]{++rejected;};
 if(p.size()<16 || get(p,0,2)!=6 || get(p,4,2)*8!=p.size())return fail();
 auto offset=get(p,2,2)*8;if(offset<16 || offset>p.size() || p.size()-offset<28)return fail();
 auto pipe=p.subspan(size_t(offset));auto size=get(pipe,4,4);
 if(get(pipe,0,4)!=1 || size<20 || size>pipe.size()-8)return fail();
 auto h=pipe.subspan(8,size_t(size));auto length=get(h,10,2);
 if(length>h.size()-20 || h[16]!=transaction || h[17]!=5 || get(h,18,2))return fail();
 auto body=h.subspan(20,size_t(length));auto status=uint32_t(get(h,12,4));
 if(state==State::WaitVersion && get(h,4,2)==0){
  if(status){state=State::Failed;lastStatus=status;return;}
  if(body.size()<16 || get(body,0,2)!=1 || get(body,2,2)!=1 || get(body,4,4) ||
     get(body,10,2) || get(body,12,2)!=4 || get(body,14,2))return fail();
  auto fw=get(body,8,2);if(fw!=1 && fw!=3)return fail();
  framework=uint16_t(fw);state=State::Ready;++transaction;return;
 }
 if(state!=State::WaitResponse || get(h,0,2)!=framework || get(h,2,2) || get(h,4,2)!=2 || get(h,6,2)!=4 || get(h,8,2))return fail();
 std::optional<ArmHyperVKvpCodec::Record> record;
 if(!status){record=ArmHyperVKvpCodec::enumeration(body,pool,index);if(!record)return fail();++records;}
 else if(status==0x80070103)++ends;
 result=Result{pool,index,status,std::move(record)};lastStatus=status;++responses;++transaction;state=State::Ready;
}
std::optional<uint64_t> ArmHyperVKvp::delay(uint64_t now) const{
 if(state==State::SendVersion || state==State::SendQuery)return 0;
 if(state==State::WaitVersion || state==State::WaitResponse)return now-since>=timeout?0:timeout-(now-since);
 return {};
}
std::string ArmHyperVKvp::report() const{
 const char *name=state==State::Closed?"closed":state==State::SendVersion?"send-version":state==State::WaitVersion?"wait-version":state==State::Ready?"ready":state==State::SendQuery?"send-query":state==State::WaitResponse?"wait-response":"failed";
 std::ostringstream s;s<<"{\"state\":\""<<name<<"\",\"frameworkMajor\":"<<framework<<",\"kvpMajor\":"<<(framework?4:0)
 <<",\"negotiations\":"<<negotiations<<",\"requests\":"<<requests<<",\"responses\":"<<responses<<",\"records\":"<<records
 <<",\"endOfPool\":"<<ends<<",\"lastStatus\":"<<lastStatus<<",\"resultPending\":"<<(result?"true":"false")
 <<",\"rejectedResponses\":"<<rejected<<",\"timeouts\":"<<timeouts<<'}';return s.str();
}
}
