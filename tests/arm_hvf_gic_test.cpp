#include "hvf_machine.hpp"
#include "arm_gic_ns.hpp"
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace pc;
int main() {
    unsigned checks=0;
    auto check=[&](bool value,const char *message) {++checks;if(!value)throw std::runtime_error(message);};
    try {
        {HvfMachine normal(2*1024*1024,true);check(((normal.systemReg(0xc020)>>8)&15)==0,"Native EL1-only policy changed");}
        HvfMachine vm(2*1024*1024,HvfMachine::GicProfile::HyperVNonsecure,{},2);
        ArmGicNonsecureView view([&](uint32_t o){return vm.nativeGicDistributorRead(o);},
                                [&](uint32_t o,uint64_t v){vm.nativeGicDistributorWrite(o,v);});
        check((view.read_register(4,4)&0x400)!=0,"Nonsecure security view missing");
        check(view.read_register(0xffe8,4)==0x30,"Native GIC identity cache incorrect");
        check(vm.reg(1)==0 && vm.reg(4)==0,"Bootstrap leaked registers into firmware context");
        check(vm.systemReg(0xc600)==0,"Bootstrap left its unmapped VBAR installed");
        // EL1 guest checks both VPs' PFR0/CurrentEL/private Group1 assignment,
        // programs the guest distributor, acknowledges native SPI48 and SGI8.
        // DAIF stays masked; IAR/EOI exercise real GIC delivery explicitly.
        constexpr std::array<uint32_t,54> code{
0xd5380400,0xd3482c00,0xd4000402,0xd5384240,0xd4000422,0xd53800a1,0x92401c21,0xd2a10162,0xf2801002,0x8b014442,0xb9400040,0xd4000442,0xb5000501,0xd2a10001,0xb9400420,0xd4000462,0x52800242,0xb9000022,0x52a00022,0xb9010422,0xd28c3002,0xf822683f,0x52801402,0x3910c022,0xd2800022,0xd518cca2,0xd5033fdf,0xd2801202,0xd5184602,0xd2800022,0xd518cce2,0xd5033fdf,0xd4000482,0xd538cc00,0xd40004a2,0xd2801fe2,0xd5184602,0xd5033fdf,0xd538cc00,0xd4000542,0xd518cc20,0xd40004c2,0xd2a10162,0xf2802002,0x52802003,0xb9000043,0xd40004e2,0xd538cc00,0xd4000502,0xd518cc20,0xd4000522,0x14000000,0xd4000602,0x14000000
        };
        vm.writeMemory(HvfMachine::ramBase,{reinterpret_cast<const uint8_t*>(code.data()),sizeof(code)});
        vm.start(HvfMachine::ramBase);
        check(vm.startCPU(1,HvfMachine::ramBase,0)==0,"Secondary did not start");
        std::array<bool,2> done{};
        unsigned features=0,levels=0,groups=0,mmio=0;
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(!(done[0]&&done[1]) && std::chrono::steady_clock::now()<deadline) {
            auto exit=vm.run(std::chrono::milliseconds(100));
            if(exit.reason==HvfMachine::ExitReason::Canceled)continue;
            check(exit.nativeReason==1,"Native exception reason was not preserved");
            if(exit.exceptionClass()==0x24 && (exit.syndrome&(1ull<<24))) {
                unsigned width=1u<<((exit.syndrome>>22)&3),reg=(exit.syndrome>>16)&31;
                check(exit.physicalAddress>=ArmGicNonsecureView::GuestDistributorBase &&
                      exit.physicalAddress<ArmGicNonsecureView::GuestDistributorBase+0x10000,"Unexpected native data abort");
                auto offset=exit.physicalAddress-ArmGicNonsecureView::GuestDistributorBase;
                if(exit.syndrome&64)view.write_register(offset,reg<31?vm.reg(reg):0,width);
                else {auto value=view.read_register(offset,width);if(reg<31)vm.setReg(reg,width==4?uint32_t(value):value);}
                vm.setPC(exit.pc+4);++mmio;continue;
            }
            check(exit.reason==HvfMachine::ExitReason::Exception && exit.exceptionClass()==0x16,"Unexpected native exit");
            switch(exit.syndrome&0xffff) {
            case 0x20:check(vm.reg(0)==1,"Guest EL2 identity mismatch");++features;break;
            case 0x21:check(vm.reg(0)==4,"Guest entered an unsupported exception level");++levels;break;
            case 0x22:check(vm.reg(0)==UINT32_MAX,"Private interrupts not assigned to Group1");++groups;break;
            case 0x23:check(vm.reg(0)&0x400,"Guest did not see distributor view");break;
            case 0x24:vm.setSPI(48,true);break;
            case 0x25:check(vm.reg(0)==1023,"Priority mask did not block SPI48");break;
            case 0x2a:check(vm.reg(0)==48,"Unmasked guest did not acknowledge native SPI48");break;
            case 0x26:vm.setSPI(48,false);break;
            case 0x27:vm.sendPrivateInterrupt(1,8);break;
            case 0x28:check(vm.reg(0)==8,"Guest did not acknowledge native SGI8");break;
            case 0x29:done[0]=true;break;
            case 0x30:done[1]=true;check(vm.stopCPU()==0,"Secondary did not stop");break;
            default:throw std::runtime_error("Unexpected guest completion marker");
            }
        }
        check(done[0]&&done[1],"Native GIC guest timed out");
        check(features==2&&levels==2&&groups==2&&mmio==5,"Incomplete native guest observations");
        auto interrupts=vm.privateInterruptState(0);
        check((interrupts.enabled&0x100) && !(interrupts.pending&0x100) && !(interrupts.active&0x100),"SGI8 was not retired");
        std::cout<<"{\"test\":\"hyperv-gic\",\"passed\":true,\"checks\":"<<checks<<"}\n";
    } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
