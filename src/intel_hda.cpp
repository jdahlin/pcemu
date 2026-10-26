// Intel ICH9 High Definition Audio (HDA) lets the OS discover an audio codec,
// configure its speaker path with verbs, and play PCM (Pulse Code Modulation)
// samples from guest RAM. A Command Output Ring Buffer (CORB) carries verbs;
// a Response Input Ring Buffer (RIRB) carries their replies. Alternatively the
// driver can send an immediate command through registers. The codec has a root,
// an Audio Function Group (AFG), a Digital-to-Analog Converter (DAC), and a fixed
// speaker pin. The output stream walks a Buffer Descriptor List (BDL), advances
// its Link Position In Buffer (LPIB), and raises completion/error interrupts.
// Register offsets below are relative to the PCI Base Address Register (BAR).
//
// Reference: Intel High Definition Audio Specification, revision 1.0a, June 17,
// 2010 (the published document has no separate document number), chapters 3/7.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=26
// ICH9 PCI identity follows QEMU's ich9-intel-hda discovery convention:
// https://github.com/qemu/qemu/blob/master/hw/audio/intel-hda.c
// This is an original bounded subset, not a claim of full ICH9 conformance.
//
// IMPLEMENTATION NOTE: One output stream, no input/bidirectional streams, stereo
// signed 16-bit 48 kHz PCM only. No codec amplifiers: the guest mixes/attenuates
// samples in software. No MSI, unsolicited events, jack detection or recording.
// PCI power-management capabilities and GCTL flush cycles are unsupported.
// BAR assignment/probing accepts DWORD writes; other BAR write widths are ignored.
// Commands complete synchronously with VPs stopped; the board supplies elapsed
// microseconds and routes active-high (true means asserted) legacy PCI INTx.
// The optional host sink owns audio buffering. Missing sinks discard samples.
// Host scheduling stalls are capped to 100 ms per service to bound work/latency.
// FIXME: Link frame/reset delays, FIFO timing and physical codec electrical
// behavior are not modeled (sections 4.3 and 5.5); stream readiness is immediate.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=97
#include "intel_hda.hpp"
#include <algorithm>
#include <sstream>
#include <vector>
namespace pc {
namespace {
// Controller register map, HDA 1.0a section 3.3, Table 2 (PDF pages 28–29).
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=28
struct Register {
    enum : unsigned {
        // GCAP: tells the driver stream counts and 64-bit addressing support.
        GlobalCapabilities = 0x00,
        // VMIN: minor HDA register-interface version.
        MinorVersion = 0x02,
        // VMAJ: major HDA register-interface version.
        MajorVersion = 0x03,
        // OUTPAY: maximum outbound 16-bit words per 48 kHz frame.
        OutputPayloadCapability = 0x04,
        // INPAY: link response capacity; no capture stream is exposed.
        InputPayloadCapability = 0x06,
        // GCTL: CRST starts/stops the link and resets controller state.
        GlobalControl = 0x08,
        // WAKEEN: permits codec-zero status changes to request service.
        WakeEnable = 0x0c,
        // STATESTS: latched codec enumeration/wake request; write one clears.
        StateChangeStatus = 0x0e,
        // GSTS: flush status is unsupported and reads zero.
        GlobalStatus = 0x10,
        // INTCTL: global/controller/stream interrupt enables.
        InterruptControl = 0x20,
        // INTSTS: derived summary of pending controller/stream causes.
        InterruptStatus = 0x24,
        // WALCLK: elapsed link time in nominal 24 MHz ticks.
        WallClockCounter = 0x30,
        // SSYNC: hold playback until the synchronization bit clears.
        StreamSynchronization = 0x38,
        // CORBLBASE: low aligned address of guest command-ring RAM.
        CommandOutputRingBufferLowerBase = 0x40,
        // CORBUBASE: high 32 address bits of the command ring.
        CommandOutputRingBufferUpperBase = 0x44,
        // CORBWP: last command submitted by guest software.
        CommandOutputRingBufferWritePointer = 0x48,
        // CORBRP: last consumed command; bit 15 resets the pointer.
        CommandOutputRingBufferReadPointer = 0x4a,
        // CORBCTL: command DMA enable and memory-error interrupt enable.
        CommandOutputRingBufferControl = 0x4c,
        // CORBSTS: command-memory error, cleared by writing one.
        CommandOutputRingBufferStatus = 0x4d,
        // CORBSIZE: supported ring sizes and active size selection.
        CommandOutputRingBufferSize = 0x4e,
        // RIRBLBASE: low aligned address of guest response-ring RAM.
        ResponseInputRingBufferLowerBase = 0x50,
        // RIRBUBASE: high 32 address bits of the response ring.
        ResponseInputRingBufferUpperBase = 0x54,
        // RIRBWP: last published response; bit 15 resets the pointer.
        ResponseInputRingBufferWritePointer = 0x58,
        // RINTCNT: response count before interrupt (zero means 256).
        ResponseInterruptCount = 0x5a,
        // RIRBCTL: response DMA and response/error interrupt enables.
        ResponseInputRingBufferControl = 0x5c,
        // RIRBSTS: response/error latches, cleared by writing one.
        ResponseInputRingBufferStatus = 0x5d,
        // RIRBSIZE: supported response sizes and active size selection.
        ResponseInputRingBufferSize = 0x5e,
        // ICW: one codec verb sent without command-ring DMA.
        ImmediateCommand = 0x60,
        // IRR: codec reply to the immediate command.
        ImmediateResponse = 0x64,
        // ICS: result-valid latch; writing busy sends the command.
        ImmediateStatus = 0x68,
        // DPLBASE: aligned position-buffer address and DMA enable bit.
        PositionLowerBase = 0x70,
        // DPUBASE: high 32 bits of the position-buffer address.
        PositionUpperBase = 0x74,
        // WALCLKA: read-only alias of WALCLK for driver polling.
        WallClockAlias = 0x2030,
        // SD0LPIBA: read-only alias of playback position.
        LinkPositionAlias = 0x2084,
        // SD0CTL: stream tag, run/reset and per-cause interrupt enables.
        OutputStreamControl = 0x80,
        // SD0STS: completion/error latches and immediate FIFO-ready state.
        OutputStreamStatus = 0x83,
        // SD0LPIB: bytes transmitted, wrapping at CyclicBufferLength.
        LinkPosition = 0x84,
        // SD0CBL: total bytes in the cyclic audio buffer.
        CyclicBufferLength = 0x88,
        // SD0LVI: zero-based last BDL entry, at least one (two entries).
        LastValidIndex = 0x8c,
        // SD0FIFOS: maximum modeled fetch size in bytes.
        FifoSize = 0x90,
        // SD0FMT: PCM rate, sample width and channel count.
        StreamFormat = 0x92,
        // SD0BDPL: low 32 bits of the aligned descriptor-list address.
        BufferDescriptorListLowerBase = 0x98,
        // SD0BDPU: high 32 bits of the descriptor-list address.
        BufferDescriptorListUpperBase = 0x9c,
    };
};
// PCI type-0 configuration byte offsets, PCI 3.0 section 6.1, Figure 6-1.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=214
struct PciRegister {
    enum : unsigned {
        VendorDeviceIdentity = 0, // Vendor and device identify which driver can bind.
        Command = 4,              // Memory/DMA/INTx controls; next byte is its high half.
        Status = 6,               // Interrupt-pending status; capabilities are absent.
        ClassRevision = 8,        // HDA programming class and ICH9 revision.
        BarLower = 0x10,          // 64-bit memory BAR, low address/type bits.
        BarUpper = 0x14,          // High 32 bits of the same memory BAR.
        SubsystemIdentity = 0x2c, // Board identity repeats the ICH9 identity.
        InterruptPin = 0x3d,      // INTA; machine firmware supplies its GSI route.
    };
};
// Conventional header bytes and the ECAM function extent; extended space is zero.
constexpr unsigned ConfigurationHeaderBytes = 256, ConfigurationFunctionBytes = 4096;
// PCI BAR probing writes all ones; a 64-bit BAR encodes memory type 10b in bits2:1.
constexpr uint32_t BarProbeRequest = 0xffffffffu, Bar64BitMemoryType = 4;
// PCI fields: Command memory/master/INTx-disable, Status interrupt pending.
// PCI 3.0 sections 6.2.1/6.2.2/6.2.5; ICH9 identity follows QEMU.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=218
// https://github.com/qemu/qemu/blob/master/hw/audio/intel-hda.c
constexpr uint32_t Ich9Identity = 0x293e8086, AudioClassRevision = 0x04030003;
constexpr uint16_t MemoryEnable = 2, BusMasterEnable = 4, InterruptDisable = 0x400;
constexpr uint64_t BarBytes = 0x4000, BarMask = ~(BarBytes - 1);
// IMPLEMENTATION NOTE: Board aperture; accesses may never alias RAM or firmware.
constexpr uint64_t BoardMemoryBegin = 0x10000000, BoardMemoryEnd = 0x3f000000;
// GCAP: one output stream [15:12], 64-bit DMA [0]; no input/bidirectional streams.
// OUTPAY: 60 words per link frame; INPAY: 29 words. HDA 3.3.2–3.3.6.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=30
constexpr uint16_t PlaybackCapabilities = 0x1001, OutputPayloadWords = 60, InputPayloadWords = 29;
// GCTL.CRST exits reset. INTCTL.GIE/CIE enable global/controller interrupts;
// INTSTS.GIS/CIS summarize them; bit 0 selects the sole output stream (3.3.7/14/15).
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=32
constexpr uint32_t ControllerResetExit = 1, GlobalInterrupt = 1u << 31,
                   ControllerInterrupt = 1u << 30, OutputStreamInterrupt = 1;
// CORB/RIRB control DMA-enable [1], response interrupt [0]; size capability
// bits advertise 2/16/256 entries and reset selection 2 means 256 (3.3.20–31).
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=38
constexpr uint8_t RingDmaEnable = 2, ResponseInterruptEnable = 1, RingSizeCapabilities = 0x70;
constexpr unsigned RingEntries[] = {2, 16, 256};
constexpr uint16_t RingPointerReset = 0x8000;
// Stream control RUN[1]/SRST[0], IOCE[2]/FEIE[3]/DEIE[4]; status
// BCIS[2]/FIFOE[3]/DESE[4]/FIFORDY[5]. BDL IOC is descriptor DWORD3 bit0.
// HDA 3.3.35–43 and 3.6.2.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=45
constexpr uint8_t StreamReset = 1, StreamRun = 2, BufferCompletion = 4, FifoError = 8,
                  DescriptorError = 16, FifoReady = 32;
// BDL entries are 16 bytes (3.6.3); each stereo16 frame is two 16-bit samples.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=57
constexpr unsigned DescriptorBytes = 16, FrameBytes = 4, SampleRateHz = 48000;
constexpr uint16_t Stereo16Bit48kHz = 0x11; // FMT: 48 kHz base, 16 bits [6:4], two channels [3:0].
// IMPLEMENTATION NOTE: Host time units and maximum catch-up per service call.
constexpr uint64_t MicrosecondsPerSecond = 1000000, MaximumServiceMicroseconds = 100000;
// WALCLK increments at nominal 24 MHz (section 3.3.16).
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=37
constexpr uint64_t WallTicksPerMicrosecond = 24;
// Codec verbs, sections 7.3.3/7.3.4. Twelve-bit verbs carry eight-bit payloads;
// converter format verbs use a four-bit opcode and sixteen-bit payload.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=143
struct Verb {
    enum : unsigned {
        GetParameter = 0xf00,
        GetConnectionSelect = 0xf01,
        GetConnectionList = 0xf02,
        GetPowerState = 0xf05,
        SetPowerState = 0x705,
        GetConverterStreamChannel = 0xf06,
        SetConverterStreamChannel = 0x706,
        GetPinControl = 0xf07,
        SetPinControl = 0x707,
        GetConfigurationDefault = 0xf1c,
        GetSubsystemId = 0xf20,
        SetConverterFormat = 0x200,
        GetConverterFormat = 0xa00,
        FunctionReset = 0x7ff,
    };
};
// Codec discovery parameters, section 7.3.4 (PDF pages 200–215).
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=200
struct Parameter {
    enum : unsigned {
        VendorId = 0,
        RevisionId = 2,
        SubordinateNodes = 4,
        FunctionGroupType = 5,
        AudioFunctionCapabilities = 8,
        WidgetCapabilities = 9,
        PcmCapabilities = 0x0a,
        StreamCapabilities = 0x0b,
        PinCapabilities = 0x0c,
        ConnectionListLength = 0x0e,
        SupportedPowerStates = 0x0f,
    };
};
// Root node 0 -> audio function 1 -> DAC 2, speaker 3. QEMU-compatible codec
// discovery identity 1af4:0011, revision 1.0. Speaker is fixed/internal, analog,
// association 1/sequence 0; pin enables output only. HDA 7.3.3.31/7.3.4.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=179
// https://github.com/qemu/qemu/blob/master/hw/audio/hda-codec-common.h
constexpr unsigned RootNode = 0, FunctionNode = 1, ConverterNode = 2, SpeakerNode = 3;
constexpr uint32_t CodecIdentity = 0x1af40011, CodecRevision = 0x00100100,
                   SpeakerConfiguration = 0x90170110;
// PCM bit 17 advertises 16-bit samples; bit 6 advertises 48 kHz, stream bit 0 PCM.
// Widget bits: stereo[0], power[10]; speaker adds type4[23:20], connection list[8].
// Pin bit4 permits output; power bits0/3 permit D0 (on) and D3 (off).
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=203
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=206
constexpr uint32_t Pcm16Bit48kHz = (1u << 17) | (1u << 6), PcmStream = 1, StereoPowerWidget = 0x401,
                   SpeakerWidget = 0x400501, OutputPinCapability = 0x10, D0D3PowerStates = 9;
// Pin Widget Control bit6 connects the output path (7.3.3.13).
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=163
constexpr uint8_t SpeakerOutputEnable = 0x40;
// Read a little-endian wire field; caller supplies an already validated span.
// IMPLEMENTATION NOTE: Host serialization supporting HDA section 3.6 DMA layouts.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=57
uint64_t load_le(std::span<const uint8_t> bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes.size(); ++i)
        value |= uint64_t(bytes[i]) << (8 * i);
    return value;
}
// Store a little-endian wire field in a validated guest/register span.
// IMPLEMENTATION NOTE: Host serialization supporting HDA section 3.6 DMA layouts.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=57
void store_le(std::span<uint8_t> bytes, uint64_t value) {
    for (unsigned i = 0; i < bytes.size(); ++i)
        bytes[i] = uint8_t(value >> (8 * i));
}
} // namespace
// Initialize the fixed board attachment with controller reset asserted.
// HDA 3.3.7, Global Control; host callback binding is emulator policy.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=32
IntelHda::IntelHda(std::span<uint8_t> memory, uint64_t base, std::function<void(bool)> irq,
                   PlaybackSink playback)
    : ram(memory), ram_base(base), interrupt(std::move(irq)), sink(std::move(playback)) {
    reset_controller();
}
// Read an internal field without guest access side effects (host helper).
// HDA 3.3 register layout.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=27
uint64_t IntelHda::load_register(unsigned offset, unsigned width) const {
    return load_le(std::span(registers).subspan(offset, width));
}
// Store an internal field; bus write masks are applied by write_register (host helper).
// HDA 3.3 register layout.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=27
void IntelHda::store_register(unsigned offset, uint64_t value, unsigned width) {
    store_le(std::span(registers).subspan(offset, width), value);
}
// Restore reset values and codec controls while preserving PCI BAR/Command.
// HDA 3.3.7 CRST; implementation completes codec reset synchronously.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=32
void IntelHda::reset_controller() {
    auto wake_enable = registers[Register::WakeEnable],
         state_change = registers[Register::StateChangeStatus];
    registers.fill(0);
    registers[Register::WakeEnable] = wake_enable;
    registers[Register::StateChangeStatus] = state_change;
    store_register(Register::GlobalCapabilities, PlaybackCapabilities, 2);
    registers[Register::MajorVersion] = 1;
    store_register(Register::OutputPayloadCapability, OutputPayloadWords, 2);
    store_register(Register::InputPayloadCapability, InputPayloadWords, 2);
    registers[Register::CommandOutputRingBufferSize] =
        registers[Register::ResponseInputRingBufferSize] = RingSizeCapabilities | 2;
    store_register(Register::FifoSize, 256, 2); // Maximum modeled fetch size in bytes, HDA 3.3.40.
    descriptor_index = descriptor_offset = response_count = 0;
    wall_ticks = frame_fraction = 0;
    converter_stream_channel = 0;
    converter_format = Stereo16Bit48kHz;
    pin_control = SpeakerOutputEnable;
    power_state.fill(0);
    update_interrupt_output();
}
// Read PCI discovery and size-probe fields, including interrupt-pending status.
// PCI Local Bus Specification 3.0, 6.2.1/6.2.2/6.2.5; ICH9 ID is the documented QEMU discovery
// convention. https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=218
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=225
uint64_t IntelHda::read_configuration(uint64_t offset, unsigned width) const {
    if (!width || width > 8 || offset >= ConfigurationFunctionBytes ||
        width > ConfigurationFunctionBytes - offset)
        return ~uint64_t(0);
    std::array<uint8_t, ConfigurationHeaderBytes> configuration{};
    store_le(std::span(configuration).subspan(PciRegister::VendorDeviceIdentity, 4), Ich9Identity);
    store_le(std::span(configuration).subspan(PciRegister::Command, 2), pci_command);
    configuration[PciRegister::Status] = interrupt_status() ? 8 : 0;
    store_le(std::span(configuration).subspan(PciRegister::ClassRevision, 4), AudioClassRevision);
    store_le(std::span(configuration).subspan(PciRegister::BarLower, 4),
             (bar_probe ? uint32_t(BarMask) : uint32_t(bar)) |
                 Bar64BitMemoryType); // 64-bit memory BAR.
    store_le(std::span(configuration).subspan(PciRegister::BarUpper, 4),
             bar_upper_probe ? BarProbeRequest : bar >> 32);
    store_le(std::span(configuration).subspan(PciRegister::SubsystemIdentity, 4), Ich9Identity);
    configuration[PciRegister::InterruptPin] = 1; // INTA; the board assigns its GIC interrupt.
    uint64_t result = 0;
    for (unsigned i = 0; i < width; ++i)
        if (offset + i < configuration.size())
            result |= uint64_t(configuration[offset + i]) << (8 * i);
    return result;
}
// Apply PCI Command lanes and relocatable 64-bit BAR halves; unsupported fields are RO.
// PCI Local Bus Specification 3.0, 6.2.2/6.2.5. Board aperture restrictions are emulator policy.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=218
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=225
void IntelHda::write_configuration(uint64_t offset, uint64_t value, unsigned width) {
    if (!width || width > 8 || offset >= ConfigurationFunctionBytes ||
        width > ConfigurationFunctionBytes - offset)
        return;
    if (offset == PciRegister::BarLower && width == 4) {
        bar_probe = uint32_t(value) == BarProbeRequest;
        if (!bar_probe)
            bar = (bar & 0xffffffff00000000ull) | (value & uint32_t(BarMask));
    } else if (offset == PciRegister::BarUpper && width == 4) {
        bar_upper_probe = uint32_t(value) == BarProbeRequest;
        if (!bar_upper_probe)
            bar = (bar & 0xffffffff) | (uint64_t(uint32_t(value)) << 32);
    } else {
        for (unsigned i = 0; i < width; ++i) {
            if (offset + i == PciRegister::Command)
                pci_command = (pci_command & 0xff00) |
                              ((value >> (8 * i)) & (MemoryEnable | BusMasterEnable));
            if (offset + i == PciRegister::Command + 1)
                pci_command = (pci_command & 0xff) |
                              ((uint16_t(uint8_t(value >> (8 * i))) << 8) & InterruptDisable);
        }
    }
    process_command_ring();
    update_interrupt_output();
}
// Decode only enabled BAR accesses entirely inside the board PCI aperture.
// PCI Local Bus Specification 3.0, 6.2.2/6.2.5; aperture limits are implementation policy.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=218
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=225
bool IntelHda::contains(uint64_t address, unsigned width) const {
    return width && width <= 8 && (pci_command & MemoryEnable) && bar >= BoardMemoryBegin &&
           bar <= BoardMemoryEnd - BarBytes && address >= bar && address - bar < BarBytes &&
           width <= BarBytes - (address - bar);
}
// Validate a DMA range using subtraction, avoiding address/length overflow.
// IMPLEMENTATION NOTE: RAM-only policy supporting HDA 3.6 DMA structures.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=57
std::span<uint8_t> IntelHda::dma_memory(uint64_t address, size_t bytes) {
    if (address < ram_base || address - ram_base > ram.size() ||
        bytes > ram.size() - (address - ram_base))
        return {};
    return ram.subspan(size_t(address - ram_base), bytes);
}
// Read register lanes, deriving INTSTS and the 24 MHz wall clock dynamically.
// HDA 3.3.15/16/37/44/45. Reserved registers read zero in this subset.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=37
uint64_t IntelHda::read_register(uint64_t address, unsigned width) const {
    if (!contains(address, width))
        return ~uint64_t(0);
    uint64_t offset = address - bar, value = 0;
    for (unsigned i = 0; i < width; ++i) {
        auto lane = offset + i;
        if (lane >= Register::WallClockAlias && lane < Register::WallClockAlias + 4)
            lane = Register::WallClockCounter + lane - Register::WallClockAlias;
        if (lane >= Register::LinkPositionAlias && lane < Register::LinkPositionAlias + 4)
            lane = Register::LinkPosition + lane - Register::LinkPositionAlias;
        uint8_t byte = lane < registers.size() ? registers[lane] : 0;
        if (lane >= Register::InterruptStatus && lane < Register::InterruptStatus + 4)
            byte = uint8_t(interrupt_status() >> (8 * (lane - Register::InterruptStatus)));
        if (lane >= Register::WallClockCounter && lane < Register::WallClockCounter + 4)
            byte = uint8_t(wall_ticks >> (8 * (lane - Register::WallClockCounter)));
        value |= uint64_t(byte) << (8 * i);
    }
    return value;
}
// Apply register lane masks, write-one-to-clear status, ring/stream reset and verbs.
// HDA 3.3.7/9/20–43 and 3.4.3 immediate commands.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=32
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=54
void IntelHda::write_register(uint64_t address, uint64_t value, unsigned width) {
    if (!contains(address, width))
        return;
    auto offset = address - bar;
    // 3.3 register lane widths are independent of the CPU's access width.
    // https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=28
    const auto old_control = registers[Register::OutputStreamControl];
    bool immediate = false;
    for (unsigned i = 0; i < width; ++i) {
        uint64_t lane = offset + i;
        uint8_t byte = uint8_t(value >> (8 * i));
        if (lane >= registers.size())
            continue;
        if (lane == Register::GlobalControl) {
            if (!(byte & ControllerResetExit)) {
                reset_controller();
                return;
            }
            if (!(registers[Register::GlobalControl] & ControllerResetExit))
                registers[Register::StateChangeStatus] = 1;
            registers[Register::GlobalControl] =
                byte & 1; // CRST; unsolicited responses are not implemented.
            continue;
        }
        if (!(registers[Register::GlobalControl] & ControllerResetExit))
            continue;
        if (lane == Register::StateChangeStatus ||
            lane == Register::CommandOutputRingBufferStatus ||
            lane == Register::ResponseInputRingBufferStatus ||
            lane == Register::OutputStreamStatus) {
            registers[lane] &= ~byte;
            if (lane == Register::OutputStreamStatus &&
                (registers[Register::OutputStreamControl] & StreamRun))
                registers[lane] |= FifoReady;
            // 3.3.21 CORBRP reset is acknowledged until software clears it.
            // https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=39
        } else if (lane == Register::CommandOutputRingBufferReadPointer + 1) {
            store_register(
                Register::CommandOutputRingBufferReadPointer,
                byte & 0x80 ? RingPointerReset
                            : load_register(Register::CommandOutputRingBufferReadPointer, 2) & 255,
                2);
        } else if (lane == Register::ResponseInputRingBufferWritePointer + 1) {
            if (byte & 0x80) {
                store_register(Register::ResponseInputRingBufferWritePointer, 0, 2);
                response_count = 0;
            }
        } else if (lane == Register::CommandOutputRingBufferSize ||
                   lane == Register::ResponseInputRingBufferSize) {
            if ((byte & 3) < 3)
                registers[lane] = RingSizeCapabilities | (byte & 3);
        } else if (lane == Register::ImmediateStatus) {
            registers[lane] &= ~(byte & 2);
            immediate = byte & 1;
        } else {
            uint8_t mask = 0;
            if (lane == Register::WakeEnable)
                mask = 1;
            if (lane >= Register::InterruptControl && lane < Register::InterruptControl + 4)
                mask = lane == Register::InterruptControl       ? 1
                       : lane == Register::InterruptControl + 3 ? 0xc0
                                                                : 0;
            if (lane == Register::StreamSynchronization)
                mask = 1;
            if ((lane >= Register::CommandOutputRingBufferLowerBase &&
                 lane < Register::CommandOutputRingBufferLowerBase + 8) ||
                (lane >= Register::ResponseInputRingBufferLowerBase &&
                 lane < Register::ResponseInputRingBufferLowerBase + 8) ||
                (lane >= Register::PositionLowerBase && lane < Register::PositionLowerBase + 8) ||
                (lane >= Register::BufferDescriptorListLowerBase &&
                 lane < Register::BufferDescriptorListLowerBase + 8)) {
                mask = 255;
                if (lane == Register::CommandOutputRingBufferLowerBase ||
                    lane == Register::ResponseInputRingBufferLowerBase ||
                    lane == Register::BufferDescriptorListLowerBase)
                    mask = 0x80;
                if (lane == Register::PositionLowerBase)
                    mask = 0x81;
            }
            if (lane == Register::CommandOutputRingBufferWritePointer ||
                lane == Register::ResponseInterruptCount)
                mask = 255;
            if (lane == Register::CommandOutputRingBufferControl)
                mask = 3;
            if (lane == Register::ResponseInputRingBufferControl)
                mask = 7;
            if (lane >= Register::ImmediateCommand && lane < Register::ImmediateCommand + 4)
                mask = 255;
            if (lane == Register::OutputStreamControl)
                mask = 0x1f;
            if (lane == Register::OutputStreamControl + 2)
                mask = 0xf0; // stream tag; fixed output direction.
            if (lane >= Register::CyclicBufferLength && lane < Register::CyclicBufferLength + 4)
                mask = 255;
            if (lane == Register::LastValidIndex || lane == Register::StreamFormat ||
                lane == Register::StreamFormat + 1)
                mask = 255;
            registers[lane] = (registers[lane] & ~mask) | (byte & mask);
        }
    }
    // 3.3.35 stream reset discards descriptor progress and writable stream setup.
    // https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=46
    if (registers[Register::OutputStreamControl] & StreamReset) {
        std::fill(registers.begin() + Register::OutputStreamControl, registers.begin() + 0xa0, 0);
        registers[Register::OutputStreamControl] = StreamReset;
        store_register(Register::FifoSize, 256, 2);
        descriptor_index = descriptor_offset = 0;
        frame_fraction = 0;
    } else if (!(old_control & StreamRun) &&
               (registers[Register::OutputStreamControl] & StreamRun)) {
        registers[Register::OutputStreamStatus] |= FifoReady;
        frame_fraction = 0;
    } else if (!(registers[Register::OutputStreamControl] & StreamRun))
        registers[Register::OutputStreamStatus] &= ~FifoReady;
    // 3.4.3 immediate commands bypass DMA and publish IRV synchronously here.
    // https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=54
    if (immediate && !(registers[Register::CommandOutputRingBufferControl] & RingDmaEnable)) {
        uint32_t command = uint32_t(load_register(Register::ImmediateCommand, 4));
        if ((command >> 28) == 0) {
            store_register(Register::ImmediateResponse, execute_codec_verb(command), 4);
            registers[Register::ImmediateStatus] = 2;
        }
    }
    process_command_ring();
    update_interrupt_output();
}
// Execute discovery and supported playback controls for the fixed codec graph.
// HDA 7.3.3 verbs and 7.3.4 parameter responses, with encoded topology explained above.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=143
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=200
uint32_t IntelHda::execute_codec_verb(uint32_t command) {
    ++codec_commands;
    unsigned node = (command >> 20) & 255, verb = (command >> 8) & 0xfff, payload = command & 255;
    if (node > SpeakerNode)
        return 0;
    // 7.3.3.8 Converter Format: 4-bit verb with a 16-bit format payload.
    // https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=149
    if ((verb & 0xf00) == Verb::SetConverterFormat || (verb & 0xf00) == Verb::GetConverterFormat) {
        if (node != ConverterNode)
            return 0;
        if ((verb & 0xf00) == Verb::SetConverterFormat)
            converter_format = uint16_t(command);
        return (verb & 0xf00) == Verb::GetConverterFormat ? converter_format : 0;
    }
    if (verb == Verb::GetParameter) {
        switch (payload) {
        case Parameter::VendorId:
            return node == RootNode ? CodecIdentity : 0;
        case Parameter::RevisionId:
            return node == RootNode ? CodecRevision : 0;
        case Parameter::SubordinateNodes:
            return node == RootNode       ? (FunctionNode << 16) | 1
                   : node == FunctionNode ? (ConverterNode << 16) | 2
                                          : 0;
        case Parameter::FunctionGroupType:
            return node == FunctionNode ? 1 : 0; // Audio function, no unsolicited responses.
        case Parameter::AudioFunctionCapabilities:
            return 0; // No additional delay/beep generator.
        // 7.3.4.6 widget topology; 7.3.4.7/8 formats; 7.3.4.9 pin direction.
        // https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=203
        // https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=206
        case Parameter::WidgetCapabilities:
            return node == ConverterNode ? StereoPowerWidget
                   : node == SpeakerNode ? SpeakerWidget
                                         : 0;
        case Parameter::PcmCapabilities:
            return node == FunctionNode || node == ConverterNode ? Pcm16Bit48kHz : 0;
        case Parameter::StreamCapabilities:
            return node == FunctionNode || node == ConverterNode ? PcmStream : 0;
        case Parameter::PinCapabilities:
            return node == SpeakerNode ? OutputPinCapability : 0;
        case Parameter::ConnectionListLength:
            return node == SpeakerNode ? 1 : 0;
        case Parameter::SupportedPowerStates:
            return node ? D0D3PowerStates : 0;
        default:
            return 0; // Unadvertised parameters are absent.
        }
    }
    switch (verb) {
    case Verb::GetConnectionList:
        return node == SpeakerNode && !payload ? ConverterNode : 0;
    case Verb::GetConnectionSelect:
        return 0; // The sole connection is index zero.
    // 7.3.3.31 fixed-speaker configuration and 7.3.3.30 subsystem identity.
    // https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=178
    case Verb::GetConfigurationDefault:
        return node == SpeakerNode ? SpeakerConfiguration : 0;
    case Verb::GetSubsystemId:
        return node == FunctionNode ? CodecIdentity : 0;
    // 7.3.3.10: requested/actual power states; host policy transitions immediately.
    // https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=153
    case Verb::GetPowerState:
        return power_state[node] |
               (power_state[node] << 4); // Requested and actual state, no delay.
    case Verb::SetPowerState:
        if (node && (payload == 0 || payload == 3))
            power_state[node] = payload;
        break;
    // 7.3.3.11 stream tag/channel selects which output stream reaches the DAC.
    // https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=161
    case Verb::GetConverterStreamChannel:
        return node == ConverterNode ? converter_stream_channel : 0;
    case Verb::SetConverterStreamChannel:
        if (node == ConverterNode)
            converter_stream_channel = payload;
        break;
    // 7.3.3.13 Pin Widget Control enables the speaker path.
    // https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=163
    case Verb::GetPinControl:
        return node == SpeakerNode ? pin_control : 0;
    case Verb::SetPinControl:
        if (node == SpeakerNode)
            pin_control = payload & SpeakerOutputEnable;
        break;
    // 7.3.3.33 Function Reset restores this codec's playback controls.
    // https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=184
    case Verb::FunctionReset:
        if (node == FunctionNode) {
            power_state.fill(0);
            converter_stream_channel = 0;
            converter_format = Stereo16Bit48kHz;
            pin_control = SpeakerOutputEnable;
        }
        break;
    default:
        break; // Unsupported verbs return zero without changing controls.
    }
    return 0;
}
// Fetch CORB commands only with bus mastering and both ring DMA engines enabled.
// Publish each eight-byte solicited reply before advancing RIRBWP. Invalid DMA
// latches CORB memory-error/RIRB overrun status instead of touching host memory.
// HDA 3.3.20–31, 3.6.4/5 CORB/RIRB layouts and 4.4 operation.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=38
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=57
void IntelHda::process_command_ring() {
    if (!(pci_command & BusMasterEnable) ||
        !(registers[Register::GlobalControl] & ControllerResetExit) ||
        !(registers[Register::CommandOutputRingBufferControl] & RingDmaEnable) ||
        !(registers[Register::ResponseInputRingBufferControl] & RingDmaEnable) ||
        (load_register(Register::CommandOutputRingBufferReadPointer, 2) & RingPointerReset))
        return;
    unsigned entries = RingEntries[registers[Register::CommandOutputRingBufferSize] & 3],
             replies = RingEntries[registers[Register::ResponseInputRingBufferSize] & 3];
    unsigned rp = registers[Register::CommandOutputRingBufferReadPointer],
             wp = registers[Register::CommandOutputRingBufferWritePointer];
    if (wp >= entries || rp >= entries) {
        registers[Register::CommandOutputRingBufferStatus] |= 1;
        return;
    }
    const auto commands = load_register(Register::CommandOutputRingBufferLowerBase, 8),
               responses = load_register(Register::ResponseInputRingBufferLowerBase, 8);
    for (unsigned count = 0; rp != wp && count < entries; ++count) {
        unsigned next = (rp + 1) % entries;
        auto command_memory = dma_memory(commands, entries * 4);
        if (command_memory.empty()) {
            registers[Register::CommandOutputRingBufferStatus] |= 1;
            break;
        }
        auto command = uint32_t(load_le(command_memory.subspan(next * 4, 4)));
        if ((command >> 28) == 0) {
            auto response_memory = dma_memory(responses, replies * 8);
            if (response_memory.empty()) {
                registers[Register::ResponseInputRingBufferStatus] |= 4;
                break;
            }
            unsigned response_pointer =
                (registers[Register::ResponseInputRingBufferWritePointer] + 1) % replies;
            store_le(response_memory.subspan(response_pointer * 8, 8), execute_codec_verb(command));
            store_register(Register::ResponseInputRingBufferWritePointer, response_pointer, 2);
            unsigned threshold = registers[Register::ResponseInterruptCount]
                                     ? registers[Register::ResponseInterruptCount]
                                     : 256;
            if (++response_count >= threshold) {
                registers[Register::ResponseInputRingBufferStatus] |= 1;
                response_count = 0;
            }
        }
        rp = next;
        store_register(Register::CommandOutputRingBufferReadPointer, rp, 2);
    }
}
// Advance playback from guest BDLs, bounded by advertised format and elapsed frames.
// HDA 3.3.33–43 and 3.6.2: IOC latches BCIS, CBL wraps LPIB, position DMA
// writes the stream's eight-byte slot. Bad descriptor geometry latches DESE.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=45
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=57
void IntelHda::advance_output_stream(unsigned frames) {
    if (!frames || !(pci_command & BusMasterEnable) ||
        !(registers[Register::OutputStreamControl] & StreamRun) ||
        (registers[Register::OutputStreamControl] & StreamReset) ||
        (registers[Register::StreamSynchronization] & 1))
        return;
    auto fail = [this] {
        registers[Register::OutputStreamStatus] |= DescriptorError;
        registers[Register::OutputStreamControl] &= ~StreamRun;
        registers[Register::OutputStreamStatus] &= ~FifoReady;
    };
    auto format = load_register(Register::StreamFormat, 2);
    unsigned cyclic_bytes = unsigned(load_register(Register::CyclicBufferLength, 4)),
             last = registers[Register::LastValidIndex];
    unsigned position = unsigned(load_register(Register::LinkPosition, 4));
    const auto bdl_base = load_register(Register::BufferDescriptorListLowerBase, 8);
    if (format != Stereo16Bit48kHz || !last || !cyclic_bytes || cyclic_bytes % FrameBytes ||
        position >= cyclic_bytes) {
        fail();
        return;
    }
    auto descriptors = dma_memory(bdl_base, (last + 1) * DescriptorBytes);
    if (descriptors.empty() || descriptor_index > last) {
        fail();
        return;
    }
    std::vector<int16_t> samples;
    samples.reserve(frames * 2);
    unsigned remaining = frames * FrameBytes;
    while (remaining) {
        auto descriptor = descriptors.subspan(descriptor_index * DescriptorBytes, DescriptorBytes);
        auto buffer = load_le(descriptor.first(8));
        unsigned length = unsigned(load_le(descriptor.subspan(8, 4)));
        if (!length || length % FrameBytes || buffer % 128 || descriptor_offset >= length) {
            fail();
            break;
        }
        unsigned bytes = std::min({remaining, length - descriptor_offset, cyclic_bytes - position});
        // Validate the whole descriptor span before calculating its data subrange.
        auto data = dma_memory(buffer, length);
        if (data.empty()) {
            fail();
            break;
        }
        data = data.subspan(descriptor_offset, bytes);
        for (unsigned i = 0; i < bytes; i += 2)
            samples.push_back(int16_t(uint16_t(data[i]) | (uint16_t(data[i + 1]) << 8)));
        descriptor_offset += bytes;
        remaining -= bytes;
        position = (position + bytes) % cyclic_bytes;
        if (descriptor_offset == length) {
            if (load_le(descriptor.subspan(12, 4)) & 1)
                registers[Register::OutputStreamStatus] |= BufferCompletion;
            descriptor_offset = 0;
            descriptor_index = descriptor_index == last ? 0 : descriptor_index + 1;
        }
        if (!position && (descriptor_index || descriptor_offset)) {
            fail();
            break;
        } // BDL must agree with CBL.
    }
    playback_frames += samples.size() / 2;
    store_register(Register::LinkPosition, position, 4);
    auto position_base = load_register(Register::PositionLowerBase, 8);
    if (position_base & 1) {
        auto destination = dma_memory(position_base & ~uint64_t(127), 8);
        if (destination.empty())
            fail();
        else
            store_le(destination, position);
    }
    const unsigned stream_tag = registers[Register::OutputStreamControl + 2] >> 4;
    if (!samples.empty() && stream_tag && converter_stream_channel == (stream_tag << 4) &&
        converter_format == Stereo16Bit48kHz && (pin_control & SpeakerOutputEnable) &&
        !power_state[FunctionNode] && !power_state[ConverterNode] && !power_state[SpeakerNode]) {
        output_frames += samples.size() / 2;
        nonzero_samples += std::count_if(samples.begin(), samples.end(),
                                         [](int16_t sample) { return sample != 0; });
        if (sink)
            sink(samples);
    }
}
// Aggregate enabled stream/controller causes; GIS summarizes any pending cause.
// HDA 3.3.14/15 and 3.5. PCI INTx-disable gates the wire separately.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=37
uint32_t IntelHda::interrupt_status() const {
    uint32_t result = 0;
    auto stream_causes = registers[Register::OutputStreamStatus] &
                         registers[Register::OutputStreamControl] &
                         (BufferCompletion | FifoError | DescriptorError);
    if (stream_causes)
        result |= OutputStreamInterrupt;
    if (((registers[Register::ResponseInputRingBufferStatus] & 1) &&
         (registers[Register::ResponseInputRingBufferControl] & ResponseInterruptEnable)) ||
        ((registers[Register::ResponseInputRingBufferStatus] & 4) &&
         (registers[Register::ResponseInputRingBufferControl] & 4)) ||
        ((registers[Register::CommandOutputRingBufferStatus] & 1) &&
         (registers[Register::CommandOutputRingBufferControl] & 1)) ||
        (registers[Register::StateChangeStatus] & registers[Register::WakeEnable]))
        result |= ControllerInterrupt;
    return result ? result | GlobalInterrupt : 0;
}
// Drive active-high INTx only when GIE, source enable and PCI masking permit it.
// HDA 3.5 interrupt structure; PCI 3.0 section 6.2.2 Command masking.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=55
void IntelHda::update_interrupt_output() {
    uint32_t enabled = uint32_t(load_register(Register::InterruptControl, 4));
    bool level = (enabled & GlobalInterrupt) && (interrupt_status() & enabled & ~GlobalInterrupt) &&
                 !(pci_command & InterruptDisable);
    if (interrupt)
        interrupt(level);
}
// Advance wall time at 24 MHz and playback at 48 kHz, retaining fractional frames.
// HDA 3.3.16/3.6.2; bounded host elapsed-time scheduling is implementation policy.
// Pending sub-threshold responses signal on service, representing an empty
// response slot after the synchronous batch (3.3.30, RINTFL).
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=37
void IntelHda::service_elapsed_microseconds(uint64_t microseconds) {
    if (!(registers[Register::GlobalControl] & ControllerResetExit))
        return;
    wall_ticks += microseconds * WallTicksPerMicrosecond;
    process_command_ring();
    if (response_count && microseconds) {
        registers[Register::ResponseInputRingBufferStatus] |= 1;
        response_count = 0;
    }
    if (registers[Register::OutputStreamControl] & StreamRun) {
        frame_fraction += std::min(microseconds, MaximumServiceMicroseconds) * SampleRateHz;
        unsigned frames = unsigned(frame_fraction / MicrosecondsPerSecond);
        frame_fraction %= MicrosecondsPerSecond;
        advance_output_stream(frames);
    }
    update_interrupt_output();
}
// Expose host diagnostic counters and guest control state without changing it.
// IMPLEMENTATION NOTE: Host observability for HDA 3.3/7.3; not a hardware register.
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf#page=27
std::string IntelHda::diagnostics() const {
    std::ostringstream output;
    output << "hdaCommands=" << codec_commands << " hdaFrames=" << playback_frames
           << " hdaOutputFrames=" << output_frames << " hdaNonzeroSamples=" << nonzero_samples
           << " hdaBar=0x" << std::hex << bar << " hdaPciCommand=0x" << pci_command
           << " hdaControl=0x" << load_register(Register::GlobalControl, 4)
           << " hdaStreamControl=0x" << load_register(Register::OutputStreamControl, 3)
           << " hdaStreamStatus=0x" << unsigned(registers[Register::OutputStreamStatus])
           << " hdaFormat=0x" << load_register(Register::StreamFormat, 2);
    return output.str();
}
} // namespace pc
