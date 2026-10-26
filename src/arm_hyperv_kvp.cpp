#include "arm_hyperv_kvp.hpp"
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <utility>
namespace pc {
namespace {
uint64_t readLE(std::span<const uint8_t> p,size_t at,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(p[at+i])<<(8*i);return v;}
void put(std::span<uint8_t> p,size_t at,uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)p[at+i]=uint8_t(v>>(8*i));}
}
ArmVmbus::Offer ArmHyperVKvp::offer(){
 ArmVmbus::Offer o{};o.channel=channelId;o.connection=connectionId;o.flags=0x11;o.userData[0]=4;
 o.interfaceId={0xe7,0xf4,0xa0,0xa9,0x45,0x5a,0x96,0x4d,0xb8,0x27,0x8a,0x84,0x1e,0x8c,0x03,0xe6};
 o.instanceId={0x19,0xf9,0x2f,0x24,0xdb,0x07,0x80,0x41,0x9c,0x2e,0xb8,0x6c,0xb6,0x8c,0x8c,0x55};return o;
}
void ArmHyperVKvp::opened(uint64_t now){state=State::SendVersion;framework=0;service=0;transaction=0;since=now;result.reset();queryBody.clear();queryKey.clear();lastStatus=0;versionReserved=versionBodySize=versionFrameworkCount=versionServiceCount=versionFramework=versionService=0;}
void ArmHyperVKvp::closed(){state=State::Closed;framework=0;service=0;result.reset();queryBody.clear();queryKey.clear();}
bool ArmHyperVKvp::request(ArmHyperVKvpCodec::Operation op,ArmHyperVKvpCodec::Pool requested,uint32_t at,const std::u16string &key,const ArmHyperVKvpCodec::Value *value){
 if(state!=State::Ready || result || unsigned(requested)>3)return false;
 if((op==ArmHyperVKvpCodec::Operation::Set || op==ArmHyperVKvpCodec::Operation::Delete) &&
    requested!=ArmHyperVKvpCodec::Pool::External && requested!=ArmHyperVKvpCodec::Pool::AutoExternal)return false;
 std::vector<uint8_t> body;
 try{body=ArmHyperVKvpCodec::encode_request(op,requested,at,key,value);}catch(const std::invalid_argument&){return false;}
 queryBody=std::move(body);queryKey=key;operation=op;pool=requested;index=at;state=State::SendQuery;return true;
}
bool ArmHyperVKvp::enumerate(ArmHyperVKvpCodec::Pool p,uint32_t i){return request(ArmHyperVKvpCodec::Operation::Enumerate,p,i);}
bool ArmHyperVKvp::get(ArmHyperVKvpCodec::Pool p,const std::u16string &key){return request(ArmHyperVKvpCodec::Operation::Get,p,0,key);}
bool ArmHyperVKvp::set(ArmHyperVKvpCodec::Pool p,const std::u16string &key,const ArmHyperVKvpCodec::Value &v){return request(ArmHyperVKvpCodec::Operation::Set,p,0,key,&v);}
bool ArmHyperVKvp::erase(ArmHyperVKvpCodec::Pool p,const std::u16string &key){return request(ArmHyperVKvpCodec::Operation::Delete,p,0,key);}
std::optional<ArmHyperVKvp::Result> ArmHyperVKvp::takeResult(){return std::exchange(result,{});}
void ArmHyperVKvp::expire(uint64_t now){
 if((state==State::WaitVersion || state==State::WaitResponse) && now-since>=timeout){state=State::Failed;++timeouts;}
}
std::vector<uint8_t> ArmHyperVKvp::packet(uint64_t now){
 expire(now);bool negotiation=state==State::SendVersion;
 if(!negotiation && state!=State::SendQuery)return {};
 auto body=negotiation?std::vector<uint8_t>(28):queryBody;
 if(negotiation){put(body,0,2,2);put(body,2,3,2);put(body,8,1,2);put(body,12,3,2);put(body,16,3,2);put(body,20,4,2);put(body,24,5,2);}
 std::vector<uint8_t> p((44+body.size()+7)&~size_t(7));
 put(p,0,6,2);put(p,2,2,2);put(p,4,p.size()/8,2);
 put(p,16,1,4);put(p,20,20+body.size(),4);
 put(p,24,negotiation?0:framework,2);put(p,28,negotiation?0:2,2);put(p,30,negotiation?0:service,2);
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
 if(p.size()<16 || readLE(p,0,2)!=6 || readLE(p,4,2)*8!=p.size())return fail();
 auto offset=readLE(p,2,2)*8;if(offset<16 || offset>p.size() || p.size()-offset<28)return fail();
 auto pipe=p.subspan(size_t(offset));auto size=readLE(pipe,4,4);
 if(readLE(pipe,0,4)!=1 || size<20 || size>pipe.size()-8)return fail();
 auto h=pipe.subspan(8,size_t(size));auto length=readLE(h,10,2);
 if(length>h.size()-20 || h[16]!=transaction || h[17]!=5 || readLE(h,18,2))return fail();
 auto body=h.subspan(20,size_t(length));auto status=uint32_t(readLE(h,12,4));
 if(state==State::WaitVersion && readLE(h,4,2)==0){
  versionBodySize=uint16_t(body.size());
  if(body.size()>=8){
   versionFrameworkCount=uint16_t(readLE(body,0,2));versionServiceCount=uint16_t(readLE(body,2,2));
   versionReserved=uint32_t(readLE(body,4,4));
  }
  if(body.size()>=16){versionFramework=uint32_t(readLE(body,8,4));versionService=uint32_t(readLE(body,12,4));
  }
  if(status){state=State::Failed;lastStatus=status;return;}
  if(body.size()<16 || readLE(body,0,2)!=1 || readLE(body,2,2)!=1 || readLE(body,4,4) ||
     readLE(body,10,2) || (readLE(body,12,2)<3 || readLE(body,12,2)>5) || readLE(body,14,2))return fail();
  auto fw=readLE(body,8,2);if(fw!=1 && fw!=3)return fail();
  framework=uint16_t(fw);service=uint16_t(readLE(body,12,2));state=State::Ready;++transaction;return;
 }
 if(state!=State::WaitResponse || readLE(h,0,2)!=framework || readLE(h,2,2) || readLE(h,4,2)!=2 || readLE(h,6,2)!=service || readLE(h,8,2))return fail();
 std::optional<ArmHyperVKvpCodec::Record> record;
 if(!status){
  if(operation==ArmHyperVKvpCodec::Operation::Enumerate)record=ArmHyperVKvpCodec::decode_enumeration(body,pool,index);
  else if(operation==ArmHyperVKvpCodec::Operation::Get)record=ArmHyperVKvpCodec::decode_get_response(body,pool,queryKey);
  else{
   if(!ArmHyperVKvpCodec::mutation_acknowledged(body,operation,pool,queryKey))return fail();
   ++mutationAcks;
  }
  if(operation==ArmHyperVKvpCodec::Operation::Enumerate || operation==ArmHyperVKvpCodec::Operation::Get){if(!record)return fail();++records;}
 }
 else if(status==0x80070103 && operation==ArmHyperVKvpCodec::Operation::Enumerate)++ends;
 result=Result{pool,index,status,std::move(record),operation};lastStatus=status;queryBody.clear();queryKey.clear();++responses;++transaction;state=State::Ready;
}
std::optional<uint64_t> ArmHyperVKvp::delay(uint64_t now) const{
 if(state==State::SendVersion || state==State::SendQuery)return 0;
 if(state==State::WaitVersion || state==State::WaitResponse)return now-since>=timeout?0:timeout-(now-since);
 return {};
}
std::string ArmHyperVKvp::report() const{
 const char *name=state==State::Closed?"closed":state==State::SendVersion?"send-version":state==State::WaitVersion?"wait-version":state==State::Ready?"ready":state==State::SendQuery?"send-query":state==State::WaitResponse?"wait-response":"failed";
 std::ostringstream s;s<<"{\"state\":\""<<name<<"\",\"frameworkMajor\":"<<framework<<",\"kvpMajor\":"<<service
 <<",\"negotiations\":"<<negotiations<<",\"requests\":"<<requests<<",\"responses\":"<<responses<<",\"records\":"<<records
 <<",\"versionReply\":{\"bodyBytes\":"<<versionBodySize<<",\"frameworkCount\":"<<versionFrameworkCount
 <<",\"serviceCount\":"<<versionServiceCount<<",\"reserved\":"<<versionReserved<<",\"framework\":"<<versionFramework<<",\"service\":"<<versionService<<'}'
 <<",\"operation\":"<<unsigned(operation)<<",\"mutationAcknowledgments\":"<<mutationAcks
 <<",\"endOfPool\":"<<ends<<",\"lastStatus\":"<<lastStatus<<",\"resultPending\":"<<(result?"true":"false")
 <<",\"rejectedResponses\":"<<rejected<<",\"timeouts\":"<<timeouts<<'}';return s.str();
}
}
