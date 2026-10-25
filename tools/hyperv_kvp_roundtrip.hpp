#pragma once
#include "arm_hyperv_kvp.hpp"
#include <string>
#include <utility>
namespace pc {
// Opt-in validation on a private VM. Preflight never overwrites an existing
// key. Once SET is attempted, cleanup is explicit DELETE, never a SET retry.
class HyperVKvpRoundtrip {
 public:
  explicit HyperVKvpRoundtrip(std::u16string name):key(std::move(name)){}
  template<class Machine> void tick(Machine &vm){
   using C=ArmHyperVKvpCodec;
   if(done)return;
   if(!queued){
    if(stage==1){C::Value v;v.text=value;queued=vm.setGuestKvp(C::Pool::External,key,v);if(queued)setAttempted=true;}
    else if(stage==3)queued=vm.deleteGuestKvp(C::Pool::External,key);
    else queued=vm.getGuestKvp(C::Pool::External,key);
    if(!queued)return;
   }
   auto r=vm.takeGuestKvpResult();if(!r)return;
   queued=false;lastStatus=r->status;
   auto expected=stage==1?C::Operation::Set:stage==3?C::Operation::Delete:C::Operation::Get;
   if(r->operation!=expected || r->pool!=C::Pool::External){done=true;return;}
   switch(stage){
    case 0:absent=r->status==0x80070002;if(!absent){done=true;return;}stage=1;break;
    case 1:setAck=r->status==0;stage=setAck?2:3;break;
    case 2:readback=r->status==0 && r->record && r->record->key==key && r->record->value.type==C::Type::String && r->record->value.text==value;stage=3;break;
    case 3:deleteAck=r->status==0;stage=4;break;
    case 4:removed=r->status==0x80070002;done=true;break;
   }
  }
  bool finished()const{return done;}
  bool passed()const{return done && absent && setAck && readback && deleteAck && removed;}
  std::string report()const{
   auto b=[](bool v){return v?"true":"false";};
   return std::string("{\"finished\":")+b(done)+",\"passed\":"+b(passed())+",\"setAttempted\":"+b(setAttempted)+",\"setAcknowledged\":"+b(setAck)+",\"readbackVerified\":"+b(readback)+",\"deleteAcknowledged\":"+b(deleteAck)+",\"removalVerified\":"+b(removed)+",\"cleanupRequired\":"+b(setAttempted&&!removed)+",\"lastStatus\":"+std::to_string(lastStatus)+'}';
  }
 private:
  std::u16string key,value=u"PCEmu disposable KVP roundtrip";
  unsigned stage=0;bool queued=false,done=false,absent=false,setAttempted=false,setAck=false,readback=false,deleteAck=false,removed=false;
  uint32_t lastStatus=0;
};
}
