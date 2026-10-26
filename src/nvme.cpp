// NVMe (Non-Volatile Memory Express) exposes one disk namespace through queues
// in guest RAM. The guest maps BAR0, allocates Admin Submission/Completion Queues,
// selects entry/page sizes and enables the controller. Admin commands identify
// the disk and create I/O queues. Submission tail doorbells submit 64-byte commands;
// completion head doorbells release 16-byte completions. A phase bit distinguishes
// new completion entries across wraparound. PRP (Physical Region Page) pointers
// describe guest data buffers; the model validates their entire scatter list first.
//
// IMPLEMENTATION NOTE: All commands run synchronously with guest CPUs stopped.
// There are sixteen queue slots including Admin, at most 256 entries each, 4 KiB
// pages and 512-byte logical blocks. Legacy INTx vector zero is the only interrupt.
// Successful writes flush because no volatile cache is advertised. There is no
// SGL, metadata, protection information, discard, multipath or background I/O.
// AERs are retained without generating events until reset. Coalescing and async
// event configuration are stored/read back but do not schedule actual events.
// Abort reports not-found. Identify/log data are intentionally sparse model data.
// COMPATIBILITY: CNS=2, Identify version at byte 80 and generic status 12h are
// retained beyond/reserved in the 1.1a reference. The BAR is a bounded 32-bit board
// mapping. FIXME: Complete required PCI/Identify/log/error behavior before claiming
// full NVMe 1.1 conformance; preserve existing guest-visible values in this migration.
// Reference: NVM Express 1.1a (23 September 2013), sections 2-6.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=36
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=44
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=83

