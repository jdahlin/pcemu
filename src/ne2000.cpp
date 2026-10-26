// The NE2000 is a programmed-I/O Ethernet board: guest drivers use the DP8390
// Network Interface Controller (NIC) registers to configure a station (MAC, media access control)
// address, receive ring and interrupts. Remote direct memory access (DMA) moves bytes between CPU
// I/O ports and board memory, not the machine's system RAM. Local DMA sends a contiguous transmit
// buffer or fills a receive ring in 256-byte pages. A register page selector reuses offsets;
// direction also changes register meaning. All offsets below are relative to the board's fixed I/O
// base (300h).
//
// The board exposes a duplicated station-address PROM (programmable read-only
// memory) and 16 KiB packet RAM. The guest uploads a frame through remote DMA,
// sets transmit page/count and issues TXP. Host frames pass destination filtering
// and are stored with status, next-page pointer, length and a generated Ethernet
// frame check sequence (FCS). CURR advances while BNRY protects unread packets.
// Interrupt Status latches events; Interrupt Mask controls the active-high INT
// output without clearing them. The machine wires that output to IRQ10; the
// network backend drains/fills host frame queues outside this controller.
//
// IMPLEMENTATION NOTE: Transmits complete synchronously, with no collision,
// carrier, bus arbitration, first-in/first-out (FIFO) timing or retransmission model. Frames are
// padded to 60 bytes excluding FCS; host acceptance/queue caps are emulator policy. Reset clears
// RAM, filters and queues and restores our fixed MAC address, while preserving host traffic
// counters. Unimplemented/reserved register reads are zero except reserved page 3/absent board
// memory, which return FFh. COMPATIBILITY: Any TCR loopback mode loops through the same receiver
// regardless of DCR.LS; RCR.PRO bypasses every destination filter; monitor mode drops without
// tallying; Page 2 remote/local-next diagnostic pointers expose address high bytes
// and address counters return zero. Board Reset reads reset state and writes set
// RST, rather than modeling a physical reset-latch pulse. DCR.WTS consumes an
// unused upper byte lane on byte I/O; other DCR controls are stored only.
// FIXME: Implement DCR byte ordering/send-packet DMA, TCR CRC/loopback controls,
// monitor tallying, physical-only PRO filtering and diagnostic register state to
// match the cited register definitions. Preserve these guest-visible differences
// in this documentation/naming migration. Counter saturation at C0h and overflow
// reset-status behavior also remain unmodeled (10.3 ISR and Network Tally Counters).
// National Semiconductor DP8390D/NS32490D, July 1995, TL/F/8582, sections 7-11.
// Original manufacturer datasheet (archived mirror, PDF pages equal printed pages):
// https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=17
// https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=22
// https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=25
// https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=29
// NE2000-specific board conventions have an implementation reference, not a
// separate board specification, in Linux ne.c (PROM signatures/data/reset/RAM):
// https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/8390/ne.c#L138

