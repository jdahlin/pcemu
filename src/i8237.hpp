#pragma once
#include <array>
#include <cstdint>
#include <optional>
namespace pc {
// Intel 8237A, datasheet 231466, September 1993, Register Description/Programming.
// https://openwatcom.org/ftp/devel/docs/231466.pdf#page=7
// Primary PC/AT byte DMA controller; board-owned page latches extend addresses.
struct I8237Device {
    enum class Register : unsigned {
        CommandStatus = 8, SoftwareRequest = 9, SingleMask = 10, Mode = 11,
        ClearBytePointer = 12, MasterClear = 13, ClearMasks = 14, AllMasks = 15
    };
    // Mode bits: memory-read direction means RAM -> peripheral. Address changes
    // by one byte, with no carry into the external page register. AutoInitialize
    // restores base address/count on terminal count. Cascade has no data cycles.
    // Intel 231466, Mode Register / Figure 5 register addressing.
    // https://openwatcom.org/ftp/devel/docs/231466.pdf#page=8
    static constexpr uint8_t TransferTypeMask=12, MemoryRead=8, AutoInitialize=16,
        DecrementAddress=32, ServiceModeMask=192, BlockMode=128, CascadeMode=192,
        ControllerDisabled=4, MemoryToMemory=1, AllChannels=15;
    struct Channel {
        uint16_t baseAddress=0, baseCount=0, address=0, count=0;
        uint8_t mode=0, page=0;
    };
    struct State {
        std::array<Channel,4> channels{};
        uint8_t command=0, mask=AllChannels, softwareRequest=0, hardwareRequest=0, terminalCount=0;
        bool highByte=false;
    } state;
    struct Transfer { uint32_t address; bool terminalCount; };
    // Clear control logic/software requests/TC and mask channels; retain data/DREQ.
    void master_clear();
    // Read current address/count or status, using one shared low/high byte pointer.
    uint8_t read_register(unsigned offset);
    // Program current/base registers or control logic; offsets are relative to 00h.
    void write_register(unsigned offset,uint8_t value);
    // Drive the peripheral request input; independent of software request/masking.
    void set_request(unsigned channel,bool asserted);
    // Acknowledge one RAM-to-device byte, returning its physical address and TC.
    // Masked/disabled/wrong-direction/cascade requests leave all counters unchanged.
    std::optional<Transfer> acknowledge_memory_read(unsigned channel);
};
} // namespace pc
