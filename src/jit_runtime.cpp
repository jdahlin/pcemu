#include "jit_runtime.hpp"
#include <cstring>
#include <algorithm>
#include <array>
namespace pc::jit_detail {
void nativeFinish(CPU *cpu, int op, uint32_t a, uint32_t b, int w, int condition,
                         uint32_t target) {
    if (op >= 8) {
        auto carry = cpu->flags & CF;
        cpu->alu(op == 8 ? 0 : 5, a, b, w);
        cpu->flags = (cpu->flags & ~CF) | carry;
    } else if (op >= 0)
        cpu->alu(op, a, b, w);
    if (condition >= 0 && cpu->condition(condition))
        cpu->ip = target;
}
// Probe only existing read translations. No page walks, A/D writes, device
// callbacks or architectural mutation may occur before deciding to replay.
static bool warmSystemWord(CPU &cpu, uint32_t linear, uint32_t &value) {
    if ((linear & 4095) > 4092) return false;
    uint32_t physical = linear;
    if (cpu.cr[0] & 0x80000000u) {
        if (cpu.tlbCR3 != cpu.cr[3]) return false;
        const auto page = linear >> 12;
        const auto &entry = cpu.tlb[(page ^ (page >> 10)) & (cpu.tlb.size()-1)];
        if (entry.epoch != cpu.tlbEpoch || entry.tag != page) return false;
        // Descriptor fetches are system accesses even for ring-3 callers.
        physical = entry.base | (linear & 4095);
    }
    if (!cpu.a20) physical &= ~(1u << 20);
    if (uint64_t(physical)+4 > cpu.mem.size() ||
        (physical < 0xc0000 && uint64_t(physical)+4 > 0xa0000)) return false;
    std::memcpy(&value, cpu.mem.data()+physical, 4);
    return true;
}
uint32_t nativeSegment(CPU *cpu, unsigned seg, uint32_t selector, unsigned completed) noexcept {
    // CS is kept interpreted: its fetch context can change inside this block.
    if (seg >= 6 || seg == CS) return 0;
    const auto sel = uint16_t(selector);
    try {
        if (!(cpu->cr[0] & 1) || (cpu->flags & 0x20000) || !(sel & ~3)) {
            cpu->segment(seg, sel); // These cases perform no guest memory access.
        } else {
            const auto base = (sel & 4) ? cpu->ldtBase : cpu->gdtBase;
            const auto limit = (sel & 4) ? cpu->ldtLimit : cpu->gdtLimit;
            const auto offset = uint32_t(sel & ~7);
            uint32_t low, high;
            if (offset+7 > limit || !warmSystemWord(*cpu,base+offset,low) ||
                !warmSystemWord(*cpu,base+offset+4,high)) return 0;
            auto decoded = decodeSegmentDescriptor(seg,sel,low,high);
            cpu->s[seg] = decoded;
        }
        // SS blocks cannot be linked or run inside an unaccounted cached tail.
        if (seg == SS) cpu->interruptBlockedThrough = cpu->instructions + completed;
        return 1;
    } catch (const Fault &) {
        return 0; // Fault delivery occurs after the native frame returns.
    }
}
// Data probes differ from descriptor probes: ordinary accesses obey current
// user/system permissions and writes require an already-dirty translation.
static bool warmDataAddress(const CPU &cpu, uint32_t linear, unsigned width, bool write,
                            uint32_t &physical,bool system=false) {
    if ((linear & 4095) + width > 4096) return false;
    physical = linear;
    if (cpu.cr[0] & 0x80000000u) {
        if (cpu.tlbCR3 != cpu.cr[3]) return false;
        const auto page = linear >> 12;
        const auto &entry = cpu.tlb[(page ^ (page >> 10)) & (cpu.tlb.size()-1)];
        if (entry.epoch != cpu.tlbEpoch || entry.tag != page || (write && !entry.dirty)) return false;
        if (!system && cpu.cpl() == 3 && !cpu.systemAccess &&
            (!(entry.permissions & 4) || (write && !(entry.permissions & 2)))) return false;
        physical = entry.base | (linear & 4095);
    }
    if (!cpu.a20) physical &= ~(1u << 20);
    const uint64_t end = uint64_t(physical) + width;
    return end <= cpu.mem.size() && !(physical < 0xc0000 && end > 0xa0000) &&
           !(physical < 0xe1000000 && end > 0xe0000000);
}
// After the pure proof below, preserve operand-before-immediate page-walk order.
void warmImmediateOperand(CPU &cpu,const NativeMemoryMove &op) {
    uint32_t offset=op.displacement;
    if(op.base>=0)offset+=cpu.r[op.base];
    if(op.index>=0)offset+=cpu.r[op.index]<<op.scale;
    if(!op.address32)offset&=65535;
    cpu.physical(cpu.address(op.segment,offset,op.width));
}
// Group-1 immediates read the operand before fetching the immediate. Prove
// present ordinary RAM and exclude aliases of the later fetch's PDE/PTE writes
// before warming either translation. This probe itself has no side effects.
bool canPrefetchImmediateTail(CPU &cpu,const NativeMemoryMove &op,uint32_t codeIP) noexcept {
    try {
        uint32_t offset=op.displacement,physical;
        if(op.base>=0)offset+=cpu.r[op.base];
        if(op.index>=0)offset+=cpu.r[op.index]<<op.scale;
        if(!op.address32)offset&=65535;
        const auto source=cpu.address(op.segment,offset,op.width);
        if((source&4095)+op.width>4096)return false;
        if(!warmDataAddress(cpu,source,op.width,false,physical)) {
            if(!(cpu.cr[0]&0x80000000u))return false;
            auto raw=[&](uint32_t at,uint32_t &value){
                if(!cpu.a20)at&=~(1u<<20);
                if(uint64_t(at)+4>cpu.mem.size())return false;
                std::memcpy(&value,cpu.mem.data()+at,4);return bool(value&1);
            };
            uint32_t pde,pte;
            if(!raw((cpu.cr[3]&~4095u)+(source>>22)*4,pde) ||
               !raw((pde&~4095u)+((source>>12)&1023)*4,pte))return false;
            if(cpu.cpl()==3 && !cpu.systemAccess && !(pde&pte&4))return false;
            physical=(pte&~4095u)|(source&4095);
            if(!cpu.a20)physical&=~(1u<<20);
            if(uint64_t(physical)+op.width>cpu.mem.size() ||
               (physical<0xc0000 && uint64_t(physical)+op.width>0xa0000))return false;
        }
        if(!(cpu.cr[0]&0x80000000u))return true;
        auto linear=cpu.address(CS,codeIP);
        auto raw=[&](uint32_t at,uint32_t &value){
            if(!cpu.a20)at&=~(1u<<20);
            if(uint64_t(at)+4>cpu.mem.size() || (physical<at+4 && uint64_t(physical)+op.width>at))return false;
            std::memcpy(&value,cpu.mem.data()+at,4);return bool(value&1);
        };
        uint32_t pde,pte;
        if(!raw((cpu.cr[3]&~4095u)+(linear>>22)*4,pde) || !raw((pde&~4095u)+((linear>>12)&1023)*4,pte))return false;
        if(cpu.cpl()==3 && !cpu.systemAccess && !(pde&pte&4))return false;
        auto target=(pte&~4095u)|(linear&4095);
        if(!cpu.a20)target&=~(1u<<20);
        return target<cpu.mem.size() && !(target>=0xa0000 && target<0xc0000);
    } catch(const Fault&) {return false;}
}
uint32_t nativeDescriptorQuery(CPU *cpu,const NativeMemoryMove *source,unsigned selector,unsigned destination,unsigned width,unsigned limitQuery) noexcept {
    if(destination>=8 || (width!=2 && width!=4) || !(cpu->cr[0]&1) || (cpu->flags&0x20000))return 0;
    try {
        if(source){
            uint32_t offset=source->displacement,physical;
            if(source->base>=0)offset+=cpu->r[source->base];
            if(source->index>=0)offset+=cpu->r[source->index]<<source->scale;
            if(!source->address32)offset&=65535;
            if(!warmDataAddress(*cpu,cpu->address(source->segment,offset),2,false,physical))return 0;
            selector=0;std::memcpy(&selector,cpu->mem.data()+physical,2);
        }
        const auto sel=uint16_t(selector),offset=uint16_t(sel&~7u);
        const auto base=(sel&4)?cpu->ldtBase:cpu->gdtBase;
        const auto bound=(sel&4)?cpu->ldtLimit:cpu->gdtLimit;
        bool valid=(sel&~3u) && unsigned(offset)+7<=bound;
        uint32_t low=0,high=0;
        if(valid){
            // Descriptor reads ignore caller U/S permissions but must already
            // be warm RAM. Probe both words before any architectural mutation.
            auto read=[&](uint32_t linear,uint32_t &value){
                uint32_t physical;
                if(!warmDataAddress(*cpu,linear,4,false,physical,true))return false;
                std::memcpy(&value,cpu->mem.data()+physical,4);return true;
            };
            if(!read(base+offset,low)||!read(base+offset+4,high))return 0;
            const auto type=(high>>8)&15;
            const bool systemType=type==1 || type==2 || type==3 || type==9 || type==11 ||
                (!limitQuery && (type==4 || type==5 || type==12));
            const auto dpl=(high>>13)&3;
            const bool visible=(high&0x1c00)==0x1c00 || (dpl>=cpu->cpl() && dpl>=(sel&3));
            valid=visible && ((high&0x1000)||systemType);
        }
        if(valid){
            uint32_t value=limitQuery?((low&65535)|(high&0xf0000)):(high&0x00f0ff00u);
            if(limitQuery && (high&0x800000))value=(value<<12)|4095;
            cpu->set(destination,value,width);
        }
        cpu->flags=(cpu->flags&~ZF)|(valid?ZF:0);
        return 1;
    }catch(const Fault &){return 0;}
}
uint32_t nativeEnter(CPU *cpu,unsigned width,unsigned size,unsigned level) noexcept {
    if((width!=2 && width!=4) || cpu->watchWrite)return 0;
    struct Write {uint32_t physical,value;};
    std::array<Write,32> writes;
    unsigned count=0;level&=31;size&=65535;
    const bool stack32=cpu->s[SS].big;
    uint32_t sp=cpu->r[SP],bp=cpu->r[BP];
    const auto stackMask=stack32?UINT32_MAX:65535u,operandMask=width==4?UINT32_MAX:65535u;
    auto push=[&](uint32_t value){
        const auto offset=(sp-width)&stackMask;uint32_t physical;
        if(count==writes.size() || !warmDataAddress(*cpu,cpu->address(SS,offset,width),width,true,physical))return false;
        if(cpu->codeUsed[physical>>6] || cpu->codeUsed[(physical+width-1)>>6])return false;
        sp=(sp&~stackMask)|offset;writes[count++]={physical,value};return true;
    };
    try {
        if(!push(bp&operandMask))return 0;
        const auto frame=sp&stackMask;
        for(unsigned n=1;n<level;++n){
            const auto frameMask=(!(cpu->cr[0]&1) && !stack32)?65535u:operandMask;
            bp=(bp&~frameMask)|((bp-width)&frameMask);
            uint32_t physical,value=0;
            if(!warmDataAddress(*cpu,cpu->address(SS,bp&((!(cpu->cr[0]&1) && !stack32)?65535u:operandMask),width),width,false,physical))return 0;
            std::memcpy(&value,cpu->mem.data()+physical,width);
            // Reference reads observe earlier pushes, even through partial
            // overlaps or different virtual addresses aliasing the same RAM.
            for(unsigned k=0;k<count;++k){
                const auto begin=std::max(uint64_t(physical),uint64_t(writes[k].physical));
                const auto end=std::min(uint64_t(physical)+width,uint64_t(writes[k].physical)+width);
                for(auto p=begin;p<end;++p){
                    const auto shift=unsigned(p-physical)*8,source=unsigned(p-writes[k].physical)*8;
                    value=(value&~(255u<<shift))|(((writes[k].value>>source)&255u)<<shift);
                }
            }
            if(!push(value))return 0;
        }
        if(level && !push(frame))return 0;
        bp=(bp&~operandMask)|(frame&operandMask);
        sp=(sp&~stackMask)|((sp-size)&stackMask);
        for(unsigned n=0;n<count;++n){
            std::memcpy(cpu->mem.data()+writes[n].physical,&writes[n].value,width);
            cpu->written(writes[n].physical,width);
        }
        cpu->r[BP]=bp;cpu->r[SP]=sp;return 1;
    } catch(const Fault &){return 0;}
}

uint32_t nativeFarPointer(CPU *cpu,const NativeMemoryMove *op,unsigned segment,unsigned completed) noexcept {
    if((op->width!=2 && op->width!=4) || op->reg<0 || op->reg>=8 || segment>=6 || segment==CS)return 0;
    try {
        uint32_t offset=op->displacement;
        if(op->base>=0)offset+=cpu->r[op->base];
        if(op->index>=0)offset+=cpu->r[op->index]<<op->scale;
        if(!op->address32)offset&=65535;
        const auto linear=cpu->address(op->segment,offset,op->width);
        const auto tail=cpu->address(op->segment,(offset+op->width)&(op->address32?UINT32_MAX:65535u),2);
        uint32_t value=0,selector=0,physical;
        if(!warmDataAddress(*cpu,linear,op->width,false,physical))return 0;
        std::memcpy(&value,cpu->mem.data()+physical,op->width);
        if(!warmDataAddress(*cpu,tail,2,false,physical))return 0;
        std::memcpy(&selector,cpu->mem.data()+physical,2);
        // LSS loads SS and its GPR atomically; unlike MOV/POP SS it creates
        // no interrupt shadow. Reuse segment validation, preserving that state.
        const auto shadow=cpu->interruptBlockedThrough;
        if(!nativeSegment(cpu,segment,selector,completed))return 0;
        if(segment==SS)cpu->interruptBlockedThrough=shadow;
        cpu->set(op->reg,value,op->width);
        return 1;
    } catch(const Fault &) {return 0;}
}

uint32_t nativeFarTransfer(CPU *cpu,const NativeFarTransfer *operation) noexcept {
    const unsigned w=operation->width;
    if((w!=2&&w!=4)||(operation->call&&cpu->watchWrite))return 0;
    uint32_t off=operation->offset,sel=operation->selector,sp=cpu->r[SP];
    Segment cs=cpu->s[CS],ss=cpu->s[SS];
    const bool protectedMode=(cpu->cr[0]&1)&&!(cpu->flags&0x20000);
    struct Write {uint32_t physical,value;};
    std::array<Write,35> writes;
    std::array<uint32_t,31> parameters;
    unsigned writeCount=0,writeWidth=w;
    bool inner=false;
    std::array<uint32_t,2> csPhysical{};
    unsigned csWords=0;
    auto read=[&](uint32_t linear,unsigned width,bool system,uint32_t &value,uint32_t *address=nullptr){
        uint32_t physical;
        if(!warmDataAddress(*cpu,linear,width,false,physical,system))return false;
        value=0;std::memcpy(&value,cpu->mem.data()+physical,width);
        if(address)*address=physical;
        return true;
    };
    auto descriptor=[&](uint16_t selector,uint32_t &lo,uint32_t &hi,std::array<uint32_t,2> &physical){
        const auto base=(selector&4)?cpu->ldtBase:cpu->gdtBase;
        const auto limit=(selector&4)?cpu->ldtLimit:cpu->gdtLimit;
        const auto offset=uint32_t(selector&~7);
        return offset+7<=limit && read(base+offset,4,true,lo,&physical[0]) &&
               read(base+offset+4,4,true,hi,&physical[1]);
    };
    auto push=[&](uint32_t value){
        if(writeCount==writes.size() || (protectedMode&&ss.unusable))return false;
        const auto offset=(sp-writeWidth)&(ss.big?UINT32_MAX:65535u);
        sp=ss.big?offset:(sp&0xffff0000u)|offset;
        uint32_t physical;
        if(!warmDataAddress(*cpu,ss.base+offset,writeWidth,true,physical,inner))return false;
        writes[writeCount++]={physical,value};return true;
    };
    try {
        if(operation->indirect) {
            const auto &m=operation->memory;
            uint32_t offset=m.displacement;
            if(m.base>=0)offset+=cpu->r[m.base];
            if(m.index>=0)offset+=cpu->r[m.index]<<m.scale;
            if(!m.address32)offset&=65535;
            const auto linear=cpu->address(m.segment,offset,w);
            const auto tail=cpu->address(m.segment,(offset+w)&(m.address32?UINT32_MAX:65535u),2);
            if(!read(linear,w,false,off)||!read(tail,2,false,sel))return 0;
        }
        const auto selected=uint16_t(sel);
        if(protectedMode) {
            uint32_t lo,hi;
            if(!descriptor(selected,lo,hi,csPhysical))return 0;
            if(!(hi&0x1000)) {
                const auto type=(hi>>8)&15;
                if(!operation->call || (type!=4&&type!=12))return 0;
                writeWidth=type==12?4:2;
                const auto target=uint16_t(lo>>16);
                off=(lo&65535)|(writeWidth==4?(hi&0xffff0000):0);
                uint32_t targetLo,targetHi;
                if(!descriptor(target,targetLo,targetHi,csPhysical))return 0;
                const unsigned oldLevel=cpu->s[CS].selector&3,newLevel=(targetHi>>13)&3;
                cs=decodeSegmentDescriptor(CS,(target&~3u)|newLevel,targetLo,targetHi);
                csWords=2;
                if(newLevel<oldLevel) {
                    if(!cpu->trSelector)return 0;
                    const uint32_t oldSP=cpu->get(SP,writeWidth),oldSS=cpu->s[SS].selector;
                    const auto stackOffset=cpu->tr32?4+newLevel*8:2+newLevel*4;
                    uint32_t newSP,newSS;
                    if(!read(cpu->trBase+stackOffset,cpu->tr32?4:2,true,newSP)||
                       !read(cpu->trBase+stackOffset+(cpu->tr32?4:2),2,true,newSS))return 0;
                    const unsigned count=hi&31;
                    for(unsigned n=0;n<count;++n) {
                        const auto offset=(oldSP+n*writeWidth)&(ss.big?UINT32_MAX:65535u);
                        if(!read(cpu->address(SS,offset),writeWidth,false,parameters[n]))return 0;
                    }
                    uint32_t ssLo,ssHi;std::array<uint32_t,2> ignored;
                    if(!(newSS&~3u)||!descriptor(newSS,ssLo,ssHi,ignored))return 0;
                    ss=decodeSegmentDescriptor(SS,newSS,ssLo,ssHi);
                    sp=cpu->tr32?newSP:(sp&0xffff0000u)|(newSP&65535);
                    inner=true;
                    if(!push(oldSS)||!push(oldSP))return 0;
                    for(unsigned n=count;n>0;--n)if(!push(parameters[n-1]))return 0;
                }
            } else {
                if(!(selected&~3u))return 0;
                cs=decodeSegmentDescriptor(CS,selected,lo,hi);csWords=2;
            }
        } else {
            cs.selector=selected;cs.base=uint32_t(selected)<<4;cs.limit=65535;cs.big=false;cs.unusable=false;
        }
        if(!protectedMode && off>65535)return 0;
        if(operation->call && (!push(cpu->s[CS].selector)||!push(operation->end)))return 0;
        // CPU::segment(CS) rereads this descriptor after the pushes. A stack
        // alias could change it, so such accesses must retain interpreter order.
        for(unsigned n=0;n<writeCount;++n)for(unsigned d=0;d<csWords;++d)
            if(uint64_t(writes[n].physical)<uint64_t(csPhysical[d])+4 &&
               uint64_t(csPhysical[d])<uint64_t(writes[n].physical)+writeWidth)return 0;
        for(unsigned n=0;n<writeCount;++n) {
            const auto physical=writes[n].physical;
            std::memcpy(cpu->mem.data()+physical,&writes[n].value,writeWidth);
            if(cpu->codeUsed[physical>>6] || cpu->codeUsed[(physical+writeWidth-1)>>6])
                cpu->written(physical,writeWidth);
        }
        cpu->r[SP]=sp;if(inner)cpu->s[SS]=ss;cpu->s[CS]=cs;cpu->ip=off;
        return 1;
    } catch(const Fault &) {return 0;}
}

uint32_t nativeFarReturn(CPU *cpu,const NativeFarReturn *operation) noexcept {
    const unsigned w=operation->width;
    if(w!=2 && w!=4)return 0;
    const bool vm=cpu->flags&0x20000,protectedMode=cpu->cr[0]&1;
    // NT IRET is a task switch, not a stack-frame return. Decline before any
    // pops or mutation so the interpreter follows TR's backlink, updates busy
    // bits and sets TS. Intel 253668-085US, 9.4 / Table 9-2:
    // https://cdrdv2-public.intel.com/835754/253668-sdm-vol-3a.pdf#page=279
    if(operation->iret && protectedMode && !vm && (cpu->flags & 0x4000))return 0;
    if(operation->iret && vm && ((cpu->flags>>12)&3)!=3)return 0;
    uint32_t sp=cpu->r[SP],off=0,sel=0,f=cpu->flags;
    std::array<Segment,6> segments;
    std::copy(cpu->s,cpu->s+6,segments.begin());
    auto advance=[&](uint32_t amount,bool wide){
        if(wide)sp+=amount;
        else sp=(sp&0xffff0000u)|((sp+amount)&65535);
    };
    auto pop=[&](uint32_t &value){
        const auto offset=cpu->s[SS].big?sp:sp&65535;
        uint32_t physical;
        if(!warmDataAddress(*cpu,cpu->address(SS,offset,w),w,false,physical))return false;
        value=0;std::memcpy(&value,cpu->mem.data()+physical,w);
        advance(w,cpu->s[SS].big);
        return true;
    };
    auto segment=[&](unsigned index,uint32_t selector,uint32_t flags){
        const auto selected=uint16_t(selector);
        if(!protectedMode || (flags&0x20000)) {
            auto &d=segments[index];d.selector=selected;d.base=uint32_t(selected)<<4;
            d.limit=65535;d.big=false;d.unusable=false;return true;
        }
        if(!(selected&~3))return false;
        const auto base=(selected&4)?cpu->ldtBase:cpu->gdtBase;
        const auto limit=(selected&4)?cpu->ldtLimit:cpu->gdtLimit;
        const auto offset=uint32_t(selected&~7);
        uint32_t low,high;
        if(offset+7>limit || !warmSystemWord(*cpu,base+offset,low) ||
           !warmSystemWord(*cpu,base+offset+4,high))return false;
        segments[index]=decodeSegmentDescriptor(index,selected,low,high);
        return true;
    };
    try {
        if(!pop(off)||!pop(sel)||(operation->iret&&!pop(f)))return 0;
        if((!protectedMode || vm) && off>65535)return 0;
        const auto old=cpu->cpl();
        if(operation->iret && protectedMode && !vm && old==0 && w==4 && (f&0x20000)) {
            uint32_t newSP,newSS,newES,newDS,newFS,newGS;
            if(!pop(newSP)||!pop(newSS)||!pop(newES)||!pop(newDS)||!pop(newFS)||!pop(newGS))return 0;
            f=(f&0x00037fd7)|2;
            if(!segment(CS,sel,f)||!segment(SS,newSS,f)||!segment(ES,newES,f)||
               !segment(DS,newDS,f)||!segment(FS,newFS,f)||!segment(GS,newGS,f))return 0;
            sp=newSP;off&=65535;
        } else {
            if(protectedMode && !vm && (sel&3)>old) {
                if(!operation->iret)advance(operation->extra,cpu->s[SS].big);
                uint32_t newSP,newSS;
                if(!pop(newSP)||!pop(newSS)||!segment(SS,newSS,cpu->flags))return 0;
                sp=w==4?newSP:(sp&0xffff0000u)|(newSP&65535);
                if(!operation->iret)advance(operation->extra,segments[SS].big);
            } else if(!operation->iret)advance(operation->extra,cpu->s[SS].big);
            if(!segment(CS,sel,cpu->flags))return 0;
            if(operation->iret) {
                uint32_t change=w==4?0x00017fd7u:0x7fd7u;
                if(protectedMode && old!=0)change&=~0x3000u;
                if(protectedMode && old>((cpu->flags>>12)&3))change&=~uint32_t(IF);
                f=(cpu->flags&~change)|(f&change)|2;
            }
        }
        // Every stack and descriptor read was an existing warm ordinary RAM
        // hit. No fault, page walk or device callback can occur after this point.
        std::copy(segments.begin(),segments.end(),cpu->s);
        cpu->r[SP]=sp;cpu->flags=f;cpu->ip=off;
        return 1;
    } catch(const Fault &) {return 0;}
}

namespace {
struct StringSpan {uint32_t physical=0,elements=0;};
struct StringSpans {std::array<StringSpan,8> spans;unsigned size=0;};
bool stringSpans(const CPU &cpu,unsigned segment,uint32_t offset,unsigned elements,unsigned width,
                 unsigned addressWidth,bool backwards,bool write,StringSpans &out) {
    const uint32_t mask=addressWidth==4?UINT32_MAX:65535u;
    const uint32_t delta=backwards?uint32_t(-int(width)):width;
    while(elements) {
        if(out.size==out.spans.size())return false;
        const auto linear=cpu.address(segment,offset,width);
        uint32_t physical;
        if(!warmDataAddress(cpu,linear,width,write,physical))return false;
        const auto pageElements=backwards?(linear&4095)/width+1:(4096-(linear&4095))/width;
        const uint64_t wrapElements=backwards?uint64_t(offset)/width+1:(uint64_t(mask)-offset)/width+1;
        const unsigned n=unsigned(std::min<uint64_t>(elements,std::min<uint64_t>(pageElements,wrapElements)));
        if(!n)return false;
        const auto lowOffset=backwards?offset-(n-1)*width:offset;
        cpu.address(segment,lowOffset,n*width);
        const auto low=backwards?physical-(n-1)*width:physical;
        if(uint64_t(low)+n*width>cpu.mem.size())return false;
        if(write) {
            const auto last=low+n*width-1;
            for(uint32_t word=low>>6;word<=last>>6;++word)if(cpu.codeUsed[word])return false;
        }
        out.spans[out.size++]={physical,n};
        offset=(offset+delta*n)&mask;elements-=n;
    }
    return true;
}
struct StringCursor {
    const StringSpans &all;unsigned span=0,index=0,width;bool backwards;
    uint32_t address() const {return all.spans[span].physical+(backwards?uint32_t(-int(width)):width)*index;}
    unsigned available() const {return all.spans[span].elements-index;}
    void consume(unsigned n){index+=n;if(index==all.spans[span].elements){++span;index=0;}}
};
uint32_t stringRead(const CPU &cpu,uint32_t physical,unsigned width) {
    uint32_t value=0;std::memcpy(&value,cpu.mem.data()+physical,width);return value;
}
void stringWrite(CPU &cpu,uint32_t physical,unsigned width,uint32_t value) {
    std::memcpy(cpu.mem.data()+physical,&value,width);
}
}
uint32_t nativeString(CPU *cpu,const NativeString *op) noexcept {
    const auto kind=op->opcode&0xfe;
    if((kind!=0xa4 && kind!=0xa6 && kind!=0xaa && kind!=0xac && kind!=0xae) ||
       (op->width!=1 && op->width!=2 && op->width!=4) ||
       (op->repeat && op->repeat!=0xf2 && op->repeat!=0xf3) || op->sourceSegment>=6)return 0;
    const unsigned aw=op->address32?4:2,width=op->width;
    const auto count=op->repeat?cpu->get(CX,aw):1u;
    const auto batch=std::min(count,4096u);
    if(!batch){cpu->repeatResume.valid=false;cpu->ip=op->end;return 1;}
    const bool source=kind==0xa4 || kind==0xa6 || kind==0xac;
    const bool destination=kind!=0xac;
    const bool write=kind==0xa4 || kind==0xaa;
    if(write && cpu->watchWrite)return 0;
    const bool backwards=cpu->flags&DF;
    const uint32_t delta=backwards?uint32_t(-int(width)):width;
    const auto si=cpu->get(SI,aw),di=cpu->get(DI,aw);
    StringSpans sources,destinations;
    // The complete reference-sized batch must be safe before touching data.
    // In particular, no partial native progress is replayed or counted twice.
    try {
        if(source && !stringSpans(*cpu,op->sourceSegment,si,batch,width,aw,backwards,false,sources))return 0;
        if(destination && !stringSpans(*cpu,ES,di,batch,width,aw,backwards,write,destinations))return 0;
    } catch(const Fault &) {return 0;}
    auto initialFlags=cpu->flags;
    const auto &resume=cpu->repeatResume;
    if(op->repeat && resume.valid && resume.selector==cpu->s[CS].selector && resume.base==cpu->s[CS].base &&
       resume.ip==op->start && resume.lastFlags==cpu->flags && resume.ax==cpu->r[AX] &&
       resume.cx==cpu->r[CX] && resume.si==cpu->r[SI] && resume.di==cpu->r[DI])initialFlags=resume.initialFlags;
    cpu->repeatResume.valid=false;
    StringCursor src{sources,0,0,width,backwards},dst{destinations,0,0,width,backwards};
    unsigned processed=0;bool done=false;
    const auto accumulator=cpu->get(AX,width);
    if(kind==0xac) {
        // Warm ordinary RAM reads have no observable intermediate effects.
        const auto &last=sources.spans[sources.size-1];
        cpu->set(AX,stringRead(*cpu,last.physical+delta*(last.elements-1),width),width);
        processed=batch;
    } else while(processed<batch) {
        if(kind==0xa4) {
            const unsigned n=std::min(src.available(),dst.available()),bytes=n*width;
            const auto s=src.address(),d=dst.address();
            const auto lowS=backwards?s-(n-1)*width:s,lowD=backwards?d-(n-1)*width:d;
            if(uint64_t(lowS)+bytes<=lowD || uint64_t(lowD)+bytes<=lowS)
                std::memcpy(cpu->mem.data()+lowD,cpu->mem.data()+lowS,bytes);
            else for(unsigned i=0;i<n;++i)stringWrite(*cpu,d+delta*i,width,stringRead(*cpu,s+delta*i,width));
            src.consume(n);dst.consume(n);processed+=n;
        } else if(kind==0xaa) {
            const unsigned n=dst.available();const auto d=dst.address();
            if(width==1)std::memset(cpu->mem.data()+(backwards?d-(n-1):d),accumulator,n);
            else for(unsigned i=0;i<n;++i)stringWrite(*cpu,d+delta*i,width,accumulator);
            dst.consume(n);processed+=n;
        } else {
            const auto left=kind==0xa6?stringRead(*cpu,src.address(),width):accumulator;
            const auto right=stringRead(*cpu,dst.address(),width);
            cpu->alu(7,left,right,width);
            if(source)src.consume(1);dst.consume(1);++processed;
            if(op->repeat && bool(cpu->flags&ZF)!=(op->repeat==0xf3)){done=true;break;}
        }
    }
    if(source)cpu->set(SI,si+delta*processed,aw);
    if(destination)cpu->set(DI,di+delta*processed,aw);
    const auto remaining=count-processed;
    if(op->repeat)cpu->set(CX,remaining,aw);
    cpu->ip=op->end;
    if(op->repeat && remaining && !done) {
        cpu->ip=op->start;
        cpu->repeatResume={true,cpu->s[CS].selector,cpu->s[CS].base,op->start,initialFlags,
                           cpu->flags,cpu->r[AX],cpu->r[CX],cpu->r[SI],cpu->r[DI]};
    }
    return 1;
}
uint32_t nativePopMemory(CPU *cpu, const NativeMemoryMove *destination, uint32_t stackOffset,
                         uint32_t destinationOffset) noexcept {
    cpu->nativeMemoryCalls++;
    const unsigned width = destination->width;
    if ((width != 2 && width != 4) || cpu->watchWrite) return 0;
    try {
        uint32_t source, target;
        if (!warmDataAddress(*cpu,cpu->address(SS,stackOffset,width),width,false,source) ||
            !warmDataAddress(*cpu,cpu->address(destination->segment,destinationOffset,width),width,true,target)) return 0;
        if (cpu->codeUsed[target >> 6] || cpu->codeUsed[(target+width-1) >> 6]) return 0;
        // Both probes are side-effect-free. Capture the entire source before
        // the write, including physically aliased or overlapping operands.
        uint32_t value = 0;
        std::memcpy(&value,cpu->mem.data()+source,width);
        std::memcpy(cpu->mem.data()+target,&value,width);
        cpu->written(target,width);
        return 1;
    } catch (const Fault &) {
        return 0; // Architectural exceptions are delivered after frame return.
    }
}
// No C++ exception can unwind through generated ARM64 frames. Decline before
// touching devices or guest data; the dispatcher replays that instruction in
// the interpreter with committed GPRs, flags and its original instruction IP.
uint32_t nativeMemory(CPU *cpu, const NativeMemoryMove *op, uint32_t offset, uint32_t value,
                             uint32_t codePage, uint32_t codeGeneration) {
    cpu->nativeMemoryCalls++;
    auto previousCR2 = cpu->cr[2];
    try {
        if (op->width != 1 && op->width != 2 && op->width != 4) {
            cpu->nativeFallback = true;
            return 0;
        }
        if (op->store && cpu->watchWrite) {
            cpu->nativeFallback = true;
            return 0;
        }
        auto linear = cpu->address(op->segment, offset, op->width);
        if ((linear & 4095) + op->width > 4096) {
            cpu->nativeFallback = true;
            return 0;
        }
        // RMW probes read translation first. A declined device access must
        // replay its read before dirty-bit updates or a write callback.
        const bool rmw = op->delta || op->alu >= 0 || op->exchange || op->bit > 4 || op->invert;
        auto physical = cpu->physical(linear, op->store && !rmw);
        if (uint64_t(physical) + op->width > cpu->mem.size() ||
            (physical < 0xc0000 && uint64_t(physical) + op->width > 0xa0000)) {
            cpu->nativeFallback = true;
            return 0;
        }
        if (op->store) {
            if (rmw) {
                uint32_t original = 0;
                std::memcpy(&original, cpu->mem.data() + physical, op->width);
                // The operand can alias its own PTE. Capture it before the
                // write probe updates D in that same guest word.
                physical = cpu->physical(linear, true);
                cpu->nativeValue = original;
                if (op->invert) value = ~original;
                else if (op->bit > 4) {
                    auto mask = 1u << (value & (op->width * 8 - 1));
                    value = op->bit == 5 ? original | mask : op->bit == 6 ? original & ~mask : original ^ mask;
                } else if (op->delta) value = original + uint32_t(op->delta);
                else if (!op->exchange) switch (op->alu) {
                    case 0: value = original + value; break;
                    case 1: value = original | value; break;
                    case 2: value = original + value + (cpu->flags & CF); break;
                    case 3: value = original - value - (cpu->flags & CF); break;
                    case 4: value = original & value; break;
                    case 5: value = original - value; break;
                    case 6: value = original ^ value; break;
                    default: cpu->nativeFallback = true; return 0;
                }
            }
            if (op->width == 4)
                std::memcpy(cpu->mem.data() + physical, &value, 4);
            else if (op->width == 2)
                std::memcpy(cpu->mem.data() + physical, &value, 2);
            else
                cpu->mem[physical] = uint8_t(value);
            cpu->written(physical, op->width);
        } else {
            value = 0;
            if (op->width == 4)
                std::memcpy(&value, cpu->mem.data() + physical, 4);
            else if (op->width == 2)
                std::memcpy(&value, cpu->mem.data() + physical, 2);
            else
                value = cpu->mem[physical];
            cpu->nativeValue = value;
        }
        return cpu->generation[codePage] == codeGeneration ? 1 : 2;
    } catch (...) {
        cpu->cr[2] = previousCR2;
        cpu->nativeFallback = true;
        return 0;
    }
}
} // namespace pc::jit_detail
