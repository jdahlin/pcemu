// Intel's 8237A direct memory access (DMA) controller transfers bytes between RAM
// and peripherals without CPU load/store instructions. Each channel holds a
// current address and count plus base values for automatic reinitialization.
// Peripherals drive DMA Request (DREQ); a transfer acknowledgment consumes one
// byte, changes the address, and decrements the count. A count of zero describes
// one final byte: the decrement to FFFFh sets Terminal Count (TC). Software reads
// TC through the status register; that read clears its latched TC indications.
//
// The primary PC/AT controller has four byte channels on ports 00h..0Fh. Paired
// low/high register accesses share one byte pointer across all channels. The
// board's external page latches extend each 16-bit address to 24 bits; address
// overflow wraps within its 64 KiB page. Machine wiring owns those page ports,
// RAM accesses and peripheral scheduling. This model has no host audio service.
//
// IMPLEMENTATION NOTE: Peripheral callbacks acknowledge one byte at a time;
// bus arbitration, electrical timing, memory refresh and simultaneous requests
// are not simulated. Demand/single/block service share the byte acknowledgment
// path. Memory-to-memory, verify, RAM-write and cascade cycles are unsupported
// and cannot accidentally consume playback RAM. The secondary word controller
// is not modeled. Reset masks every channel and clears control state; constructor
// data registers start at zero as emulator policy, not specified power-on state.
// Intel 8237A High Performance Programmable DMA Controller, 231466, Sep. 1993,
// Register Description, Programming and Mode Definitions.
// https://openwatcom.org/ftp/devel/docs/231466.pdf#page=7
#include "i8237.hpp"
namespace pc {
// Reset masks/software requests/TC/byte pointer as specified by Master Clear, preserving
// address/count registers and externally driven DREQ levels.
// Intel 231466, Software Commands: Master Clear.
// https://openwatcom.org/ftp/devel/docs/231466.pdf#page=9
void I8237Device::master_clear() {
    state.command=state.softwareRequest=state.terminalCount=0;
    state.mask=AllChannels;
    state.highByte=false;
}
// Read current registers through the shared byte pointer, or sample request/TC
// status and clear TC only. Unspecified read offsets return an open-bus FFh.
// Intel 231466, Current Address/Word Count and Status Register.
// https://openwatcom.org/ftp/devel/docs/231466.pdf#page=7
// https://openwatcom.org/ftp/devel/docs/231466.pdf#page=9
uint8_t I8237Device::read_register(unsigned offset) {
    if(offset<8) {
        const auto &channel=state.channels[offset/2];
        uint16_t value=(offset&1)?channel.count:channel.address;
        uint8_t result=state.highByte?value>>8:value;
        state.highByte=!state.highByte;
        return result;
    }
    if(offset==unsigned(Register::CommandStatus)) {
        uint8_t result=state.terminalCount|((state.hardwareRequest|state.softwareRequest)<<4);
        state.terminalCount=0;
        return result;
    }
    return 0xff;
}
// Write base/current data in the selected byte order or update control registers.
// First-byte writes do not reset the shared pointer; Clear Byte Pointer does.
// Intel 231466, Register Description, Figure 5 and Software Commands.
// https://openwatcom.org/ftp/devel/docs/231466.pdf#page=7
// https://openwatcom.org/ftp/devel/docs/231466.pdf#page=8
// https://openwatcom.org/ftp/devel/docs/231466.pdf#page=9
void I8237Device::write_register(unsigned offset,uint8_t value) {
    if(offset<8) {
        auto &channel=state.channels[offset/2];
        auto &current=(offset&1)?channel.count:channel.address;
        auto &base=(offset&1)?channel.baseCount:channel.baseAddress;
        current=state.highByte?(current&0xff)|(uint16_t(value)<<8):(current&0xff00)|value;
        base=current;
        state.highByte=!state.highByte;
        return;
    }
    unsigned channel=value&3;
    switch(Register(offset)) {
    case Register::CommandStatus: state.command=value;break;
    case Register::SoftwareRequest:
        if(value&4)state.softwareRequest|=1<<channel;
        else state.softwareRequest&=~(1<<channel);
        break;
    case Register::SingleMask:
        if(value&4)state.mask|=1<<channel;
        else state.mask&=~(1<<channel);
        break;
    case Register::Mode:state.channels[channel].mode=value&0xfc;break;
    case Register::ClearBytePointer:state.highByte=false;break;
    case Register::MasterClear:master_clear();break;
    case Register::ClearMasks:state.mask=0;break;
    case Register::AllMasks:state.mask=value&AllChannels;break;
    }
}
// Drive DREQ without changing the software request latch or channel mask.
// Intel 231466, DREQ input and Status Register request bits.
// https://openwatcom.org/ftp/devel/docs/231466.pdf#page=2
// https://openwatcom.org/ftp/devel/docs/231466.pdf#page=9
void I8237Device::set_request(unsigned channel,bool asserted) {
    if(channel>=state.channels.size())return;
    if(asserted)state.hardwareRequest|=1<<channel;
    else state.hardwareRequest&=~(1<<channel);
}
// Consume one eligible memory-read DACK cycle. TC occurs on 0000h -> FFFFh;
// auto-initialize reloads base registers, otherwise the channel masks itself.
// Intel 231466, Transfer Types, Autoinitialize and Current Word Count Register.
// https://openwatcom.org/ftp/devel/docs/231466.pdf#page=5
// https://openwatcom.org/ftp/devel/docs/231466.pdf#page=7
std::optional<I8237Device::Transfer> I8237Device::acknowledge_memory_read(unsigned channel) {
    if(channel>=state.channels.size())return {};
    auto &c=state.channels[channel];
    unsigned bit=1<<channel;
    if((state.command&(ControllerDisabled|MemoryToMemory)) || (state.mask&bit) ||
       !((state.hardwareRequest&bit) || ((state.softwareRequest&bit) && (c.mode&ServiceModeMask)==BlockMode)) ||
       (c.mode&TransferTypeMask)!=MemoryRead || (c.mode&ServiceModeMask)==CascadeMode)return {};
    Transfer result{(uint32_t(c.page)<<16)|c.address,c.count==0};
    c.address+= (c.mode&DecrementAddress)?-1:1;
    --c.count;
    if(result.terminalCount) {
        state.terminalCount|=bit;
        state.softwareRequest&=~bit;
        if(c.mode&AutoInitialize) {c.address=c.baseAddress;c.count=c.baseCount;}
        else state.mask|=bit;
    }
    return result;
}
} // namespace pc
