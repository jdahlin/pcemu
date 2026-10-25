#include "arm_hyperv_kvp_codec.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
using C=pc::ArmHyperVKvpCodec;
namespace {
unsigned checks=0;void check(bool v){++checks;if(!v){std::cerr<<"KVP codec check "<<checks<<" failed\n";std::exit(1);}}
void put(std::vector<uint8_t>&b,size_t at,uint64_t v,unsigned n=4){for(unsigned i=0;i<n;++i)b.at(at+i)=uint8_t(v>>(8*i));}
uint64_t get(const std::vector<uint8_t>&b,size_t at,unsigned n=4){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(b.at(at+i))<<(8*i);return v;}
auto response(C::Value value){
 auto s=C::request(C::Operation::Set,C::Pool::Auto,0,u"Name",&value);
 auto b=C::request(C::Operation::Enumerate,C::Pool::Auto,7);
 std::copy(s.begin()+4,s.begin()+2576,b.begin()+8);return b;
}
template<class F>void invalid(F f){bool failed=false;try{f();}catch(const std::invalid_argument&){failed=true;}check(failed);}
}
int main(){
 C::Value value;value.text=u"Windows 11 \U0001f30d";
 auto b=response(value);auto record=C::enumeration(b,C::Pool::Auto,7);
 check(record && record->key==u"Name" && record->value.text==value.text);
 check(get(b,8)==1 && get(b,12)==10 && get(b,16)==2*(value.text.size()+1));
 check(!C::enumeration(b,C::Pool::Guest,7));check(!C::enumeration(b,C::Pool::Auto,8));
 for(size_t n=0;n<C::payloadBytes;++n)check(!C::enumeration(std::span<const uint8_t>(b).first(n),C::Pool::Auto,7));
 for(auto pair:{std::pair<size_t,uint64_t>{12,0},{12,3},{12,514},{12,UINT32_MAX},{16,0},{16,1},{16,2050},{8,99},{2,1}}){auto bad=b;put(bad,pair.first,pair.second);check(!C::enumeration(bad,C::Pool::Auto,7));}
 auto bad=b;put(bad,28,1,2);check(!C::enumeration(bad,C::Pool::Auto,7));
 bad=b;put(bad,20,0,2);check(!C::enumeration(bad,C::Pool::Auto,7));
 bad=b;put(bad,20,0xdc00,2);check(!C::enumeration(bad,C::Pool::Auto,7));
 for(auto type:{C::Type::Dword,C::Type::Qword}){
  value.type=type;value.number=type==C::Type::Dword?UINT32_MAX:UINT64_MAX;
  auto packet=response(value);auto r=C::enumeration(packet,C::Pool::Auto,7);check(r && r->value.number==value.number);
  put(packet,16,2);check(!C::enumeration(packet,C::Pool::Auto,7));
 }
 auto getRequest=C::request(C::Operation::Get,C::Pool::Guest,0,u"Key");check(getRequest[0]==0 && get(getRequest,8)==8);
 auto del=C::request(C::Operation::Delete,C::Pool::External,0,u"Key");check(del[0]==2 && get(del,4)==8 && del[8]=='K');
 invalid([&]{C::request(C::Operation::Get,C::Pool::Auto);});
 invalid([&]{C::request(C::Operation::Set,C::Pool::External,0,u"Key");});
 invalid([&]{C::request(C::Operation::Enumerate,C::Pool::Auto,0,u"Key");});
 invalid([&]{C::request(C::Operation::Delete,C::Pool::Auto,0,std::u16string(256,u'x'));});
 invalid([&]{C::request(C::Operation::Get,C::Pool::Auto,0,std::u16string{char16_t(0xd800)});});
 invalid([&]{C::request(C::Operation::Get,C::Pool::Auto,0,std::u16string{u'a',0,u'b'});});
 value.type=C::Type::Dword;value.number=uint64_t(UINT32_MAX)+1;
 invalid([&]{C::request(C::Operation::Set,C::Pool::External,0,u"Key",&value);});
 value.type=C::Type::String;value.text=std::u16string(1024,u'x');
 invalid([&]{C::request(C::Operation::Set,C::Pool::External,0,u"Key",&value);});
 value.text.clear();check(C::enumeration(response(value),C::Pool::Auto,7)->value.text.empty());
 std::cout<<checks<<" KVP codec checks passed\n";
}
