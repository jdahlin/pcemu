#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <vector>
namespace pc {
// ISA NE2000 board with National DP8390D NIC, July 1995, TL/F/8582.
// Original manufacturer datasheet (archived mirror):
// https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf
// Board PROM/data/reset conventions are independently described by Linux ne.c:
// https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/8390/ne.c#L138
struct NE2000 {
    // IMPLEMENTATION NOTE: Fixed board I/O address; machine wiring routes IRQ10.
    static constexpr uint16_t base = 0x300;
    // DP8390D 10.2 Register Address Assignments. Offsets are relative to base;
    // page and direction select different registers at the same offset.
    // https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=17
    enum Register : unsigned {
        CommandRegister = 0, // CR is available on every register page.
        CurrentLocalAddressLowRegister = 1,
        CurrentLocalAddressHighRegister = 2, // Page 0 read, CLDA0/1.
        PageStartRegister = 1,
        PageStopRegister = 2, // Page 0 write, Page 2 read; ring bounds in 256-byte pages.
        BoundaryRegister = 3, // BNRY: protects pages not yet consumed by the guest.
        TransmitStatusRegister = 4,
        TransmitPageStartRegister = 4, // TSR read, TPSR write.
        CollisionCountRegister = 5,
        TransmitCountLowRegister = 5, // NCR read, TBCR0 write.
        FifoRegister = 6,
        TransmitCountHighRegister = 6, // FIFO diagnostic read, TBCR1 write.
        InterruptStatusRegister = 7,   // ISR: latched events, write one to acknowledge.
        CurrentRemoteAddressLowRegister = 8,
        CurrentRemoteAddressHighRegister = 9, // CRDA0/1 read.
        RemoteStartLowRegister = 8,
        RemoteStartHighRegister = 9, // RSAR0/1 write: next transfer address.
        RemoteCountLowRegister = 10,
        RemoteCountHighRegister = 11, // RBCR0/1 write: bytes left.
        ReceiveStatusRegister = 12,
        ReceiveConfigurationRegister = 12, // RSR read, RCR write.
        TransmitConfigurationRegister = 13,
        DataConfigurationRegister = 14,
        InterruptMaskRegister = 15, // TCR/DCR/IMR: Page 0 write, Page 2 diagnostic read.
        AlignmentErrorTallyRegister = 13,
        CrcErrorTallyRegister = 14,
        MissedPacketTallyRegister = 15, // Page 0 read clears count.
        PhysicalAddressFirstRegister = 1,
        PhysicalAddressLastRegister = 6,   // Page 1 PAR0..5, destination MAC filter.
        CurrentPageRegister = 7,           // Page 1 CURR: next receive packet's first page.
        MulticastAddressFirstRegister = 8, // Page 1 MAR0..7: 64-bit multicast hash filter.
        RemoteNextPacketRegister = 3,
        LocalNextPacketRegister = 5, // Page 2 diagnostic pointers.
        AddressCounterHighRegister = 6,
        AddressCounterLowRegister = 7, // Page 2 diagnostic count.
        // NE2000 board ports, not DP8390 internal register assignments.
        // https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/8390/ne.c#L138
        DataRegister = 0x10,
        DataWindowLastRegister = 0x17, // Aliased remote-DMA data ports.
        ResetRegister = 0x1f, // Board reset handshake, described in implementation overview.
    };
    // CR STOP/START/TXP, encoded remote-DMA command and page selector.
    // Direct memory access (DMA) here transfers board RAM, not guest system RAM.
    // DP8390D 10.3 Command Register.
    // https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=19
    static constexpr uint8_t StopController = 1, StartController = 2, TransmitPacket = 4,
                             RemoteDmaCommandMask = 0x38, RemoteRead = 0x08, RemoteWrite = 0x10,
                             AbortRemoteDma = 0x20;
    static constexpr unsigned RegisterPageShift = 6, RegisterWindowSize = 16;
    // ISR/IMR bits [6:0] latch/enable matching events; RST is status-only, not
    // write-one-to-clear and cannot assert INT. CNT signals a tally's top bit.
    // https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=20
    static constexpr uint8_t PacketReceivedInterrupt = 1, PacketTransmittedInterrupt = 2,
                             TransmitErrorInterrupt = 8, RingOverwriteInterrupt = 0x10,
                             CounterOverflowInterrupt = 0x20, RemoteDmaCompleteInterrupt = 0x40,
                             ResetStatus = 0x80, InterruptEventMask = 0x7f;
    // DCR.WTS consumes word-wide DMA; TCR.LB[1:0] selects loopback instead of wire output.
    // https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=22
    // https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=23
    static constexpr uint8_t WordTransferSelect = 1, LoopbackModeMask = 6;
    // TSR.PTX means successful transmit, ABT means aborted (also used for host validation failure).
    // https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=24
    static constexpr uint8_t TransmitSuccessful = 1, TransmitAborted = 8;
    // RCR.AR accepts runts, AB broadcast, AM hash-filtered multicast, PRO unmatched
    // physical destinations, MON monitors without buffering. Compatibility gaps are in the
    // overview. https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=25
    static constexpr uint8_t AcceptRuntPackets = 2, AcceptBroadcast = 4, AcceptMulticast = 8,
                             PromiscuousPhysical = 0x10, MonitorMode = 0x20;
    // RSR.PRX means intact packet, MPA lack of buffers, PHY multicast/broadcast destination.
    // https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=26
    static constexpr uint8_t ReceiveSuccessful = 1, MissedPacket = 0x10, GroupAddressMatch = 0x20;
    // Board RAM is 16 KiB at 4000h..7fffh; PROM is exposed as 16 duplicated bytes.
    // PROM bytes 14/15 contain 57h signatures that let drivers recognize an NE2000.
    // https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/8390/ne.c#L145
    // https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/8390/ne.c#L167
    static constexpr unsigned PacketRamStart = 0x4000, PacketRamEnd = 0x8000, StationPromBytes = 16,
                              StationPromBusBytes = StationPromBytes * 2, PromSignatureOffset = 14;
    static constexpr uint8_t PromSignatureByte = 0x57;
    // IMPLEMENTATION NOTE: Locally administered unicast address, with ASCII PCEM
    // bytes and suffix 01; this is a host identity, not a vendor-assigned hardware ID.
    static constexpr std::array<uint8_t, 6> DefaultStationAddress{0x02, 0x50, 0x43,
                                                                  0x45, 0x4d, 0x01};
    // DP8390D ring pages and tally threshold, sections 10.6 and Network Tally Counters.
    // https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=27
    // https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=29
    static constexpr unsigned BufferPageBytes = 256, BufferPageAddressShift = 8;
    static constexpr uint8_t TallyInterruptThreshold = 0x80;
    // Ethernet destination+source+type/length header, minimum frame without FCS,
    // and the four-byte frame check sequence (cyclic redundancy check, CRC). Ring header is
    // status, next page and little-endian length including FCS but excluding header.
    // DP8390D 4.0 Encapsulation and 7.0 Completion of Reception.
    // https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=3
    // https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=10
    static constexpr size_t EthernetAddressBytes = 6, EthernetHeaderBytes = 14,
                            MinimumFrameBytes = 60, FrameCheckSequenceBytes = 4,
                            ReceiveRingHeaderBytes = 4;
    // IMPLEMENTATION NOTE: Host frame and outgoing queue limits; 1518 is the
    // retained acceptance cap even though the host interface omits the FCS.
    static constexpr size_t MaximumHostFrameBytes = 1518, MaximumQueuedFrames = 64;
    // Ethernet CRC-32 generator x^32+x^26+...+1; normal form for the DP8390
    // destination hash, reflected form for least-significant-bit-first frame FCS.
    // Initial all-ones remainder; FCS complements it, multicast hash does not.
    // DP8390D 3.0 CRC Generator/Checker and 10.9 Multicast Address Registers.
    // https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=2
    // https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=28
    static constexpr uint32_t EthernetCrcPolynomial = 0x04c11db7,
                              ReflectedEthernetCrcPolynomial = 0xedb88320,
                              InitialCrcRemainder = 0xffffffff;
    // Select the top six CRC bits, giving one bit in the eight-byte MAR filter.
    static constexpr unsigned MulticastHashShift = 26;
    std::array<uint8_t, PacketRamEnd> memory{};
    std::array<uint8_t, 6> physical = DefaultStationAddress;
    std::array<uint8_t, 8> multicast{};
    std::array<uint8_t, 3> tally{};
    uint8_t command = StopController | AbortRemoteDma, start = 0, stop = 0, boundary = 0,
            current = 0;
    uint8_t transmitPage = 0, transmitStatus = 0, receiveStatus = 0;
    uint8_t isr = ResetStatus, imr = 0, rcr = 0, tcr = 0, dcr = 0;
    uint16_t remote = 0, remoteCount = 0, transmitCount = 0, local = 0;
    uint64_t transmitted = 0, received = 0, dropped = 0;
    // A bounded wire queue is drained by the host outside the CPU hot loop.
    std::deque<std::vector<uint8_t>> outgoing;
    // Create a reset controller, board RAM and station PROM.
    NE2000();
    // Reset controller/board buffers and rebuild the fixed station PROM.
    void reset();
    // Return active-high INT when any latched event has its matching IMR bit set.
    // DP8390D 10.3 Interrupt Status/Mask Registers.
    // https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=20
    bool interrupt_level() const {
        return (isr & imr & InterruptEventMask) != 0;
    }
    // Read a page/direction-selected register, data port or reset handshake.
    uint32_t read_register(uint16_t port, int width);
    // Write a controller register or stream bytes through remote DMA.
    void write_register(uint16_t port, uint32_t value, int width);
    // Filter a host frame and publish its data/status/FCS into the receive ring.
    bool receive_packet(const uint8_t *data, size_t length);

  private:
    // Read station PROM or packet RAM; unpopulated locations return FFh.
    uint8_t read_buffer_memory(uint16_t address) const;
    // Write packet RAM only, protecting PROM and unpopulated addresses.
    void write_buffer_memory(uint16_t address, uint8_t value);
    // Read one remote-DMA byte if command/count permit, then advance DMA.
    uint8_t read_remote_dma_byte();
    // Write one remote-DMA byte if command/count permit, then advance DMA.
    void write_remote_dma_byte(uint8_t value);
    // Increment/wrap remote address, decrement remaining bytes and latch RDC at zero.
    void advance_remote_dma();
    // Validate and synchronously loop back or enqueue the transmit RAM frame.
    void transmit_packet();
};
} // namespace pc