#include "nvme.hpp"
#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace pc {
namespace {
// NVMe 1.1a controller register offsets, 3.1; all multi-byte fields little-endian.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=36
enum Register : unsigned {
    ControllerCapabilitiesRegister = 0x00,  // CAP: queue/page/command-set capability fields.
    VersionRegister = 0x08,                 // VS: major/minor/tertiary version bytes.
    InterruptMaskSetRegister = 0x0c,        // INTMS: write one to mask a vector.
    InterruptMaskClearRegister = 0x10,      // INTMC: write one to unmask a vector.
    ControllerConfigurationRegister = 0x14, // CC: enable, shutdown and entry/page format.
    ControllerStatusRegister = 0x1c,        // CSTS: ready, fatal and shutdown state.
    AdminQueueAttributesRegister = 0x24,    // AQA: zero-based SQ/CQ sizes.
    AdminSubmissionQueueRegister = 0x28,    // ASQ: 64-bit page-aligned Admin SQ base.
    AdminCompletionQueueRegister = 0x30,    // ACQ: 64-bit page-aligned Admin CQ base.
    DoorbellRegisters = 0x1000,             // SQ tail / CQ head pairs with CAP.DSTRD=0.
};
// PCI type-0 configuration fields; NVMe 1.1a section 2.1.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=19
enum PciRegister : unsigned {
    VendorDeviceId = 0,
    Command = 4,
    StatusRegister = 6,
    ClassRevision = 8,
    CacheLineSize = 0x0c,
    LatencyTimer = 0x0d,
    BaseAddress = 0x10,
    InterruptLine = 0x3c,
    InterruptPin = 0x3d,
};
// COMPATIBILITY: Retained Red Hat vendor 1b36h/device 0010h and NVMe class
// 01/08/02, revision 1. Identity does not imply reuse of another device model.
constexpr uint16_t PciVendorId = 0x1b36, PciDeviceId = 0x0010;
constexpr uint32_t PciClassRevision = 0x01080201;
// IMPLEMENTATION NOTE: A 16 KiB, 32-bit BAR in the board PCI memory aperture.
constexpr uint32_t BarBytes = 0x4000, BarAddressMask = 0xffffc000, BoardMemoryBegin = 0x10000000,
                   BoardMemoryEnd = 0x3f000000;
constexpr unsigned FunctionConfigurationBytes = 4096, ConventionalConfigurationBytes = 256;
// PCI Command memory/bus-master/INTx-disable and PCI Status interrupt-pending fields.
constexpr uint16_t PciMemoryEnable = 2, PciBusMasterEnable = 4, PciInterruptDisable = 0x400,
                   PciInterruptPending = 8;
// Byte writes preserve the opposite half of the PCI Command word.
constexpr uint16_t LowByteMask = 0xff, HighByteMask = 0xff00;
constexpr uint8_t CommandLowWritable = 7, CommandHighWritable = 4;
// Fixed geometry advertised by CAP/CC/Identify: 4 KiB pages, 64-byte commands,
// 16-byte completions, 512-byte namespace blocks. Queue counts are zero-based on wire.
constexpr unsigned PageBytes = 4096, PageOffsetMask = PageBytes - 1, SubmissionEntryBytes = 64,
                   CompletionEntryBytes = 16, LogicalBlockBytes = 512;
constexpr unsigned SubmissionEntryLog2 = 6, CompletionEntryLog2 = 4, LogicalBlockLog2 = 9;
constexpr unsigned DoorbellPairBytes = 8, CompletionDoorbellOffset = 4, BitsPerByte = 8,
                   HighWordShift = 16;
// CAP.CQR[16]=1 contiguous queues; TO[31:24]=1 (500 ms); CSS[37]=1 NVM;
// MPSMIN/MPSMAX=0 (4 KiB), DSTRD=0 (4-byte doorbells). MQES=255 (256 entries).
constexpr uint64_t ContiguousQueuesRequired = 1ull << 16, Timeout500Milliseconds = 1ull << 24,
                   NvmCommandSetSupported = 1ull << 37;
constexpr uint32_t ControllerVersion11 = 0x00010100;
// CC.EN[0], CSS/MPS/AMS[13:4], SHN[15:14], IOSQES/IOCQES[23:16].
constexpr uint32_t ControllerEnable = 1, InitialModeMask = 0x3fff, EntrySizeFieldMask = 0xff,
                   EntrySizeFieldShift = 16,
                   ExpectedEntrySizes = (CompletionEntryLog2 << 4) | SubmissionEntryLog2,
                   ShutdownNotificationMask = 0xc000;
// CSTS.RDY[0], CFS[1], SHST[3:2] (10b means shutdown processing complete).
constexpr uint32_t ControllerReady = 1, ControllerFatal = 2, ShutdownStatusMask = 12,
                   ShutdownComplete = 8;
// AQA.ASQS[11:0], ACQS[27:16], reserved high nibble of each 16-bit half.
constexpr uint32_t AdminQueueSizeMask = 0xfff, AdminQueueReservedMask = 0xf000f000;
// Single legacy interrupt vector: INTMS/INTMC bit 0 gates INTx.
constexpr uint32_t InterruptVectorZero = 1;
// Generic command layout, 4.2: opcode:u8, flags:u8, CID:u16, NSID:u32,
// metadata pointer:u64, PRP1:u64, PRP2:u64 and command-specific DWORDs 10..15.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=46
enum CommandField : unsigned {
    Opcode = 0,
    Flags = 1,
    Identifier = 2,
    NamespaceId = 4,
    MetadataPointer = 16,
    Prp1 = 24,
    Prp2 = 32,
    Dword10 = 40,
    Dword11 = 44,
    Dword12 = 48,
};
constexpr uint8_t PointerFormatMask = 0xc0, FusedCommandMask = 3;
// Physical Region Pages (PRPs): first buffer DWORD-aligned, subsequent pages
// page-aligned; list entries are u64 page addresses (4.3).
constexpr unsigned FirstPrpAlignmentMask = 3, PrpEntryBytes = 8, PrpListAlignmentMask = 7;
// Completion entry, 4.6: result:u32,reserved:u32,SQ head:u16,SQ ID:u16,CID:u16,SF:u16.
// SF's low bit is phase; status code/type is shifted left one when published.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=55
enum CompletionField : unsigned {
    Result = 0,
    Reserved = 4,
    SubmissionHead = 8,
    SubmissionId = 10,
    CommandIdentifier = 12,
    StatusField = 14,
};
// Admin opcodes (clause 5) are distinct from NVM I/O opcodes (clause 6).
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=64
enum AdminCommand : unsigned {
    DeleteSubmissionQueue = 0,
    CreateSubmissionQueue = 1,
    GetLogPage = 2,
    DeleteCompletionQueue = 4,
    CreateCompletionQueue = 5,
    Identify = 6,
    Abort = 8,
    SetFeatures = 9,
    GetFeatures = 0xa,
    AsynchronousEventRequest = 0xc,
};
enum IoCommand : unsigned { Flush = 0, Write = 1, Read = 2 };
// Unshifted completion status/type values (4.6.1 Figures 30..32); 01xxh means
// command-specific status type 1. DNR/MORE remain clear in this model.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=57
enum Status : uint16_t {
    Success = 0,
    InvalidOpcode = 1,
    InvalidField = 2,
    DataTransferError = 4,
    InternalDeviceError = 6,
    InvalidNamespace = 0xb,
    LbaOutOfRange = 0x80,
    AsyncRequestLimitExceeded = 0x105,
    InvalidQueueDeletion = 0x10c,
    // COMPATIBILITY: 12h is reserved in 1.1a; retained when changing queue-count
    // features with live queues. FIXME: Use the specified command-sequence error.
    QueueCountChangeCompatibilityError = 0x12,
};
// Feature identifiers (5.12): unsupported save/select bits must remain zero.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=97
enum Feature : unsigned {
    Arbitration = 1,
    PowerManagement = 2,
    VolatileWriteCache = 6,
    NumberOfQueues = 7,
    InterruptCoalescing = 8,
    InterruptVectorConfiguration = 9,
    AsynchronousEventConfiguration = 0xb,
};
// Identify/log payloads and their populated byte offsets (5.10/5.11).
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=83
constexpr unsigned IdentifyBytes = 4096, MaximumAsyncRequests = 4, MaximumTransferExponent = 5,
                   MaximumTransferBlocks = 256;
enum IdentifyControllerField : unsigned {
    VendorId = 0,
    SubsystemVendorId = 2,
    SerialNumber = 4,
    ModelNumber = 24,
    FirmwareRevision = 64,
    MaximumDataTransferSize = 77,
    ControllerId = 78,
    // COMPATIBILITY: Version at 80 and CNS=2 namespace list are later-style fields;
    // NVMe 1.1a reserves them. Retained response is not a full 1.1a conformance claim.
    VersionCompatibility = 80,
    AsyncRequestLimit = 259,
    SubmissionEntrySize = 512,
    CompletionEntrySize = 513,
    NamespaceCount = 516,
    OptionalNvmCommands = 520,
};
enum IdentifyNamespaceField : unsigned {
    Size = 0,
    Capacity = 8,
    Utilization = 16,
    LbaFormat0DataSize = 130, // LBAF0 exponent at descriptor byte 2: 2^9=512 bytes.
};
enum IdentifySelector : unsigned { Namespace = 0, Controller = 1, ActiveNamespaceList = 2 };
enum LogPage : unsigned {
    ErrorInformation = 1,
    HealthInformation = 2,
    FirmwareSlotInformation = 3
};
// Command-specific DWORD fields: selectors occupy the low byte; queue IDs/counts
// and feature values use 16-bit halves. Get Log Page NUMD is CDW10[27:16], zero-based.
constexpr uint32_t CommandSelectorMask = 0xff, CommandLowWordMask = 0xffff,
                   LogDwordCountMask = 0xfff;
constexpr unsigned CommandDwordBytes = 4, ControllerRegisterImageBytes = 64;
// Identify fixed-width ASCII field lengths (5.11, Figure 82).
constexpr unsigned SerialNumberBytes = 20, FirmwareRevisionBytes = 8;
// Health log temperature is u16 Kelvin at byte 1; firmware slot log has AFI at
// byte 0 and eight-byte slot-1 revision at byte 8 (5.10).
constexpr unsigned HealthTemperatureOffset = 1, FirmwareActiveSlotOffset = 0,
                   FirmwareSlot1RevisionOffset = 8;
constexpr uint8_t ActiveFirmwareSlot1 = 1;
constexpr uint32_t NamespaceOne = 1, AllNamespaces = UINT32_MAX;
constexpr uint32_t ProtectionInformationMask = 0x3c000000; // CDW12 PRINFO[29:26], unsupported.
constexpr uint32_t QueuePhysicallyContiguous = 1, QueueInterruptEnable = 2;
constexpr uint16_t SyntheticTemperatureKelvin = 300; // IMPLEMENTATION NOTE: Fixed health sample.
constexpr uint32_t AbortCommandNotFound = 1; // Abort CQE result bit 0: command was not aborted.
// Decode an already validated little-endian wire field.
// IMPLEMENTATION NOTE: Host wire-access helper for NVMe registers/data structures.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=36
uint64_t read_little_endian(std::span<const uint8_t> b, size_t o, unsigned n) {
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= uint64_t(b[o + i]) << (BitsPerByte * i);
    return v;
}
// Encode a little-endian register/command payload field.
// IMPLEMENTATION NOTE: Host wire-access helper for NVMe registers/data structures.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=36
void write_little_endian(std::span<uint8_t> b, size_t o, uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i)
        b[o + i] = uint8_t(v >> (BitsPerByte * i));
}
// Return all ones at the requested read width for unimplemented PCI bytes.
// IMPLEMENTATION NOTE: Host wire-access helper for NVMe registers/data structures.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=36
uint64_t access_value_mask(unsigned n) {
    return n == 8 ? UINT64_MAX : (1ull << (BitsPerByte * n)) - 1;
}
// Reject malformed register widths, alignment and region overrun before access.
// IMPLEMENTATION NOTE: Host wire-access helper for NVMe registers/data structures.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=36
void validate_access(uint64_t o, unsigned n, uint64_t size) {
    if ((n != 1 && n != 2 && n != 4 && n != 8) || o % n || o > size || n > size - o)
        throw std::runtime_error("Invalid NVMe register access");
}
} // namespace
// Bind disk/RAM/interrupt callback and construct the retained PCI discovery identity.
// NVMe 1.1a; board bounds, synchronous execution and compatibility are host policy.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=19
Nvme::Nvme(Disk &d, std::span<uint8_t> r, uint64_t b, std::function<void(bool)> irq)
    : disk(d), ram(r), ramBase(b), interrupt(std::move(irq)) {
    write_little_endian(config, PciRegister::VendorDeviceId,
                        (uint32_t(PciDeviceId) << HighWordShift) | PciVendorId,
                        4); // Red Hat PCI vendor; original PCEmu device model.
    write_little_endian(config, PciRegister::ClassRevision, PciClassRevision,
                        4); // Mass storage / NVM / NVMe, revision 1.
    // Unassigned platform interrupt line; pin 1 selects INTA#.
    config[PciRegister::InterruptLine] = UINT8_MAX;
    config[PciRegister::InterruptPin] = 1;
}
// Read PCI header/BAR size-probe and aggregate completion-interrupt status.
// NVMe 1.1a; board bounds, synchronous execution and compatibility are host policy.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=19
uint64_t Nvme::configRead(uint64_t o, unsigned n) const {
    validate_access(o & (FunctionConfigurationBytes - 1), n, FunctionConfigurationBytes);
    if (o >= ConventionalConfigurationBytes)
        return access_value_mask(n);
    auto bytes = config;
    write_little_endian(bytes, PciRegister::Command, pciCommand, 2);
    bool pending = false;
    for (const auto &q : cq)
        pending |= q.irq && q.pending;
    write_little_endian(bytes, PciRegister::StatusRegister, pending ? PciInterruptPending : 0, 2);
    write_little_endian(bytes, PciRegister::BaseAddress, probe ? BarAddressMask : bar, 4);
    return read_little_endian(bytes, o, n);
}
// Apply BAR/Command and platform routing writes, then refresh INTx.
// NVMe 1.1a; board bounds, synchronous execution and compatibility are host policy.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=19
void Nvme::configWrite(uint64_t o, uint64_t v, unsigned n) {
    validate_access(o & (FunctionConfigurationBytes - 1), n, FunctionConfigurationBytes);
    if (o >= ConventionalConfigurationBytes)
        return;
    if (o == PciRegister::BaseAddress && n == 4) {
        probe = uint32_t(v) == UINT32_MAX;
        if (!probe)
            bar = v & BarAddressMask;
        return;
    }
    for (unsigned i = 0; i < n; ++i) {
        const auto at = o + i;
        const uint8_t byte = v >> (BitsPerByte * i);
        if (at == PciRegister::Command)
            pciCommand = (pciCommand & HighByteMask) | (byte & CommandLowWritable);
        if (at == PciRegister::Command + 1)
            pciCommand = (pciCommand & LowByteMask) | (uint16_t(byte & CommandHighWritable) << 8);
        if (at == PciRegister::CacheLineSize || at == PciRegister::LatencyTimer ||
            at == PciRegister::InterruptLine)
            config[at] = byte;
    }
    update_intx();
}
// Test enabled BAR decoding inside this board’s PCI memory aperture.
// NVMe 1.1a; board bounds, synchronous execution and compatibility are host policy.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=21
bool Nvme::contains(uint64_t a, unsigned n) const {
    return (pciCommand & PciMemoryEnable) && bar >= BoardMemoryBegin && bar < BoardMemoryEnd &&
           a >= bar && a - bar < BarBytes && n <= BarBytes - (a - bar);
}
// Resolve a bounded guest physical RAM span; return empty for invalid DMA.
// NVMe 1.1a; board bounds, synchronous execution and compatibility are host policy.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=49
std::span<uint8_t> Nvme::memory(uint64_t a, size_t n) {
    if (a < ramBase || a - ramBase > ram.size() || n > ram.size() - (a - ramBase))
        return {};
    return ram.subspan(a - ramBase, n);
}
// Assert INTx for a pending interrupt-enabled CQ unless vector/PCI masking disables it.
// NVMe 1.1a; board bounds, synchronous execution and compatibility are host policy.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=38
void Nvme::update_intx() {
    bool pending = false;
    for (const auto &q : cq)
        pending |= q.irq && q.pending;
    interrupt(pending && !(interruptMask & InterruptVectorZero) &&
              !(pciCommand & PciInterruptDisable));
}
// Synthesize controller register values; unused BAR registers read zero.
// NVMe 1.1a; board bounds, synchronous execution and compatibility are host policy.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=36
uint64_t Nvme::read(uint64_t a, unsigned n) const {
    if (!contains(a, n))
        throw std::runtime_error("NVMe BAR is not enabled/mapped");
    auto o = a - bar;
    validate_access(o, n, BarBytes);
    std::array<uint8_t, ControllerRegisterImageBytes> registers{};
    // MQES=255, contiguous queues, 500ms timeout, NVM, 4 KiB pages only.
    write_little_endian(registers, Register::ControllerCapabilitiesRegister,
                        uint64_t(MaximumQueueEntries - 1) | ContiguousQueuesRequired |
                            Timeout500Milliseconds | NvmCommandSetSupported,
                        8);
    write_little_endian(registers, Register::VersionRegister, ControllerVersion11, 4);
    write_little_endian(registers, Register::InterruptMaskSetRegister, interruptMask, 4);
    write_little_endian(registers, Register::InterruptMaskClearRegister, interruptMask, 4);
    write_little_endian(registers, Register::ControllerConfigurationRegister, cc, 4);
    write_little_endian(registers, Register::ControllerStatusRegister, csts, 4);
    write_little_endian(registers, Register::AdminQueueAttributesRegister, aqa, 4);
    write_little_endian(registers, Register::AdminSubmissionQueueRegister, asq, 8);
    write_little_endian(registers, Register::AdminCompletionQueueRegister, acq, 8);
    return o + n <= registers.size() ? read_little_endian(registers, o, n) : 0;
}
// Validate Admin geometry and supported CC format, initialize queues or set fatal status.
// NVMe 1.1a; board bounds, synchronous execution and compatibility are host policy.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=38
void Nvme::enable_controller() {
    ++resets;
    asyncPending = 0;
    queueFeature = DefaultQueueFeature;
    asyncConfig = 0;
    coalescing = 0;
    sq = {};
    cq = {};
    // Only round-robin NVM, 4 KiB pages, 64-byte SQ / 16-byte CQ entries.
    if ((cc & InitialModeMask) != ControllerEnable ||
        ((cc >> EntrySizeFieldShift) & EntrySizeFieldMask) != ExpectedEntrySizes ||
        (aqa & AdminQueueReservedMask) || (aqa & AdminQueueSizeMask) >= MaximumQueueEntries ||
        (aqa >> HighWordShift) >= MaximumQueueEntries || (asq & PageOffsetMask) ||
        (acq & PageOffsetMask) ||
        memory(asq, ((aqa & AdminQueueSizeMask) + 1) * SubmissionEntryBytes).empty() ||
        memory(acq, ((aqa >> HighWordShift) + 1) * CompletionEntryBytes).empty()) {
        csts = ControllerFatal;
        return;
    }
    sq[0].base = asq;
    sq[0].size = (aqa & AdminQueueSizeMask) + 1;
    cq[0].base = acq;
    cq[0].size = (aqa >> HighWordShift) + 1;
    cq[0].irq = true;
    csts = ControllerReady;
}
// Apply controller/mask/Admin setup or queue doorbells and process synchronous commands.
// NVMe 1.1a; board bounds, synchronous execution and compatibility are host policy.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=38
void Nvme::write(uint64_t a, uint64_t v, unsigned n) {
    if (!contains(a, n))
        throw std::runtime_error("NVMe BAR is not enabled/mapped");
    auto o = a - bar;
    validate_access(o, n, BarBytes);
    if (o == Register::InterruptMaskSetRegister && n == 4)
        interruptMask |= v;
    else if (o == Register::InterruptMaskClearRegister && n == 4)
        interruptMask &= ~v;
    else if (o == Register::ControllerConfigurationRegister && n == 4) {
        bool wasEnabled = cc & ControllerEnable;
        cc = v;
        if (!(cc & ControllerEnable)) {
            csts = 0;
            sq = {};
            cq = {};
        } else if (!wasEnabled)
            enable_controller();
        if ((cc & ControllerEnable) && (cc & ShutdownNotificationMask)) {
            disk.flush();
            csts = (csts & ~ShutdownStatusMask) | ShutdownComplete;
        }
    } else if (!(cc & ControllerEnable)) {
        if (o == Register::AdminQueueAttributesRegister && n == 4)
            aqa = v;
        if ((o == Register::AdminSubmissionQueueRegister ||
             o == Register::AdminCompletionQueueRegister) &&
            n == 8)
            (o == Register::AdminSubmissionQueueRegister ? asq : acq) = v;
        if (o >= Register::AdminSubmissionQueueRegister &&
            o <= Register::AdminCompletionQueueRegister + sizeof(uint32_t) && n == 4) {
            auto &address = o < Register::AdminCompletionQueueRegister ? asq : acq;
            unsigned shift = (o & CompletionDoorbellOffset) * 8;
            address = (address & ~(uint64_t(UINT32_MAX) << shift)) | ((v & UINT32_MAX) << shift);
        }
    } else if (o >= Register::DoorbellRegisters &&
               o < Register::DoorbellRegisters + QueueCount * DoorbellPairBytes && n == 4 &&
               (csts & ControllerReady) && (pciCommand & PciBusMasterEnable)) {
        unsigned id = (o - Register::DoorbellRegisters) / DoorbellPairBytes;
        if (o & CompletionDoorbellOffset) {
            auto &q = cq[id];
            if (!q.size || v >= q.size) {
                csts |= ControllerFatal;
                return;
            }
            unsigned advanced = (unsigned(v) + q.size - q.head) % q.size;
            if (advanced > q.pending) {
                csts |= ControllerFatal;
                return;
            }
            q.head = v;
            q.pending -= advanced;
            for (unsigned i = 0; i < QueueCount; ++i)
                if (sq[i].size && sq[i].cq == id)
                    process_submission_queue(i);
        } else {
            auto &q = sq[id];
            if (!q.size || v >= q.size) {
                csts |= ControllerFatal;
                return;
            }
            q.tail = v;
            process_submission_queue(id);
        }
    }
    update_intx();
}
// Validate all PRP buffers/list entries before copying a data payload in either direction.
// NVMe 1.1a; board bounds, synchronous execution and compatibility are host policy.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=49
bool Nvme::transfer_prp_data(std::span<const uint8_t> cmd, std::span<uint8_t> data, bool toGuest) {
    // Validate the entire PRP scatter list before touching memory or disk.
    if (cmd[CommandField::Flags] & PointerFormatMask)
        return false; // Only PRP, no SGL.
    uint64_t address = read_little_endian(cmd, CommandField::Prp1, 8),
             second = read_little_endian(cmd, CommandField::Prp2, 8);
    if (address & FirstPrpAlignmentMask)
        return false;
    size_t left = data.size(),
           first = std::min(left, size_t(PageBytes - (address & PageOffsetMask)));
    std::vector<std::span<uint8_t>> pieces;
    // Collect a complete validated PRP piece before performing any data copy.
    // https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=49
    auto add = [&](uint64_t p, size_t bytes) {
        auto span = memory(p, bytes);
        if (span.size() != bytes)
            return false;
        pieces.push_back(span);
        return true;
    };
    if (!add(address, first))
        return false;
    left -= first;
    if (left && left <= PageBytes) {
        if ((second & PageOffsetMask) || !add(second, left))
            return false;
        left = 0;
    } else if (left) {
        // MDTS=5 limits requests to 128 KiB, so a single PRP list suffices.
        if (second & PrpListAlignmentMask)
            return false;
        auto list = memory(second, ((left + PageOffsetMask) / PageBytes) * PrpEntryBytes);
        if (list.empty() || list.size() > PageBytes - (second & PageOffsetMask))
            return false;
        for (unsigned i = 0; left; ++i) {
            address = read_little_endian(list, i * PrpEntryBytes, 8);
            size_t bytes = std::min(left, size_t(PageBytes));
            if ((address & PageOffsetMask) || !add(address, bytes))
                return false;
            left -= bytes;
        }
    }
    size_t at = 0;
    for (auto piece : pieces) {
        if (toGuest)
            std::copy_n(data.begin() + at, piece.size(), piece.begin());
        else
            std::copy(piece.begin(), piece.end(), data.begin() + at);
        at += piece.size();
    }
    return true;
}
// Execute the supported Admin/NVM command subset and return unshifted completion status.
// NVMe 1.1a; board bounds, synchronous execution and compatibility are host policy.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=64
uint16_t Nvme::execute_command(unsigned qid, std::span<const uint8_t> cmd, uint32_t &result,
                               bool &defer) {
    unsigned op = cmd[CommandField::Opcode];
    uint32_t ns = read_little_endian(cmd, CommandField::NamespaceId, 4),
             d10 = read_little_endian(cmd, CommandField::Dword10, 4),
             d11 = read_little_endian(cmd, CommandField::Dword11, 4);
    if ((cmd[CommandField::Flags] & (PointerFormatMask | FusedCommandMask)) ||
        read_little_endian(cmd, CommandField::MetadataPointer, 8))
        return Status::InvalidField;
    // Queue zero selects Admin opcodes. Other queue IDs select the NVM command set;
    // equal opcode numbers in those namespaces mean different operations.
    if (!qid) {
        // AER slots are held without a completion until reset: this model generates no events.
        if (op == AdminCommand::AsynchronousEventRequest) {
            if (asyncPending == MaximumAsyncRequests)
                return Status::AsyncRequestLimitExceeded;
            ++asyncPending;
            defer = true;
            return Status::Success;
        } // AER: no events generated, retained until reset.
        if (op == AdminCommand::Identify) {
            std::array<uint8_t, IdentifyBytes> data{};
            unsigned cns = d10 & CommandSelectorMask;
            if (cns == IdentifySelector::Controller) {
                write_little_endian(data, IdentifyControllerField::VendorId, PciVendorId, 2);
                write_little_endian(data, IdentifyControllerField::SubsystemVendorId, PciVendorId,
                                    2);
                std::fill(data.begin() + IdentifyControllerField::SerialNumber,
                          data.begin() + IdentifyControllerField::FirmwareRevision +
                              FirmwareRevisionBytes,
                          ' ');
                std::memcpy(data.data() + IdentifyControllerField::SerialNumber,
                            "PCEMU000000000000001", SerialNumberBytes);
                std::memcpy(data.data() + IdentifyControllerField::ModelNumber, "PCEmu NVMe", 10);
                std::memcpy(data.data() + IdentifyControllerField::FirmwareRevision, "0.1     ",
                            FirmwareRevisionBytes);
                data[IdentifyControllerField::MaximumDataTransferSize] = MaximumTransferExponent;
                write_little_endian(data, IdentifyControllerField::ControllerId, 1, 2);
                // COMPATIBILITY: 1.1a reserves bytes 80..255; retain later-style VER readback.
                write_little_endian(data, IdentifyControllerField::VersionCompatibility,
                                    ControllerVersion11, 4);
                data[IdentifyControllerField::AsyncRequestLimit] =
                    MaximumAsyncRequests - 1; // Four outstanding async event requests.
                data[IdentifyControllerField::SubmissionEntrySize] =
                    (SubmissionEntryLog2 << 4) | SubmissionEntryLog2;
                data[IdentifyControllerField::CompletionEntrySize] =
                    (CompletionEntryLog2 << 4) | CompletionEntryLog2;
                write_little_endian(data, IdentifyControllerField::NamespaceCount, 1, 4);
                write_little_endian(data, IdentifyControllerField::OptionalNvmCommands, 0,
                                    2); // No optional NVM commands, no volatile cache.
            } else if (cns == IdentifySelector::Namespace && ns == NamespaceOne) {
                write_little_endian(data, IdentifyNamespaceField::Size, disk.sectors, 8);
                write_little_endian(data, IdentifyNamespaceField::Capacity, disk.sectors, 8);
                write_little_endian(data, IdentifyNamespaceField::Utilization, disk.sectors, 8);
                data[IdentifyNamespaceField::LbaFormat0DataSize] =
                    LogicalBlockLog2; // LBAF0: 512-byte logical blocks, no metadata.
            } else if (cns == IdentifySelector::ActiveNamespaceList) {
                if (ns == 0)
                    write_little_endian(data, 0, 1, 4);
            } else
                return cns == IdentifySelector::Namespace ? Status::InvalidNamespace
                                                          : Status::InvalidField;
            return transfer_prp_data(cmd, data, true) ? Status::Success : Status::DataTransferError;
        }
        if (op == AdminCommand::CreateSubmissionQueue ||
            op == AdminCommand::CreateCompletionQueue) {
            // CDW10: QID low 16, QSIZE high 16 (zero-based). CDW11: PC bit 0;
            // CQ adds IEN bit 1/vector high 16, while SQ names its CQ in high 16.
            // https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=68
            unsigned id = d10 & CommandLowWordMask, count = (d10 >> HighWordShift) + 1;
            unsigned completion = d11 >> HighWordShift;
            auto base = read_little_endian(cmd, CommandField::Prp1, 8);
            if (!id || id >= QueueCount || count < 2 || count > MaximumQueueEntries ||
                !(d11 & QueuePhysicallyContiguous) || (base & PageOffsetMask) ||
                memory(base,
                       count * (op == AdminCommand::CreateSubmissionQueue ? SubmissionEntryBytes
                                                                          : CompletionEntryBytes))
                    .empty())
                return Status::InvalidField;
            if (op == AdminCommand::CreateCompletionQueue) {
                if (id > (queueFeature >> HighWordShift) + 1)
                    return Status::InvalidField;
                if (cq[id].size || completion)
                    return Status::InvalidField; // Only interrupt vector zero.
                cq[id] = {};
                cq[id].base = base;
                cq[id].size = count;
                cq[id].irq = d11 & QueueInterruptEnable;
            } else {
                if (id > (queueFeature & CommandLowWordMask) + 1)
                    return Status::InvalidField;
                if (sq[id].size || completion >= QueueCount || !cq[completion].size)
                    return Status::InvalidField;
                sq[id] = {};
                sq[id].base = base;
                sq[id].size = count;
                sq[id].cq = completion;
            }
            return Status::Success;
        }
        if (op == AdminCommand::DeleteSubmissionQueue ||
            op == AdminCommand::DeleteCompletionQueue) {
            unsigned id = d10 & CommandLowWordMask;
            if (!id || id >= QueueCount)
                return Status::InvalidField;
            if (op == AdminCommand::DeleteSubmissionQueue) {
                if (!sq[id].size)
                    return Status::InvalidField;
                sq[id] = {};
            } else {
                for (const auto &q : sq)
                    if (q.size && q.cq == id)
                        return Status::InvalidQueueDeletion;
                if (!cq[id].size)
                    return Status::InvalidField;
                cq[id] = {};
            }
            return Status::Success;
        }
        if (op == AdminCommand::SetFeatures || op == AdminCommand::GetFeatures) {
            unsigned feature = d10 & CommandSelectorMask;
            if (d10 & ~CommandSelectorMask)
                return Status::InvalidField; // No save/select support.
            if (feature == Feature::NumberOfQueues) {
                if (op == AdminCommand::SetFeatures) {
                    for (unsigned i = 1; i < QueueCount; ++i)
                        if (sq[i].size || cq[i].size)
                            return Status::QueueCountChangeCompatibilityError;
                    queueFeature =
                        std::min(d11 & CommandLowWordMask, QueueCount - 2) |
                        (std::min(d11 >> HighWordShift, QueueCount - 2) << HighWordShift);
                }
                result = queueFeature;
                return Status::Success;
            }
            if (feature == Feature::InterruptCoalescing) {
                if (op == AdminCommand::SetFeatures)
                    coalescing = d11 & CommandLowWordMask;
                result = coalescing;
                return Status::Success;
            }
            if (feature == Feature::AsynchronousEventConfiguration) {
                if (op == AdminCommand::SetFeatures)
                    asyncConfig = d11 & CommandSelectorMask;
                result = asyncConfig;
                return Status::Success;
            }
            if (feature == Feature::InterruptVectorConfiguration)
                return (d11 & CommandLowWordMask) ? Status::InvalidField : Status::Success;
            if (feature == Feature::VolatileWriteCache || feature == Feature::Arbitration ||
                feature == Feature::PowerManagement)
                return op == AdminCommand::SetFeatures && d11 ? Status::InvalidField
                                                              : Status::Success;
            return Status::InvalidField;
        }
        if (op == AdminCommand::GetLogPage) {
            unsigned log = d10 & CommandSelectorMask,
                     bytes = (((d10 >> HighWordShift) & LogDwordCountMask) + 1) * CommandDwordBytes;
            if (log != LogPage::ErrorInformation && log != LogPage::HealthInformation &&
                log != LogPage::FirmwareSlotInformation)
                return Status::InvalidField;
            std::vector<uint8_t> data(bytes);
            if (log == LogPage::HealthInformation && bytes >= 3)
                write_little_endian(data, HealthTemperatureOffset, SyntheticTemperatureKelvin,
                                    sizeof(uint16_t)); // 300 Kelvin.
            if (log == LogPage::FirmwareSlotInformation && bytes >= 16) {
                data[FirmwareActiveSlotOffset] = ActiveFirmwareSlot1;
                std::memcpy(data.data() + FirmwareSlot1RevisionOffset, "0.1     ",
                            FirmwareRevisionBytes);
            }
            return transfer_prp_data(cmd, data, true) ? Status::Success : Status::DataTransferError;
        }
        if (op == AdminCommand::Abort) {
            result = AbortCommandNotFound;
            return Status::Success;
        } // Abort: command not found.
        return Status::InvalidOpcode;
    }
    if (ns != NamespaceOne && !(op == IoCommand::Flush && ns == AllNamespaces))
        return Status::InvalidNamespace;
    if (op == IoCommand::Flush) {
        disk.flush();
        return Status::Success;
    }
    if (op != IoCommand::Write && op != IoCommand::Read)
        return Status::InvalidOpcode;
    uint64_t lba = read_little_endian(cmd, CommandField::Dword10, 8);
    unsigned count = (read_little_endian(cmd, CommandField::Dword12, 4) & CommandLowWordMask) + 1;
    if (count > MaximumTransferBlocks ||
        (read_little_endian(cmd, CommandField::Dword12, 4) & ProtectionInformationMask))
        return Status::InvalidField;
    if (lba >= disk.sectors || count > disk.sectors - lba)
        return Status::LbaOutOfRange;
    std::vector<uint8_t> data(count * LogicalBlockBytes);
    if (op == IoCommand::Write) {
        if (!transfer_prp_data(cmd, data, false))
            return Status::DataTransferError;
        if (!disk.write(lba, count, data.data()))
            return Status::InternalDeviceError;
        disk.flush(); // No volatile write cache advertised.
    } else {
        if (!disk.read(lba, count, data.data()))
            return Status::InternalDeviceError;
        if (!transfer_prp_data(cmd, data, true))
            return Status::DataTransferError;
    }
    return Status::Success;
}
// Consume SQ commands while CQ space exists, publish completion fields/phase and refresh INTx.
// NVMe 1.1a; board bounds, synchronous execution and compatibility are host policy.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=44
void Nvme::process_submission_queue(unsigned id) {
    auto &s = sq[id];
    if (!s.size || !(csts & ControllerReady) || (csts & ControllerFatal))
        return;
    auto &c = cq[s.cq];
    while (s.head != s.tail && c.size && c.pending < c.size - 1) {
        auto commandMemory = memory(s.base + s.head * SubmissionEntryBytes, SubmissionEntryBytes);
        std::array<uint8_t, SubmissionEntryBytes> cmd{};
        if (commandMemory.size() != SubmissionEntryBytes) {
            csts |= ControllerFatal;
            return;
        }
        std::copy(commandMemory.begin(), commandMemory.end(), cmd.begin());
        s.head = (s.head + 1) % s.size;
        uint32_t result = 0;
        bool defer = false;
        uint16_t status = execute_command(id, cmd, result, defer);
        ++requests;
        if (id)
            ++ioRequests;
        else
            ++adminRequests;
        if (status) {
            ++errors;
            lastError = (uint32_t(cmd[CommandField::Opcode]) << HighWordShift) | status;
        }
        if (defer)
            continue;
        auto completion = memory(c.base + c.tail * CompletionEntryBytes, CompletionEntryBytes);
        if (completion.size() != CompletionEntryBytes) {
            csts |= ControllerFatal;
            return;
        }
        write_little_endian(completion, CompletionField::Result, result, 4);
        write_little_endian(completion, CompletionField::Reserved, 0, 4);
        write_little_endian(completion, CompletionField::SubmissionHead, s.head, 2);
        write_little_endian(completion, CompletionField::SubmissionId, id, 2);
        write_little_endian(completion, CompletionField::CommandIdentifier,
                            read_little_endian(cmd, CommandField::Identifier, 2), 2);
        write_little_endian(completion, CompletionField::StatusField, (status << 1) | c.phase, 2);
        if (++c.tail == c.size) {
            c.tail = 0;
            c.phase = !c.phase;
        }
        ++c.pending;
    }
    update_intx();
}
// Describe host request/error counters and current CC/CSTS without changing guest state.
// NVMe 1.1a; board bounds, synchronous execution and compatibility are host policy.
// https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_1a.pdf#page=36
std::string Nvme::diagnostics() const {
    std::ostringstream out;
    out << "nvmeAdmin=" << adminRequests << " nvmeIo=" << ioRequests << " nvmeErrors=" << errors
        << " nvmeResets=" << resets << std::hex << " nvmeLastError=0x" << lastError << " nvmeCC=0x"
        << cc << " nvmeCSTS=0x" << csts;
    return out.str();
}
} // namespace pc