#include "ne2000.hpp"
#include <algorithm>
#include <cstring>
namespace pc {
// Construct a reset board/controller with the emulator station address.
// DP8390D 11.0 Initialization Procedures.
// https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=29
NE2000::NE2000() {
    reset();
}
// Reset registers, PROM/RAM and outgoing queue; host traffic counters are retained.
// IMPLEMENTATION NOTE: Memory/address clearing is board policy, not the chip reset bit table.
// DP8390D 11.0 Initialization Procedures.
// https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=29
void NE2000::reset() {
    command = StopController | AbortRemoteDma;
    isr = ResetStatus;
    imr = 0;
    rcr = tcr = dcr = 0;
    remote = remoteCount = transmitCount = local = 0;
    start = stop = boundary = current = transmitPage = 0;
    transmitStatus = receiveStatus = 0;
    tally.fill(0);
    multicast.fill(0);
    outgoing.clear();
    memory.fill(0);
    physical = DefaultStationAddress;
    // NE2000's 16-byte station PROM is duplicated on the 16-bit data bus.
    for (unsigned i = 0; i < StationPromBytes; ++i) {
        uint8_t b = i < EthernetAddressBytes ? DefaultStationAddress[i]
                                             : (i >= PromSignatureOffset ? PromSignatureByte : 0);
        memory[i * 2] = memory[i * 2 + 1] = b;
    }
}
// Read board PROM/RAM; absent locations float high (FFh) by emulator policy.
// NE2000 board map; not a DP8390 on-chip memory specification.
// https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/8390/ne.c#L145
// DP8390D 6.0 Direct Memory Access Control.
// https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=7
uint8_t NE2000::read_buffer_memory(uint16_t a) const {
    return a < StationPromBusBytes || (a >= PacketRamStart && a < PacketRamEnd) ? memory[a] : 0xff;
}
// Write populated packet RAM only; PROM/absent writes are ignored.
// NE2000 board map; not a DP8390 on-chip memory specification.
// https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/8390/ne.c#L145
// DP8390D 6.0 Direct Memory Access Control.
// https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=7
void NE2000::write_buffer_memory(uint16_t a, uint8_t v) {
    if (a >= PacketRamStart && a < PacketRamEnd)
        memory[a] = v;
}
// Advance remote address with ring wrap, decrement byte count and latch completion.
// DP8390D 9.0 Remote DMA; 10.7 Remote DMA Registers.
// https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=28
void NE2000::advance_remote_dma() {
    ++remote;
    if (stop > start && remote == uint16_t(stop) * BufferPageBytes)
        remote = uint16_t(start) * BufferPageBytes;
    if (remoteCount && --remoteCount == 0)
        isr |= RemoteDmaCompleteInterrupt;
}
// Return the next remote-read byte and consume it; inactive reads return FFh.
// DP8390D 9.0 Remote DMA, Remote Read.
// https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=13
uint8_t NE2000::read_remote_dma_byte() {
    if (!remoteCount || (command & RemoteDmaCommandMask) != RemoteRead)
        return 0xff;
    uint8_t v = read_buffer_memory(remote);
    advance_remote_dma();
    return v;
}
// Consume a remote-write byte only while the command/count are active.
// DP8390D 9.0 Remote DMA, Remote Write.
// https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=13
void NE2000::write_remote_dma_byte(uint8_t v) {
    if (!remoteCount || (command & RemoteDmaCommandMask) != RemoteWrite)
        return;
    write_buffer_memory(remote, v);
    advance_remote_dma();
}
// Read the selected register page or little-endian data-port lanes; reset reads reset the board.
// Board reset/data-port conventions:
// https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/8390/ne.c#L138
// DP8390D 10.2 Register Address Assignments.
// https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=17
uint32_t NE2000::read_register(uint16_t p, int w) {
    unsigned r = p - base;
    if (r >= Register::DataRegister && r <= Register::DataWindowLastRegister) {
        uint32_t v = 0;
        for (int i = 0; i < w; ++i)
            v |= uint32_t(read_remote_dma_byte()) << (8 * i);
        // WTS selects word-wide remote DMA even for a byte host read:
        // the unused upper lane is consumed, rather than read next time.
        if (w == 1 && (dcr & WordTransferSelect))
            read_remote_dma_byte();
        return v;
    }
    if (r == Register::ResetRegister) {
        reset();
        return 0;
    }
    if (r == Register::CommandRegister)
        return command;
    if (r >= RegisterWindowSize)
        return 0xff;
    unsigned page = command >> RegisterPageShift;
    if (page == 1) {
        if (r <= Register::PhysicalAddressLastRegister)
            return physical[r - Register::PhysicalAddressFirstRegister];
        if (r == Register::CurrentPageRegister)
            return current;
        return multicast[r - Register::MulticastAddressFirstRegister];
    }
    if (page == 2) {
        switch (r) {
        case Register::PageStartRegister:
            return start;
        case Register::PageStopRegister:
            return stop;
        case Register::RemoteNextPacketRegister:
            // COMPATIBILITY: Proxy CRDA high byte, not an independent diagnostic pointer.
            return remote >> 8;
        case Register::TransmitPageStartRegister:
            return transmitPage;
        case Register::LocalNextPacketRegister:
            // COMPATIBILITY: Proxy CLDA high byte, not an independent diagnostic pointer.
            return local >> 8;
        case Register::AddressCounterHighRegister:
            return 0;
        case Register::AddressCounterLowRegister:
            return 0;
        case Register::ReceiveConfigurationRegister:
            return rcr;
        case Register::TransmitConfigurationRegister:
            return tcr;
        case Register::DataConfigurationRegister:
            return dcr;
        case Register::InterruptMaskRegister:
            return imr;
        default:
            return 0;
        }
    }
    if (page != 0)
        return 0xff;
    switch (r) {
    case Register::CurrentLocalAddressLowRegister:
        return local & 255;
    case Register::CurrentLocalAddressHighRegister:
        return local >> 8;
    case Register::BoundaryRegister:
        return boundary;
    case Register::TransmitStatusRegister:
        return transmitStatus;
    case Register::CollisionCountRegister:
    case Register::FifoRegister:
        return 0;
    case Register::InterruptStatusRegister:
        return isr;
    case Register::CurrentRemoteAddressLowRegister:
        return remote & 255;
    case Register::CurrentRemoteAddressHighRegister:
        return remote >> 8;
    case Register::ReceiveStatusRegister:
        return receiveStatus;
    case Register::AlignmentErrorTallyRegister:
    case Register::CrcErrorTallyRegister:
    case Register::MissedPacketTallyRegister: {
        uint8_t v = tally[r - Register::AlignmentErrorTallyRegister];
        tally[r - Register::AlignmentErrorTallyRegister] = 0;
        return v;
    }
    default:
        return 0;
    }
}
// Write the selected page or DMA lanes; CR starts/stops/transmits, ISR acknowledges events.
// Board reset-port writes only latch RST here, as described in the overview.
// DP8390D 10.2 Register Address Assignments; 10.3 Command/Interrupt Status Registers.
// https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=17
void NE2000::write_register(uint16_t p, uint32_t v, int w) {
    unsigned r = p - base;
    if (r >= Register::DataRegister && r <= Register::DataWindowLastRegister) {
        for (int i = 0; i < w; ++i)
            write_remote_dma_byte(v >> (i * 8));
        if (w == 1 && (dcr & WordTransferSelect) && remoteCount)
            advance_remote_dma();
        return;
    }
    if (r == Register::ResetRegister) {
        // COMPATIBILITY: Preserve the existing reset-acknowledge latch behavior.
        isr |= ResetStatus;
        return;
    }
    if (r == Register::CommandRegister) {
        command = uint8_t(v) & ~TransmitPacket;
        if (command & StopController)
            isr |= ResetStatus;
        else if (command & StartController)
            isr &= ~ResetStatus;
        if (((command & RemoteDmaCommandMask) == RemoteRead ||
             (command & RemoteDmaCommandMask) == RemoteWrite) &&
            !remoteCount)
            isr |= RemoteDmaCompleteInterrupt;
        if ((v & TransmitPacket) && !(command & StopController) && (command & StartController))
            transmit_packet();
        return;
    }
    if (r >= RegisterWindowSize)
        return;
    unsigned page = command >> RegisterPageShift;
    if (page == 1) {
        if (r <= Register::PhysicalAddressLastRegister)
            physical[r - Register::PhysicalAddressFirstRegister] = v;
        else if (r == Register::CurrentPageRegister)
            current = v;
        else
            multicast[r - Register::MulticastAddressFirstRegister] = v;
        return;
    }
    if (page != 0)
        return;
    switch (r) {
    case Register::PageStartRegister:
        start = v;
        break;
    case Register::PageStopRegister:
        stop = v;
        break;
    case Register::BoundaryRegister:
        boundary = v;
        break;
    case Register::TransmitPageStartRegister:
        transmitPage = v;
        break;
    case Register::TransmitCountLowRegister:
        transmitCount = (transmitCount & 0xff00) | uint8_t(v);
        break;
    case Register::TransmitCountHighRegister:
        transmitCount = (transmitCount & 255) | uint16_t(v << 8);
        break;
    case Register::InterruptStatusRegister:
        isr &= ~(v & InterruptEventMask);
        break;
    case Register::RemoteStartLowRegister:
        remote = (remote & 0xff00) | uint8_t(v);
        break;
    case Register::RemoteStartHighRegister:
        remote = (remote & 255) | uint16_t(v << 8);
        break;
    case Register::RemoteCountLowRegister:
        remoteCount = (remoteCount & 0xff00) | uint8_t(v);
        break;
    case Register::RemoteCountHighRegister:
        remoteCount = (remoteCount & 255) | uint16_t(v << 8);
        break;
    case Register::ReceiveConfigurationRegister:
        rcr = v;
        break;
    case Register::TransmitConfigurationRegister:
        tcr = v;
        break;
    case Register::DataConfigurationRegister:
        dcr = v;
        break;
    case Register::InterruptMaskRegister:
        imr = v & InterruptEventMask;
        break;
    }
}
// Read TPSR/TBCR data, pad it and complete transmission into loopback or host queue.
// IMPLEMENTATION NOTE: Invalid buffers report ABT/TXE, not a real collision;
// a full host queue drops the frame but still reports PTX for compatibility.
// DP8390D 8.0 Packet Transmission; 10.3 Transmit Status Register.
// https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=12
void NE2000::transmit_packet() {
    unsigned address = unsigned(transmitPage) << BufferPageAddressShift;
    if (transmitCount < EthernetHeaderBytes || transmitCount > MaximumHostFrameBytes ||
        address < PacketRamStart || address + transmitCount > PacketRamEnd) {
        transmitStatus = TransmitAborted;
        isr |= TransmitErrorInterrupt;
        return;
    }
    std::vector<uint8_t> packet(memory.begin() + address, memory.begin() + address + transmitCount);
    packet.resize(std::max<size_t>(MinimumFrameBytes, packet.size()), 0);
    if (tcr & LoopbackModeMask)
        receive_packet(packet.data(), packet.size());
    else if (outgoing.size() < MaximumQueuedFrames)
        outgoing.push_back(std::move(packet));
    transmitStatus = TransmitSuccessful;
    isr |= PacketTransmittedInterrupt;
    ++transmitted;
}
// Filter a host Ethernet frame, reserve ring pages, store header/data/FCS and latch PRX.
// Return false on filtering, invalid ring or lack of space; only resource failures
// increment the host dropped counter. Host input has no preamble or FCS.
// DP8390D 7.0 Packet Reception; 10.9 Multicast Address Registers.
// https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=8
bool NE2000::receive_packet(const uint8_t *p, size_t n) {
    if ((command & StopController) || !(command & StartController) || (rcr & MonitorMode) ||
        n < EthernetHeaderBytes || n > MaximumHostFrameBytes)
        return false;
    bool broadcast = std::all_of(p, p + EthernetAddressBytes, [](uint8_t b) { return b == 255; });
    // The first destination byte's low bit distinguishes group from unicast addresses.
    bool group = p[0] & 1;
    if (!(rcr & PromiscuousPhysical)) {
        if (broadcast) {
            if (!(rcr & AcceptBroadcast))
                return false;
        } else if (group) {
            if (!(rcr & AcceptMulticast))
                return false;
            // Destination bits enter least-significant bit first; top six CRC
            // remainder bits choose one of the 64 multicast filter bits (10.9).
            // https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=28
            uint32_t crc = InitialCrcRemainder;
            for (unsigned i = 0; i < EthernetAddressBytes; ++i)
                for (unsigned bit = 0; bit < 8; ++bit) {
                    bool carry = ((crc >> 31) ^ (p[i] >> bit)) & 1;
                    crc <<= 1;
                    if (carry)
                        crc ^= EthernetCrcPolynomial;
                }
            unsigned index = crc >> MulticastHashShift;
            if (!(multicast[index >> 3] & (1 << (index & 7))))
                return false;
        } else if (std::memcmp(p, physical.data(), EthernetAddressBytes))
            return false;
    }
    if (n < MinimumFrameBytes && !(rcr & AcceptRuntPackets))
        return false;
    if (start < PacketRamStart / BufferPageBytes || stop > PacketRamEnd / BufferPageBytes ||
        start >= stop || current < start || current >= stop || boundary < start ||
        boundary >= stop) {
        ++dropped;
        return false;
    }
    // Reserve the complete ring header, padded frame and FCS before writing;
    // crossing BNRY must leave previously unread packet bytes intact (7.0).
    // https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=8
    size_t bytes = std::max<size_t>(MinimumFrameBytes, n);
    unsigned pages =
        unsigned((bytes + ReceiveRingHeaderBytes + FrameCheckSequenceBytes + BufferPageBytes - 1) /
                 BufferPageBytes);
    unsigned next = current;
    for (unsigned i = 0; i < pages; ++i) {
        if (++next == stop)
            next = start;
        if (next == boundary) {
            isr |= RingOverwriteInterrupt;
            receiveStatus = MissedPacket;
            ++dropped;
            if (++tally[Register::MissedPacketTallyRegister -
                        Register::AlignmentErrorTallyRegister] == TallyInterruptThreshold)
                isr |= CounterOverflowInterrupt;
            return false;
        }
    }
    uint16_t address = uint16_t(current) << BufferPageAddressShift;
    auto put = [&](uint8_t v) {
        memory[address++] = v;
        if (address == uint16_t(stop) * BufferPageBytes)
            address = uint16_t(start) * BufferPageBytes;
    };
    receiveStatus = ReceiveSuccessful | (group ? GroupAddressMatch : 0);
    // Ring header: status, next page, then byte count low/high. Count includes
    // the FCS and excludes these four header bytes (Completion of Reception).
    // https://www.ardent-tool.com/datasheets/NS_DP8390D_NS32490D.pdf#page=10
    put(receiveStatus);
    put(next);
    put((bytes + FrameCheckSequenceBytes) & 255);
    put((bytes + FrameCheckSequenceBytes) >> 8);
    for (size_t i = 0; i < bytes; ++i)
        put(i < n ? p[i] : 0);
    uint32_t crc = InitialCrcRemainder;
    for (size_t i = 0; i < bytes; ++i) {
        crc ^= i < n ? p[i] : 0;
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ ((crc & 1) ? ReflectedEthernetCrcPolynomial : 0);
    }
    crc = ~crc;
    for (unsigned i = 0; i < FrameCheckSequenceBytes; ++i)
        put(crc >> (8 * i));
    current = next;
    local = address;
    isr |= PacketReceivedInterrupt;
    ++received;
    return true;
}
} // namespace pc
