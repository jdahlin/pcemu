#pragma once
#include "cpu.hpp"
namespace pc::jit_detail {
// Native lowering matches the binary64 execution model in cpu_x87.cpp. Keep
// environment, transcendental and extended-memory forms in the decoded helper.
// Intel SDM Vol. 1 ch. 8; Vol. 2 FADD/FLD/FST/FISTP/FXCH entries:
// https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf#page=203
// https://cdrdv2-public.intel.com/835757/325383-sdm-vol-2abcd.pdf#page=494
inline bool nativeX87Form(const CPU::DecodedX87 &x) {
    unsigned op=x.opcode,m=x.modrm,g=(m>>3)&7;
    if(m<0xc0) {
        if(!(op&1))return true; // Real/integer arithmetic and comparisons.
        if(op==0xd9)return g==0||g==2||g==3||g==5||g==7;
        if(op==0xdd)return g==0||g==2||g==3||g==7;
        return (op==0xdb||op==0xdf)&&(g==0||g==2||g==3);
    }
    if(op==0xd8||op==0xdc||op==0xde)return true;
    if(op==0xd9)return g<2||m==0xd0||m==0xe0||m==0xe1||m==0xe4||
        (m>=0xe8&&m<=0xee)||m==0xf6||m==0xf7||m==0xfa||m==0xfc;
    if(op==0xdd)return g==0||g==2||g==3||g==4||g==5;
    return (op==0xda&&m==0xe9)||(op==0xdf&&m==0xe0);
}
inline unsigned nativeX87Width(const CPU::DecodedX87 &x) {
    unsigned op=x.opcode,g=(x.modrm>>3)&7;
    if((op==0xd9&&(g==5||g==7))||(op==0xdd&&g==7)||op==0xde||op==0xdf)return 2;
    return op==0xdc||op==0xdd?8:4;
}
}
