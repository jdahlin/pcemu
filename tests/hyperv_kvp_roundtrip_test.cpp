#include "../tools/hyperv_kvp_roundtrip.hpp"
#include <cstdlib>
#include <iostream>
using namespace pc;using C=ArmHyperVKvpCodec;
struct Mock {
 bool exists=false,wrong=false,deny=false,blocked=false;unsigned sets=0,deletes=0,gets=0;
 std::optional<ArmHyperVKvp::Result> result;
 bool getGuestKvp(C::Pool p,const std::u16string &key){if(blocked)return false;++gets;std::optional<C::Record> record;if(exists){record=C::Record{};record->key=key;record->value.text=wrong?u"wrong":u"PCEmu disposable KVP roundtrip";}result=ArmHyperVKvp::Result{p,0,exists?0u:0x80070002u,record,C::Operation::Get};return true;}
 bool setGuestKvp(C::Pool p,const std::u16string&,const C::Value&){if(blocked)return false;++sets;if(!deny)exists=true;result=ArmHyperVKvp::Result{p,0,deny?0x80070005u:0u,{},C::Operation::Set};return true;}
 bool deleteGuestKvp(C::Pool p,const std::u16string&){if(blocked)return false;++deletes;exists=false;result=ArmHyperVKvp::Result{p,0,0,{},C::Operation::Delete};return true;}
 auto takeGuestKvpResult(){return std::exchange(result,{});}
};
int main(){unsigned checks=0;auto check=[&](bool v){++checks;if(!v)std::exit(1);};
 for(unsigned mode=0;mode<4;++mode){Mock m;m.exists=mode==1;m.wrong=mode==2;m.deny=mode==3;HyperVKvpRoundtrip p(u"Disposable");
  m.blocked=true;p.tick(m);check(m.gets==0&&!p.finished());m.blocked=false;
  for(unsigned i=0;i<10&&!p.finished();++i)p.tick(m);
  check(p.finished());check(p.passed()==(mode==0));
  if(mode==1)check(m.sets==0&&m.deletes==0&&m.exists);
  else check(m.sets==1&&m.deletes==1&&!m.exists);
  auto counts=m.gets+m.sets+m.deletes;p.tick(m);check(counts==m.gets+m.sets+m.deletes);
 }
 Mock m;HyperVKvpRoundtrip pending(u"Disposable");pending.tick(m);m.blocked=false;
 pending.tick(m);m.result.reset();
 for(unsigned i=0;i<5;++i)pending.tick(m);
 check(m.sets==1 && !pending.finished());
 check(pending.report().find("\"cleanupRequired\":true")!=std::string::npos);
 std::cout<<checks<<" KVP roundtrip controller checks passed\n";
}
