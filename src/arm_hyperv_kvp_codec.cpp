#include "arm_hyperv_kvp_codec.hpp"
#include <stdexcept>
namespace pc {
namespace {
uint64_t get(std::span<const uint8_t> b,size_t at,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(b[at+i])<<(8*i);return v;}
void put(std::span<uint8_t> b,size_t at,uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)b[at+i]=uint8_t(v>>(8*i));}
bool valid(const std::u16string &s){
 for(size_t i=0;i<s.size();++i){
  auto c=uint16_t(s[i]);if(!c)return false;
  if(c>=0xd800 && c<=0xdbff){if(++i==s.size() || s[i]<0xdc00 || s[i]>0xdfff)return false;}
  else if(c>=0xdc00 && c<=0xdfff)return false;
 }
 return true;
}
void text(std::span<uint8_t> b,size_t at,const std::u16string &s,size_t maximum){
 if(s.size()>=maximum/2 || !valid(s))throw std::invalid_argument("Invalid KVP UTF-16 string");
 for(size_t i=0;i<s.size();++i)put(b,at+2*i,s[i],2);
}
std::optional<std::u16string> text(std::span<const uint8_t> b,size_t at,size_t size,size_t maximum){
 if(size<2 || size>maximum || (size&1) || get(b,at+size-2,2))return {};
 std::u16string s;for(size_t i=0;i<size-2;i+=2)s+=char16_t(get(b,at+i,2));
 if(!valid(s))return {};return s;
}
}
std::vector<uint8_t> ArmHyperVKvpCodec::request(Operation op,Pool pool,uint32_t index,const std::u16string &key,const Value *value){
 if(unsigned(op)>3 || unsigned(pool)>3 || (op!=Operation::Enumerate && key.empty()) ||
    (op==Operation::Enumerate && (!key.empty() || value)) ||
    (op!=Operation::Set && value) || (op==Operation::Set && !value))
  throw std::invalid_argument("Invalid KVP request");
 std::vector<uint8_t> b(payloadBytes);b[0]=uint8_t(op);b[1]=uint8_t(pool);
 if(op==Operation::Enumerate){put(b,4,index,4);return b;}
 if(op==Operation::Delete){text(b,8,key,maxKeyBytes);put(b,4,(key.size()+1)*2,4);return b;}
 text(b,16,key,maxKeyBytes);put(b,8,(key.size()+1)*2,4);
 if(op==Operation::Get)return b;
 put(b,4,uint32_t(value->type),4);
 switch(value->type){
 case Type::String:case Type::ExpandString:
  text(b,528,value->text,maxValueBytes);put(b,12,(value->text.size()+1)*2,4);break;
 case Type::Dword:
  if(value->number>UINT32_MAX)throw std::invalid_argument("KVP DWORD overflow");
  put(b,12,4,4);put(b,528,value->number,4);break;
 case Type::Qword:put(b,12,8,4);put(b,528,value->number,8);break;
 default:throw std::invalid_argument("Unsupported KVP value type");
 }
 return b;
}
namespace {
std::optional<ArmHyperVKvpCodec::Record> record(std::span<const uint8_t> b,size_t at){
 using C=ArmHyperVKvpCodec;
 auto key=text(b,at+12,size_t(get(b,at+4,4)),C::maxKeyBytes);if(!key || key->empty())return {};
 C::Record r;r.key=std::move(*key);r.value.type=C::Type(get(b,at,4));auto size=get(b,at+8,4);
 switch(r.value.type){
 case C::Type::String:case C::Type::ExpandString:{
  auto value=text(b,at+524,size_t(size),C::maxValueBytes);if(!value)return {};r.value.text=std::move(*value);break;
 }
 case C::Type::Dword:if(size!=4)return {};r.value.number=get(b,at+524,4);break;
 case C::Type::Qword:if(size!=8)return {};r.value.number=get(b,at+524,8);break;
 default:return {};
 }
 return r;
}
bool header(std::span<const uint8_t> b,ArmHyperVKvpCodec::Operation op,ArmHyperVKvpCodec::Pool pool){
 return b.size()==ArmHyperVKvpCodec::payloadBytes && unsigned(pool)<=3 && b[0]==uint8_t(op) && b[1]==uint8_t(pool) && !get(b,2,2);
}
}
std::optional<ArmHyperVKvpCodec::Record> ArmHyperVKvpCodec::enumeration(std::span<const uint8_t> b,Pool pool,uint32_t index){
 if(!header(b,Operation::Enumerate,pool) || get(b,4,4)!=index)return {};
 return record(b,8);
}
std::optional<ArmHyperVKvpCodec::Record> ArmHyperVKvpCodec::getResponse(std::span<const uint8_t> b,Pool pool,const std::u16string &key){
 if(!header(b,Operation::Get,pool))return {};
 auto r=record(b,4);if(!r || r->key!=key)return {};return r;
}
bool ArmHyperVKvpCodec::mutationAcknowledged(std::span<const uint8_t> b,Operation op,Pool pool,const std::u16string &key){
 if((op!=Operation::Set && op!=Operation::Delete) || !header(b,op,pool))return false;
 auto sizeAt=op==Operation::Set?8u:4u,keyAt=op==Operation::Set?16u:8u;
 auto returned=text(b,keyAt,size_t(get(b,sizeAt,4)),maxKeyBytes);
 return returned && !returned->empty() && *returned==key;
}
}
