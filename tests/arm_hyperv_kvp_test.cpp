#include "arm_hyperv_kvp.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
using namespace pc;using C=ArmHyperVKvpCodec;
namespace {
unsigned checks=0;void check(bool v){++checks;if(!v){std::cerr<<"KVP check "<<checks<<" failed\n";std::exit(1);}}
void put(std::span<uint8_t> b,size_t at,uint64_t v,unsigned n=4){for(unsigned i=0;i<n;++i)b[at+i]=uint8_t(v>>(8*i));}
bool has(const ArmHyperVKvp &k,const std::string &s){return k.report().find(s)!=std::string::npos;}
auto negotiate(ArmHyperVKvp &k,unsigned fw,unsigned service=4){k.channel_opened(0);auto p=k.pending_packet(0);k.packet_published(0);put(p,20,36);put(p,34,16,2);put(p,44,1,2);put(p,46,1,2);put(p,52,fw);put(p,56,service);p[41]=5;return p;}
auto answer(std::vector<uint8_t> p){
 C::Value value;value.text=u"Windows 11";auto set=C::encode_request(C::Operation::Set,C::Pool::Auto,0,u"OSName",&value);
 std::copy(set.begin()+4,set.begin()+2576,p.begin()+52);p[41]=5;return p;
}
}
int main(){
 auto o=ArmHyperVKvp::channel_offer();check(o.channel==4 && o.connection==19 && o.flags==0x11 && o.userData[0]==4);check(o.interfaceId!=o.instanceId && o.instanceId[0]==0x19 && o.interfaceId[0]==0xe7);
 for(unsigned fw:{1u,3u})for(unsigned service:{3u,4u,5u}){
  ArmHyperVKvp k;check(k.pending_packet(0).empty() && !k.next_delay_100ns(0));check(!k.enumerate_value(C::Pool::Auto,0));
  auto version=negotiate(k,fw,service);
  for(size_t n=0;n<version.size();++n){k.receive_response(std::span<const uint8_t>(version).first(n),1);check(has(k,"wait-version"));}
  auto bad=version;put(bad,56,6);k.receive_response(bad,1);check(has(k,"wait-version"));
  bad=version;put(bad,48,0x12345678);k.receive_response(bad,1);
  check(has(k,"wait-version") && has(k,"\"reserved\":305419896"));
  k.receive_response(version,1);check(has(k,"ready") && !k.next_delay_100ns(1));check(!k.take_result());
  check(k.enumerate_value(C::Pool::Auto,7));check(!k.enumerate_value(C::Pool::Auto,8));
  auto query=k.pending_packet(2);check(query.size()==2624 && query[44]==3 && query[45]==2 && query[48]==7 && query[40]==1);
  check(k.pending_packet(3)==query && has(k,"\"requests\":0"));k.packet_published(3);check(k.pending_packet(4).empty());
  auto response=answer(query);
  for(size_t n=0;n<response.size();++n){k.receive_response(std::span<const uint8_t>(response).first(n),4);check(has(k,"wait-response"));}
  for(auto where:{40u,41u,24u,28u,30u,42u,45u,48u}){bad=response;bad[where]^=1;k.receive_response(bad,4);check(has(k,"wait-response"));}
  k.receive_response(response,5);check(has(k,"\"records\":1") && !k.enumerate_value(C::Pool::Auto,8));
  auto result=k.take_result();check(result && result->index==7 && result->status==0 && result->record && result->record->key==u"OSName" && result->record->value.text==u"Windows 11");
  k.receive_response(response,6);check(has(k,"\"records\":1"));check(k.enumerate_value(C::Pool::Auto,8));
  auto end=k.pending_packet(6);k.packet_published(6);end[41]=5;put(end,36,0x80070103);k.receive_response(end,7);
  result=k.take_result();check(result && result->status==0x80070103 && !result->record && has(k,"\"endOfPool\":1"));
  check(k.enumerate_value(C::Pool::Guest,0));auto denied=k.pending_packet(8);k.packet_published(8);denied[41]=5;put(denied,36,0x80004005);k.receive_response(denied,9);
  result=k.take_result();check(result && result->status==0x80004005 && !result->record);
  C::Value value;value.text=u"Disposable value";
  check(!k.set_value(C::Pool::Auto,u"ProbeKey",value) && !k.delete_value(C::Pool::Guest,u"ProbeKey"));
  check(!k.get_value(C::Pool::External,u""));
  check(k.set_value(C::Pool::External,u"ProbeKey",value));auto mutation=k.pending_packet(10);
  check(mutation[44]==1 && !k.get_value(C::Pool::External,u"ProbeKey"));k.packet_published(10);mutation[41]=5;
  auto mismatched=mutation;mismatched[60]^=1;k.receive_response(mismatched,11);check(has(k,"wait-response"));
  k.receive_response(mutation,11);result=k.take_result();
  check(result && result->operation==C::Operation::Set && !result->record && result->status==0);
  check(k.get_value(C::Pool::External,u"ProbeKey"));auto getReply=k.pending_packet(12);k.packet_published(12);
  auto encoded=C::encode_request(C::Operation::Set,C::Pool::External,0,u"ProbeKey",&value);encoded[0]=0;
  std::copy(encoded.begin(),encoded.end(),getReply.begin()+44);getReply[41]=5;k.receive_response(getReply,13);
  result=k.take_result();check(result && result->operation==C::Operation::Get && result->record && result->record->value.text==value.text);
  check(k.delete_value(C::Pool::External,u"ProbeKey"));mutation=k.pending_packet(14);k.packet_published(14);mutation[41]=5;k.receive_response(mutation,15);
  result=k.take_result();check(result && result->operation==C::Operation::Delete && result->status==0 && !result->record);
  check(has(k,"\"mutationAcknowledgments\":2"));
  check(k.get_value(C::Pool::External,u"ProbeKey"));mutation=k.pending_packet(16);k.packet_published(16);mutation[41]=5;put(mutation,36,0x80070002);k.receive_response(mutation,17);
  result=k.take_result();check(result && result->status==0x80070002 && !result->record);
  check(k.enumerate_value(C::Pool::Auto,9));auto late=k.pending_packet(20);k.packet_published(20);late=answer(late);k.receive_response(late,20+k.ResponseTimeout100ns);
  check(has(k,"failed") && has(k,"\"timeouts\":1") && !k.take_result() && !k.enumerate_value(C::Pool::Auto,0));
  k.channel_closed();check(k.pending_packet(0).empty() && !k.next_delay_100ns(0));k.channel_opened(0);check(k.pending_packet(0)[40]==0);
 }
 ArmHyperVKvp k;auto n=negotiate(k,3);check(k.next_delay_100ns(k.ResponseTimeout100ns)==0);check(k.pending_packet(k.ResponseTimeout100ns).empty() && !k.next_delay_100ns(k.ResponseTimeout100ns));
 k.receive_response(n,k.ResponseTimeout100ns);check(has(k,"failed"));
 std::cout<<checks<<" KVP state-machine checks passed\n";
}
