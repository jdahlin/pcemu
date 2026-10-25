#include "arm_hyperv_reference_clock.hpp"
#include <algorithm>
#include <limits>
namespace pc {
namespace {
void put(std::span<uint8_t> bytes, size_t at, uint64_t value, unsigned size) {
    for (unsigned i=0;i<size;++i) bytes[at+i]=uint8_t(value>>(8*i));
}
}
std::span<uint8_t> ArmHyperVReferenceClock::page(uint64_t value) const {
    auto address=value&~uint64_t(4095);
    if(address<base || address-base>ram.size() || ram.size()-(address-base)<4096) return {};
    return ram.subspan(size_t(address-base),4096);
}
bool ArmHyperVReferenceClock::mapping(Sample s,uint64_t &scale,uint64_t &offset) {
    // A 64-bit fixed-point multiplier cannot represent ratios >= 1.
    if(s.frequency<=10000000) return false;
    scale=uint64_t((__uint128_t(10000000)<<64)/s.frequency);
    auto normalized=uint64_t((__uint128_t(s.counter)*scale)>>64);
    __int128_t difference=__int128_t(s.reference100ns)-normalized;
    if(difference<std::numeric_limits<int64_t>::min() || difference>std::numeric_limits<int64_t>::max()) return false;
    offset=uint64_t(int64_t(difference));
    return true;
}
void ArmHyperVReferenceClock::publish(uint64_t scale,uint64_t offset) {
    auto p=page(registered);
    // All VPs are stopped; invalidate first and publish sequence last. The
    // caller must finish any backend counter changes before resuming a VP.
    put(p,0,0,4);
    std::fill(p.begin()+4,p.end(),0);
    put(p,8,scale,8);put(p,16,offset,8);
    if(!++generation) ++generation;
    put(p,0,generation,4);valid=true;
}
void ArmHyperVReferenceClock::invalidate() {
    if(registered&1) put(page(registered),0,0,4);
    valid=false;
}
bool ArmHyperVReferenceClock::configure(uint64_t value,Sample sample) {
    if(value&0xffe) return false;
    uint64_t scale=0,offset=0;
    if((value&1) && (page(value).empty() || !mapping(sample,scale,offset))) return false;
    invalidate();registered=value;
    if(value&1) publish(scale,offset);
    return true;
}
bool ArmHyperVReferenceClock::update(Sample sample) {
    if(!(registered&1)) return false;
    uint64_t scale=0,offset=0;
    if(!mapping(sample,scale,offset)){invalidate();return false;}
    publish(scale,offset);return true;
}
}
