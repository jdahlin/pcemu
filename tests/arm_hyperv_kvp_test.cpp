#include "arm_hyperv_kvp.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
using namespace pc;using C=ArmHyperVKvpCodec;
namespace {
unsigned checks=0;void check(bool v){++checks;if(!v){std::cerr<<"KVP check "<<checks<<" failed\n";std::exit(1);}}
void put(std::span<uint8_t> b,size_t at,uint64_t v,unsigned n=4){for(unsigned i=0;i<n;++i)b[at+i]=uint8_t(v>>(8*i));}
bool has(const ArmHyperVKvp &k,const std::string &s){return k.report().find(s)!=std::string::npos;}
auto negotiate(ArmHyperVKvp &k,unsigned fw){k.opened(0);auto p=k.packet(0);k.sent(0);put(p,20,36);put(p,34,16,2);put(p,44,1,2);put(p,46,1,2);put(p,52,fw);put(p,56,4);p[41]=5;return p;}
auto answer(std::vector<uint8_t> p){
 C::Value value;value.text=u"Windows 11";auto set=C::request(C::Operation::Set,C::Pool::Auto,0,u"OSName",&value);
 std::copy(set.begin()+4,set.begin()+2576,p.begin()+52);p[41]=5;return p;
}
}
int main(){
 auto o=ArmHyperVKvp::offer();check(o.channel==4 && o.connection==19 && o.flags==0x11 && o.userData[0]==4);check(o.interfaceId!=o.instanceId && o.instanceId[0]==0x19 && o.interfaceId[0]==0xe7);
 for(unsigned fw:{1u,3u}){
  ArmHyperVKvp k;check(k.packet(0).empty() && !k.delay(0));check(!k.enumerate(C::Pool::Auto,0));
  auto version=negotiate(k,fw);
  for(size_t n=0;n<version.size();++n){k.receive(std::span<const uint8_t>(version).first(n),1);check(has(k,"wait-version"));}
  auto bad=version;put(bad,56,5);k.receive(bad,1);check(has(k,"wait-version"));
  bad=version;put(bad,48,0x12345678);k.receive(bad,1);
  check(has(k,"wait-version") && has(k,"\"reserved\":305419896"));
  k.receive(version,1);check(has(k,"ready") && !k.delay(1));check(!k.takeResult());
  check(k.enumerate(C::Pool::Auto,7));check(!k.enumerate(C::Pool::Auto,8));
  auto query=k.packet(2);check(query.size()==2624 && query[44]==3 && query[45]==2 && query[48]==7 && query[40]==1);
  check(k.packet(3)==query && has(k,"\"requests\":0"));k.sent(3);check(k.packet(4).empty());
  auto response=answer(query);
  for(size_t n=0;n<response.size();++n){k.receive(std::span<const uint8_t>(response).first(n),4);check(has(k,"wait-response"));}
  for(auto where:{40u,41u,24u,28u,30u,42u,45u,48u}){bad=response;bad[where]^=1;k.receive(bad,4);check(has(k,"wait-response"));}
  k.receive(response,5);check(has(k,"\"records\":1") && !k.enumerate(C::Pool::Auto,8));
  auto result=k.takeResult();check(result && result->index==7 && result->status==0 && result->record && result->record->key==u"OSName" && result->record->value.text==u"Windows 11");
  k.receive(response,6);check(has(k,"\"records\":1"));check(k.enumerate(C::Pool::Auto,8));
  auto end=k.packet(6);k.sent(6);end[41]=5;put(end,36,0x80070103);k.receive(end,7);
  result=k.takeResult();check(result && result->status==0x80070103 && !result->record && has(k,"\"endOfPool\":1"));
  check(k.enumerate(C::Pool::Guest,0));auto denied=k.packet(8);k.sent(8);denied[41]=5;put(denied,36,0x80004005);k.receive(denied,9);
  result=k.takeResult();check(result && result->status==0x80004005 && !result->record);
  check(k.enumerate(C::Pool::Auto,9));auto late=k.packet(10);k.sent(10);late=answer(late);k.receive(late,10+k.timeout);
  check(has(k,"failed") && has(k,"\"timeouts\":1") && !k.takeResult() && !k.enumerate(C::Pool::Auto,0));
  k.closed();check(k.packet(0).empty() && !k.delay(0));k.opened(0);check(k.packet(0)[40]==0);
 }
 ArmHyperVKvp k;auto n=negotiate(k,3);check(k.delay(k.timeout)==0);check(k.packet(k.timeout).empty() && !k.delay(k.timeout));
 k.receive(n,k.timeout);check(has(k,"failed"));
 std::cout<<checks<<" KVP state-machine checks passed\n";
}
