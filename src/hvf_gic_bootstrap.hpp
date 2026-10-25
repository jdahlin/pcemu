#pragma once
// Prefirmware initialization only. The newly created CPU has no guest context
// to preserve; normal start() initializes it after this bounded EL1/HVC stub.
#include <Hypervisor/Hypervisor.h>
#include <libkern/OSCacheControl.h>
#include <array>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
namespace pc {
inline std::array<uint32_t,13> hvfInitializeNonsecureGic(hv_vcpu_t cpu, hv_vcpu_exit_t *exit, unsigned index) {
    constexpr uint64_t scratch=0xf00100000ull, distributor=0xf00000000ull;
    auto check=[](hv_return_t status,const char *what) {
        if(status!=HV_SUCCESS) throw std::runtime_error(std::string(what)+": "+std::to_string(status));
    };
    struct Scratch {
        void *memory=MAP_FAILED; bool mapped=false;
        ~Scratch() {if(mapped)hv_vm_unmap(scratch,16384);if(memory!=MAP_FAILED)munmap(memory,16384);}
    } page;
    page.memory=mmap(nullptr,16384,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(page.memory==MAP_FAILED) throw std::runtime_error("Cannot allocate GIC bootstrap page");
    auto words=static_cast<uint32_t*>(page.memory);
    std::fill(words,words+4096,0xd41fffa2); // Unexpected exception: HVC #0xfffd.
    unsigned n=0;
    auto pointer=[&](uint64_t address) {
        words[n++]=0xd2800002 | (uint32_t(address&0xffff)<<5);
        words[n++]=0xf2a00002 | (uint32_t((address>>16)&0xffff)<<5);
        words[n++]=0xf2c00002 | (uint32_t((address>>32)&0xffff)<<5);
    };
    pointer(0x080b0080ull+index*0x20000ull);
    words[n++]=0x12800003; // MOV w3, #-1
    words[n++]=0xb9000043; // STR w3, [x2]: host assigns private interrupts to Group1.
    words[n++]=0xd5033f9f; // DSB SY
    words[n++]=0xb9400044; // LDR w4, [x2]: require exact readback.
    if(index==0) {
        pointer(distributor+8);words[n++]=0xb9400045; // IIDR -> w5.
        pointer(distributor+0xffd0);
        for(unsigned i=0;i<12;++i) words[n++]=0xb9400000 | (i<<10) | (2<<5) | (6+i);
    }
    words[n++]=0xd5384241; // MRS x1, CurrentEL.
    words[n++]=0xd41fff82; // HVC #0xfffc, explicit successful completion.
    sys_icache_invalidate(page.memory,16384);
    check(hv_vm_map(page.memory,scratch,16384,HV_MEMORY_READ|HV_MEMORY_EXEC),"Map GIC bootstrap");page.mapped=true;
    constexpr std::array<hv_sys_reg_t,6> systemNames{HV_SYS_REG_SCTLR_EL1,HV_SYS_REG_VBAR_EL1,
        HV_SYS_REG_ELR_EL1,HV_SYS_REG_SPSR_EL1,HV_SYS_REG_ESR_EL1,HV_SYS_REG_FAR_EL1};
    std::array<uint64_t,6> savedSystem{};
    std::array<uint64_t,31> savedRegisters{};
    uint64_t savedPc=0,savedPstate=0;
    for(unsigned i=0;i<systemNames.size();++i)
        check(hv_vcpu_get_sys_reg(cpu,systemNames[i],&savedSystem[i]),"Save bootstrap system context");
    for(unsigned i=0;i<savedRegisters.size();++i)
        check(hv_vcpu_get_reg(cpu,static_cast<hv_reg_t>(HV_REG_X0+i),&savedRegisters[i]),"Save bootstrap register");
    check(hv_vcpu_get_reg(cpu,HV_REG_PC,&savedPc),"Save bootstrap PC");
    check(hv_vcpu_get_reg(cpu,HV_REG_CPSR,&savedPstate),"Save bootstrap PSTATE");
    auto restore=[&] {
        for(unsigned i=0;i<systemNames.size();++i)
            check(hv_vcpu_set_sys_reg(cpu,systemNames[i],savedSystem[i]),"Restore bootstrap system context");
        for(unsigned i=0;i<savedRegisters.size();++i)
            check(hv_vcpu_set_reg(cpu,static_cast<hv_reg_t>(HV_REG_X0+i),savedRegisters[i]),"Restore bootstrap register");
        check(hv_vcpu_set_reg(cpu,HV_REG_PC,savedPc),"Restore bootstrap PC");
        check(hv_vcpu_set_reg(cpu,HV_REG_CPSR,savedPstate),"Restore bootstrap PSTATE");
    };
    std::array<uint32_t,13> ids{};
    try {
        check(hv_vcpu_set_sys_reg(cpu,HV_SYS_REG_SCTLR_EL1,0x30d00800),"Set bootstrap SCTLR");
        check(hv_vcpu_set_sys_reg(cpu,HV_SYS_REG_VBAR_EL1,scratch+0x800),"Set bootstrap vectors");
        check(hv_vcpu_set_reg(cpu,HV_REG_PC,scratch),"Set bootstrap PC");
        check(hv_vcpu_set_reg(cpu,HV_REG_CPSR,0x3c5),"Set bootstrap EL1h");
        check(hv_vcpu_run(cpu),"Run GIC bootstrap");
        uint64_t pc=0,group=0,el=0;
        check(hv_vcpu_get_reg(cpu,HV_REG_PC,&pc),"Read bootstrap PC");
        check(hv_vcpu_get_reg(cpu,HV_REG_X4,&group),"Read private group assignment");
        check(hv_vcpu_get_reg(cpu,HV_REG_X1,&el),"Read bootstrap EL");
        if(exit->reason!=HV_EXIT_REASON_EXCEPTION || (exit->exception.syndrome>>26)!=0x16 ||
           (exit->exception.syndrome&0xffff)!=0xfffc || pc!=scratch+n*4 || group!=UINT32_MAX || el!=4)
            throw std::runtime_error("GIC prefirmware initialization did not complete from EL1");
        if(index==0) for(unsigned i=0;i<ids.size();++i) {
            uint64_t value=0;check(hv_vcpu_get_reg(cpu,static_cast<hv_reg_t>(HV_REG_X5+i),&value),"Read GIC identity");
            ids[i]=uint32_t(value);
        }
        restore();
    } catch(...) {try{restore();}catch(...){}throw;}
    uint64_t pfr=0;
    check(hv_vcpu_get_sys_reg(cpu,HV_SYS_REG_ID_AA64PFR0_EL1,&pfr),"Read Hyper-V CPU identification");
    check(hv_vcpu_set_sys_reg(cpu,HV_SYS_REG_ID_AA64PFR0_EL1,(pfr&~0xf00ull)|0x100),"Set Hyper-V EL2 identification");
    return ids;
}
}
