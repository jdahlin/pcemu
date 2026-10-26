// This eXtensible Host Controller Interface (xHCI) model connects three fixed
// Universal Serial Bus (USB) full-speed Human Interface Devices (HIDs): keyboard,
// relative mouse and absolute pointer tablet. PCI configuration maps a 16 KiB BAR.
// Capability registers tell the guest where operational/runtime/doorbell registers
// live and advertise xHCI 1.00, eight slots, one interrupter and 32-byte contexts.
// There are no hubs, hotplug inventory, SuperSpeed links or arbitrary USB passthrough.
//
// Guest software resets/starts the controller, supplies a command ring, Device
// Context Base Address Array (DCBAA), input/output contexts and an event ring.
// Sixteen-byte Transfer Request Blocks (TRBs) use cycle bits to exchange ownership.
// Commands allocate/address slots and configure endpoints. Transfers use control
// endpoint context 1 or interrupt-IN endpoint context 3 (USB endpoint 1 IN).
// Doorbells start work; events return completion codes, slot/endpoint IDs and
// residual lengths. IMAN and USBCMD gate the real PCI INTx callback supplied by
// the board. RAM DMA/circular walks are validated and bounded on the VM thread.
//
// The fixed USB devices expose standard device/configuration/interface/endpoint
// descriptors plus HID report descriptors. Reports define modifier/six-key arrays,
// signed relative motion or little-endian absolute X/Y with a relative wheel.
// Host frontends supply usages/motion; queues retain complete reports until an
// interrupt-IN TRB is available. An empty queue returns a USB negative
// acknowledgment (NAK), asking the guest controller to retry later.
// Control setup requests discover descriptors and store configuration/idle/protocol.
//
// IMPLEMENTATION NOTE: All execution occurs on the VM-owning thread. Reset,
// commands/transfers and port reset complete synchronously. One event segment,
// bounded ring batches and report queues protect host resources; oldest input is
// dropped at the queue cap. PCI placement limits and QEMU-compatible identity
// are board/compatibility policy. Activity counters survive reset; input does not.
// COMPATIBILITY: ERDP low writes release all tracked events, not just entries
// through the new dequeue position. Port reset always enables the fixed device.
// Sparse/unimplemented registers read zero; interrupt moderation/HSEE/EWE are
// stored without timing/error-routing effects. HID GET_REPORT returns zero data,
// SET_REPORT LEDs are ignored, SET_IDLE is stored without periodic retransmission;
// several standard feature/address/interface requests are acknowledged as no-ops.
// FIXME: Implement the specified event-reclamation/full-ring protocol, port-change
// status/timing, command/context validation and remaining control effects before
// claiming xHCI conformance. HID reports should reflect GET_REPORT/current state,
// honor idle/output requests, and either restrict accepted normal key usages or
// advertise them beyond the descriptor's current 00h..65h range. Preserve these
// guest-visible behaviors in this naming/documentation migration.
// References: Intel xHCI Requirements Specification 625472 rev 1.2b (April 2023),
// chapters 5-7, used for this older 1.00 subset; USB HID 1.11 (June 27 2001),
// sections 6.2/7.2 and appendices; USB chapter-9 wire values in Linux ch9.h.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=347
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=393
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=448
// https://www.usb.org/sites/default/files/documents/hid1_11.pdf#page=61
// https://github.com/torvalds/linux/blob/master/include/uapi/linux/usb/ch9.h#L78

#include "usb_xhci.hpp"
#include <algorithm>
#include <sstream>
#include <stdexcept>
namespace pc {
namespace {
// PCI generic xHCI identity used by QEMU: Red Hat vendor 1b36h, device 000dh;
// class 0c/03/30 means serial bus/USB/xHCI, revision 01h. Subsystem mirrors identity.
// IMPLEMENTATION NOTE: Compatibility identity, not a claim of QEMU implementation equivalence.
// https://github.com/qemu/qemu/blob/master/hw/usb/hcd-xhci-pci.c#L276
constexpr uint32_t PciIdentity = 0x000d1b36, PciClassRevision = 0x0c033001;
// PCI Command MSE/BME/INTx-disable. BAR0 is 16 KiB aligned memory, INTA pin 1,
// interrupt line ffh unassigned. Byte order follows PCI configuration space.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=218
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=223
struct PciRegister {
    enum : unsigned {
        Identity = 0,
        Command = 4,
        ClassRevision = 8,
        BaseAddressRegister0 = 0x10,
        SubsystemIdentity = 0x2c,
        InterruptLinePin = 0x3c
    };
};
constexpr uint32_t MemorySpaceEnable = 2, BusMasterEnable = 4, IntxDisable = 0x400,
                   PciCommandMask = MemorySpaceEnable | BusMasterEnable | IntxDisable,
                   BarBytes = 0x4000, BarAddressMask = ~(BarBytes - 1), PciInterruptLinePin = 0x1ff;
// IMPLEMENTATION NOTE: Board-assigned BAR aperture, not an xHCI address requirement.
constexpr uint64_t MinimumBarAddress = 0x10000000, BarAddressLimit = 0x3f000000;
// All xHCI offsets are relative to BAR0. Capability length 40h selects the
// operational base; runtime/doorbell/capability offsets are advertised to drivers.
// Intel 625472 rev 1.2b, 5.3/5.4/5.5/5.6/7.2.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=347
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=356
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=389
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=480
struct Register {
    enum : unsigned {
        CapabilityLengthVersionRegister = 0,
        StructuralParameters1Register = 4,
        CapabilityParameters1Register = 0x10,
        DoorbellOffsetRegister = 0x14,
        RuntimeOffsetRegister = 0x18,
        CommandRegister = 0x40,
        StatusRegister = 0x44,
        PageSizeRegister = 0x48,
        CommandRingControlLowRegister = 0x58,
        CommandRingControlHighRegister = 0x5c,
        DeviceContextBaseArrayLowRegister = 0x70,
        DeviceContextBaseArrayHighRegister = 0x74,
        ConfigureRegister = 0x78,
        PortStatusControlFirstRegister = 0x440,
        PortStatusControlSecondRegister = 0x450,
        PortStatusControlThirdRegister = 0x460,
        InterrupterManagementRegister = 0x1020,
        InterrupterModerationRegister = 0x1024,
        EventRingSegmentTableSizeRegister = 0x1028,
        EventRingSegmentTableBaseLowRegister = 0x1030,
        EventRingSegmentTableBaseHighRegister = 0x1034,
        EventRingDequeueLowRegister = 0x1038,
        EventRingDequeueHighRegister = 0x103c,
        SupportedProtocolRegister = 0x3000,
        ProtocolNameRegister = 0x3004,
        ProtocolPortRangeRegister = 0x3008,
    };
};
constexpr unsigned OperationalOffset = 0x40, DoorbellOffset = 0x2000, RuntimeOffset = 0x1000,
                   ProtocolCapabilityOffset = 0x3000, PortRegisterStride = 16,
                   DoorbellRegisterBytes = 4;
// Version field 0100h advertises xHCI 1.00; AC64=1 supports 64-bit DMA,
// CSZ=0 means 32-byte contexts; xECP points in dwords to our USB2 capability.
// PAGESIZE bit zero means 4 KiB. Other sparse capability fields read zero.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=347
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=351
constexpr unsigned InterfaceVersion = 0x0100, Address64Capability = 1, PageSize4KiB = 1;
// USB2 Supported Protocol: capability ID 2, revision 2.0, name signature "USB "
// in little-endian bytes, first compatible port 1 and three compatible ports.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=480
constexpr uint32_t Usb2SupportedProtocol = 2 | (2u << 24), UsbProtocolName = 0x20425355;
// USBCMD R/S, HCRST, INTE, HSEE and EWE; HSEE/EWE are stored only here.
// USBSTS HCH/HSE/EINT/PCD/SRE: event/error latches; RW1C uses only named mask.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=358
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=362
constexpr uint32_t RunStop = 1, HostControllerReset = 2, InterrupterEnable = 4,
                   HostSystemErrorEnable = 8, EnableWrapEvent = 0x400,
                   CommandMask =
                       RunStop | InterrupterEnable | HostSystemErrorEnable | EnableWrapEvent,
                   Halted = 1, HostSystemError = 4, EventInterrupt = 8, PortChangeDetect = 0x10,
                   SaveRestoreError = 0x400,
                   StatusWriteOneToClearMask =
                       HostSystemError | EventInterrupt | PortChangeDetect | SaveRestoreError;
// IMAN.IP bit 0 latches pending, IMAN.IE bit 1 enables it. ERDP.EHB bit 3 is
// event-handler busy; this subset clears it when the low dequeue register is written.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=390
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=393
constexpr uint32_t InterruptPending = 1, InterruptEnable = 2, EventHandlerBusy = 8;
// PORTSC connection/power/full-speed/link fields; reset completes immediately
// with PED and PRC set and link U0. Bits [23:17] are write-one-to-clear changes.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=369
constexpr uint32_t PortConnected = 1, PortEnabled = 2, PortReset = 0x10, PortLinkStateMask = 0x1e0,
                   PortPolling = 7u << 5, PortPower = 1u << 9, FullSpeed = 1u << 10,
                   PortResetChange = 1u << 21, PortChangeMask = 0xfe0000;
// Sixteen-byte Transfer Request Blocks (TRBs): parameter u64 at +0, status u32
// at +8, control u32 at +12. Type [15:10], endpoint [20:16], slot [31:24].
// C bit 0 transfers ownership; TC bit 1 toggles Link cycles. Normal transfer
// flags ISP/CH/IOC/IDT are bits 2/4/5/6; data-stage direction is bit 16.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=427
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=438
struct TrbType {
    enum : unsigned {
        Normal = 1,
        SetupStage = 2,
        DataStage = 3,
        StatusStage = 4,
        Link = 6,
        EventData = 7,
        NoOpTransfer = 8,
        EnableSlot = 9,
        DisableSlot = 10,
        AddressDevice = 11,
        ConfigureEndpoint = 12,
        EvaluateContext = 13,
        ResetEndpoint = 14,
        StopEndpoint = 15,
        SetTransferDequeue = 16,
        ResetDevice = 17,
        NoOpCommand = 23,
        TransferEvent = 32,
        CommandCompletionEvent = 33,
        PortStatusChangeEvent = 34
    };
};
constexpr unsigned TrbBytes = 16, TrbStatusOffset = 8, TrbControlOffset = 12, TrbTypeShift = 10,
                   TrbTypeMask = 63, EndpointIdShift = 16, EndpointIdMask = 31, SlotIdShift = 24,
                   CompletionCodeShift = 24, TrbAddressMask = TrbBytes - 1,
                   CommandRingAlignmentMask = 63;
constexpr uint32_t Cycle = 1, ToggleCycle = 2, InterruptOnShortPacket = 4, Chain = 16,
                   InterruptOnCompletion = 32, ImmediateData = 64, DataStageIn = 1u << 16,
                   BlockSetAddress = 1u << 9, TransferLengthMask = 0x1ffff, EventDataFlag = 4;
// Completion codes describe xHCI operations, not USB request numbers or TRB IDs.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=465
struct Completion {
    enum : unsigned {
        Success = 1,
        TrbError = 5,
        StallError = 6,
        NoSlotsAvailable = 9,
        SlotNotEnabled = 11,
        EndpointNotEnabled = 12,
        ShortPacket = 13,
        ParameterError = 17
    };
};
// Context fields: device has 32 contexts, input adds one Input Control Context.
// State occupies low three endpoint-context bits; slot state occupies bits [31:27].
// Evaluate Context selectively updates slot latency/interrupter target or EP0 max packet size.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=407
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=412
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=422
constexpr unsigned SlotContextIndex = 0, ControlEndpointContextIndex = 1,
                   InterruptInContextIndex = 3, SlotStateShift = 27;
constexpr uint32_t ContextStateMask = 7, ContextEntriesMask = 0xf8000000,
                   SlotMaxExitLatencyMask = 0xffff, SlotInterrupterTargetMask = 0xffc00000,
                   EndpointMaxPacketSizeMask = 0xffff0000;
// DWORD offsets inside Slot/Endpoint/Input Control contexts, rather than
// device registers. RootHubPort is bits [23:16] of Slot DWORD1; USB address/state
// occupy Slot DWORD3. EP dequeue uses an address plus DCS bit zero.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=407
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=412
struct SlotField {
    enum : unsigned {
        ContextEntries = 0,
        MaxExitLatencyRootPort = 4,
        InterrupterTarget = 8,
        AddressState = 12
    };
};
struct EndpointField {
    enum : unsigned { State = 0, MaxPacketSize = 4, TransferDequeue = 8 };
};
struct InputControlField {
    enum : unsigned { DropFlags = 0, AddFlags = 4 };
};
constexpr uint32_t AddSlotAndDefaultEndpoint = 3; // Input context flags A0/A1.
struct TrbWord {
    enum : unsigned { ParameterLow = 0, ParameterHigh = 1, Status = 2, Control = 3 };
};
struct EndpointState {
    enum : unsigned { Disabled = 0, Running = 1, Halted = 2, Stopped = 3 };
};
struct SlotState {
    enum : unsigned { Default = 1, Addressed = 2, Configured = 3 };
};
// IMPLEMENTATION NOTE: Bounded host walks/queues and one event segment. Limits
// prevent a malicious circular guest ring consuming unbounded host time/memory.
constexpr unsigned RingWalkBudget = 256, MaximumEventEntries = 4096, MaximumPendingReports = 256;
// Standard USB control requests / descriptors; class HID request numbers overlap
// standard numbers, so bmRequestType selects meaning. USB 2.0 chapter 9 / Linux ch9.h.
// https://github.com/torvalds/linux/blob/master/include/uapi/linux/usb/ch9.h#L78
struct UsbRequest {
    enum : unsigned {
        GetStatus = 0,
        ClearFeature = 1,
        SetFeature = 3,
        SetAddress = 5,
        GetDescriptor = 6,
        GetConfiguration = 8,
        SetConfiguration = 9,
        GetInterface = 10,
        SetInterface = 11
    };
};
struct DescriptorType {
    enum : unsigned {
        Device = 1,
        Configuration = 2,
        Interface = 4,
        Endpoint = 5,
        Hid = 0x21,
        Report = 0x22
    };
};
// HID 1.11 class requests, 7.2. Report protocol is 1; boot protocol is 0.
// https://www.usb.org/sites/default/files/documents/hid1_11.pdf#page=61
// Fixed device/configuration descriptor fields: USB release 1.10, HID 1.11,
// device release 1.00; interface-defined class, one configuration and no strings.
// IMPLEMENTATION NOTE: USB vendor bytes reuse the QEMU-compatible Red Hat value;
// per-port product IDs 1/2/3 identify our keyboard/mouse/tablet, not physical products.
// USB chapter 9 descriptor structures; HID 1.11 6.2.1 / Appendix E.
// https://github.com/torvalds/linux/blob/master/include/uapi/linux/usb/ch9.h#L292
// https://www.usb.org/sites/default/files/documents/hid1_11.pdf#page=32
constexpr uint16_t UsbDeviceVersion = 0x0110, HidVersion = 0x0111, DeviceReleaseVersion = 0x0100,
                   UsbVendorIdentity = 0x1b36;
constexpr uint8_t DeviceDescriptorBytes = 18, ConfigurationDescriptorBytes = 9,
                  InterfaceDescriptorBytes = 9, HidDescriptorBytes = 9, EndpointDescriptorBytes = 7,
                  ConfigurationTotalBytes = ConfigurationDescriptorBytes +
                                            InterfaceDescriptorBytes + HidDescriptorBytes +
                                            EndpointDescriptorBytes,
                  DefaultControlPacketBytes = 8, InterruptPacketBytes = 8, HidClass = 3,
                  BootSubclass = 1, NoBootProtocol = 0, KeyboardInterfaceProtocol = 1,
                  MouseInterfaceProtocol = 2, InterruptInEndpointAddress = 0x81,
                  InterruptEndpointType = 3, ConfigurationAttributes = 0xa0,
                  MaximumPower2mAUnits = 50, FullSpeedPollingIntervalMs = 10;
// Configuration attributes A0h advertise bus-powered and remote-wakeup capability;
// 50 units means 100 mA. Endpoint 81h is endpoint 1 IN, interrupt type, 8-byte
// maximum packets polled every 10 ms at full speed. Physical timing/wakeup is unmodeled.
struct HidRequest {
    enum : unsigned {
        GetReport = 1,
        GetIdle = 2,
        GetProtocol = 3,
        SetReport = 9,
        SetIdle = 10,
        SetProtocol = 11
    };
};
constexpr unsigned RequestTypeMask = 0x60, ClassRequest = 0x20, DeviceToHost = 0x80;
// HID item prefixes encode size/type/tag, not arbitrary data. The descriptors
// below use Global/Local/Main short items and annotate the resulting wire layout.
// HID 1.11, 6.2.2 Report Descriptor (Global/Local/Main items), Appendix B/D.
// https://www.usb.org/sites/default/files/documents/hid1_11.pdf#page=34
// https://www.usb.org/sites/default/files/documents/hid1_11.pdf#page=74
struct HidItem {
    enum : uint8_t {
        UsagePage = 0x05,
        Usage = 0x09,
        Collection = 0xa1,
        EndCollection = 0xc0,
        UsageMinimum = 0x19,
        UsageMaximum = 0x29,
        LogicalMinimum8 = 0x15,
        LogicalMaximum8 = 0x25,
        LogicalMaximum16 = 0x26,
        ReportSize = 0x75,
        ReportCount = 0x95,
        Input = 0x81,
        Output = 0x91
    };
};
struct UsagePage {
    enum : uint8_t { GenericDesktop = 1, KeyboardKeypad = 7, Leds = 8, Button = 9 };
};
struct DesktopUsage {
    enum : uint8_t { Pointer = 1, Mouse = 2, Keyboard = 6, X = 0x30, Y = 0x31, Wheel = 0x38 };
};
constexpr uint8_t ApplicationCollection = 1, PhysicalCollection = 0, DataVariableAbsolute = 2,
                  DataVariableRelative = 6, ConstantItem = 1, DataArrayAbsolute = 0,
                  FirstModifierUsage = 0xe0, LastModifierUsage = 0xe7, MaximumBootKeyUsage = 0x65,
                  FirstNormalKeyUsage = 4, ErrorRollOver = 1, KeyboardPort = 1, MousePort = 2,
                  TabletPort = 3, KeyboardReportBytes = 8, MouseReportBytes = 4,
                  TabletReportBytes = 6, BootMouseReportBytes = 3, KeyboardKeyArrayOffset = 2,
                  MaximumKeyboardKeys = 6, MouseButtonMask = 7;
// Descriptor axes use signed -127..127 (81h/7fh), tablet absolute X/Y 0..32767.
// Host normalized absolute input 0..65535 is rescaled; these are coordinate units.
constexpr int MaximumRelativeDelta = 127;
constexpr unsigned HostAbsoluteMaximum = 65535, TabletAbsoluteMaximum = 32767;

// Reject unsupported widths, misalignment and accesses crossing a register window.
// IMPLEMENTATION NOTE: Host register-bus access policy, before any guest-visible work.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=346
void validate_register_access(uint64_t o, unsigned w, unsigned size) {
    if ((w != 1 && w != 2 && w != 4 && w != 8) || o % w || o >= size || w > size - o)
        throw std::runtime_error("Invalid xHCI register access");
}
// Eight-byte keyboard input: modifier bitmap, reserved byte, six usage slots.
// Five LED output bits plus three padding bits. Modifier usages E0h..E7h are
// Ctrl/Shift/Alt/GUI left then right; normal usage array advertises 00h..65h.
const std::vector<uint8_t> keyboardDescriptor = {
    HidItem::UsagePage,       UsagePage::GenericDesktop,
    HidItem::Usage,           DesktopUsage::Keyboard,
    HidItem::Collection,      ApplicationCollection,
    HidItem::UsagePage,       UsagePage::KeyboardKeypad,
    HidItem::UsageMinimum,    FirstModifierUsage,
    HidItem::UsageMaximum,    LastModifierUsage,
    HidItem::LogicalMinimum8, 0,
    HidItem::LogicalMaximum8, 1,
    HidItem::ReportSize,      1,
    HidItem::ReportCount,     8,
    HidItem::Input,           DataVariableAbsolute,
    HidItem::ReportCount,     1,
    HidItem::ReportSize,      8,
    HidItem::Input,           ConstantItem,
    HidItem::ReportCount,     5,
    HidItem::ReportSize,      1,
    HidItem::UsagePage,       UsagePage::Leds,
    HidItem::UsageMinimum,    1,
    HidItem::UsageMaximum,    5,
    HidItem::Output,          DataVariableAbsolute,
    HidItem::ReportCount,     1,
    HidItem::ReportSize,      3,
    HidItem::Output,          ConstantItem,
    HidItem::ReportCount,     MaximumKeyboardKeys,
    HidItem::ReportSize,      8,
    HidItem::LogicalMinimum8, 0,
    HidItem::LogicalMaximum8, MaximumBootKeyUsage,
    HidItem::UsagePage,       UsagePage::KeyboardKeypad,
    HidItem::UsageMinimum,    0,
    HidItem::UsageMaximum,    MaximumBootKeyUsage,
    HidItem::Input,           DataArrayAbsolute,
    HidItem::EndCollection};
// Four-byte relative mouse input: three button bits + five padding, then signed
// X/Y/wheel bytes. Application Mouse contains a physical Pointer collection.
const std::vector<uint8_t> mouseDescriptor = {HidItem::UsagePage,       UsagePage::GenericDesktop,
                                              HidItem::Usage,           DesktopUsage::Mouse,
                                              HidItem::Collection,      ApplicationCollection,
                                              HidItem::Usage,           DesktopUsage::Pointer,
                                              HidItem::Collection,      PhysicalCollection,
                                              HidItem::UsagePage,       UsagePage::Button,
                                              HidItem::UsageMinimum,    1,
                                              HidItem::UsageMaximum,    3,
                                              HidItem::LogicalMinimum8, 0,
                                              HidItem::LogicalMaximum8, 1,
                                              HidItem::ReportCount,     3,
                                              HidItem::ReportSize,      1,
                                              HidItem::Input,           DataVariableAbsolute,
                                              HidItem::ReportCount,     1,
                                              HidItem::ReportSize,      5,
                                              HidItem::Input,           ConstantItem,
                                              HidItem::UsagePage,       UsagePage::GenericDesktop,
                                              HidItem::Usage,           DesktopUsage::X,
                                              HidItem::Usage,           DesktopUsage::Y,
                                              HidItem::Usage,           DesktopUsage::Wheel,
                                              HidItem::LogicalMinimum8, 0x81,
                                              HidItem::LogicalMaximum8, MaximumRelativeDelta,
                                              HidItem::ReportSize,      8,
                                              HidItem::ReportCount,     3,
                                              HidItem::Input,           DataVariableRelative,
                                              HidItem::EndCollection,   HidItem::EndCollection};
// Six-byte tablet input: button bitmap, little-endian absolute X/Y u16 limited
// to 7fffh, signed wheel byte. It is a Mouse/Pointer HID application with absolute axes.
const std::vector<uint8_t> tabletDescriptor = {HidItem::UsagePage,
                                               UsagePage::GenericDesktop,
                                               HidItem::Usage,
                                               DesktopUsage::Mouse,
                                               HidItem::Collection,
                                               ApplicationCollection,
                                               HidItem::Usage,
                                               DesktopUsage::Pointer,
                                               HidItem::Collection,
                                               PhysicalCollection,
                                               HidItem::UsagePage,
                                               UsagePage::Button,
                                               HidItem::UsageMinimum,
                                               1,
                                               HidItem::UsageMaximum,
                                               3,
                                               HidItem::LogicalMinimum8,
                                               0,
                                               HidItem::LogicalMaximum8,
                                               1,
                                               HidItem::ReportCount,
                                               3,
                                               HidItem::ReportSize,
                                               1,
                                               HidItem::Input,
                                               DataVariableAbsolute,
                                               HidItem::ReportCount,
                                               1,
                                               HidItem::ReportSize,
                                               5,
                                               HidItem::Input,
                                               ConstantItem,
                                               HidItem::UsagePage,
                                               UsagePage::GenericDesktop,
                                               HidItem::Usage,
                                               DesktopUsage::X,
                                               HidItem::Usage,
                                               DesktopUsage::Y,
                                               HidItem::LogicalMinimum8,
                                               0,
                                               HidItem::LogicalMaximum16,
                                               0xff,
                                               0x7f, // 32767 in little-endian 16-bit item data.
                                               HidItem::ReportSize,
                                               16,
                                               HidItem::ReportCount,
                                               2,
                                               HidItem::Input,
                                               DataVariableAbsolute,
                                               HidItem::Usage,
                                               DesktopUsage::Wheel,
                                               HidItem::LogicalMinimum8,
                                               0x81,
                                               HidItem::LogicalMaximum8,
                                               MaximumRelativeDelta,
                                               HidItem::ReportSize,
                                               8,
                                               HidItem::ReportCount,
                                               1,
                                               HidItem::Input,
                                               DataVariableRelative,
                                               HidItem::EndCollection,
                                               HidItem::EndCollection};
} // namespace
// Bind RAM and a real INTx callback, then initialize a halted fixed-topology controller.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=358
UsbXhci::UsbXhci(std::span<uint8_t> r, uint64_t b, std::function<void(bool)> i)
    : ram(r), ramBase(b), interrupt(std::move(i)) {
    reset_controller();
}
// Require the entire DMA interval inside supplied guest RAM; throw with bounded address/length
// detail. IMPLEMENTATION NOTE: MMIO or unmapped DMA is unsupported, on the VM-owning thread.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=404
void UsbXhci::validate_guest_range(uint64_t a, uint64_t n) const {
    if (a < ramBase || n > ram.size() || a - ramBase > ram.size() - n) {
        std::ostringstream detail;
        detail << "xHCI DMA outside guest RAM: address=0x" << std::hex << a << " length=0x" << n;
        throw std::runtime_error(detail.str());
    }
}
// Read a validated little-endian scalar from guest DMA memory.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=426
uint64_t UsbXhci::read_guest_value(uint64_t a, unsigned w) const {
    validate_guest_range(a, w);
    uint64_t v = 0;
    for (unsigned i = 0; i < w; ++i)
        v |= uint64_t(ram[a - ramBase + i]) << (8 * i);
    return v;
}
// Write a validated little-endian scalar to guest DMA memory.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=426
void UsbXhci::write_guest_value(uint64_t a, uint64_t v, unsigned w) {
    validate_guest_range(a, w);
    for (unsigned i = 0; i < w; ++i)
        ram[a - ramBase + i] = v >> (8 * i);
}
// Reset operational/runtime/ring/device/input state and lower interrupts; preserve PCI
// state/counters. IMPLEMENTATION NOTE: Reset completes synchronously and fixed devices remain
// connected. https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=358
void UsbXhci::reset_controller() {
    usbcmd = 0;
    status = Halted;
    iman = 0;
    erstSize = 0;
    config = 0;
    imod = 0;
    failure.clear();
    crcr = dcbaa = erst = erdp = eventBase = eventIndex = eventCount = outstanding = 0;
    eventCycle = true;
    commandRing = {};
    slots = {};
    // Connected full-speed USB2 devices, port power on, link state polling.
    ports.fill(PortConnected | PortPower | FullSpeed | PortPolling);
    pending = {};
    keys = {};
    buttons = 0;
    tabletX = tabletY = InitialTabletCoordinate;
    update_intx();
}
// Drive PCI INTx when IMAN is pending/enabled, USBCMD permits interrupts and PCI permits INTx.
// IMPLEMENTATION NOTE: One interrupter with INTx, no MSI/MSI-X or moderation timing.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=390
void UsbXhci::update_intx() {
    interrupt((pciCommand & IntxDisable) == 0 && (usbcmd & InterrupterEnable) &&
              (iman & (InterruptPending | InterruptEnable)) ==
                  (InterruptPending | InterruptEnable));
}
// Read sparse PCI configuration bytes, returning fixed identity/class/INTA and BAR probe state.
// IMPLEMENTATION NOTE: QEMU-compatible identity, not full PCI capability emulation.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=214
uint64_t UsbXhci::configRead(uint64_t o, unsigned w) const {
    validate_register_access(o, w, 4096);
    uint64_t v = 0;
    for (unsigned i = 0; i < w; ++i) {
        unsigned at = o + i;
        uint32_t d = 0;
        switch (at & ~3u) {
        case PciRegister::Identity:
            d = PciIdentity;
            break; // QEMU generic xHCI compatible PCI identity
        case PciRegister::Command:
            d = pciCommand;
            break;
        case PciRegister::ClassRevision:
            d = PciClassRevision;
            break;
        case PciRegister::BaseAddressRegister0:
            d = probe ? BarAddressMask : bar;
            break;
        case PciRegister::SubsystemIdentity:
            d = PciIdentity;
            break;
        case PciRegister::InterruptLinePin:
            d = PciInterruptLinePin;
            break;
        }
        v |= uint64_t(uint8_t(d >> (8 * (at & 3)))) << (8 * i);
    }
    return v;
}
// Apply supported BAR0/Command writes; ignore other PCI configuration programming.
// https://lekensteyn.nl/files/docs/PCI_SPEV_V3_0.pdf#page=225
void UsbXhci::configWrite(uint64_t o, uint64_t v, unsigned w) {
    validate_register_access(o, w, 4096);
    if (o == PciRegister::BaseAddressRegister0 && w == 4) {
        probe = uint32_t(v) == 0xffffffff;
        if (!probe)
            bar = v & BarAddressMask;
    }
    if (o == PciRegister::Command && (w == 2 || w == 4)) {
        pciCommand = v & PciCommandMask;
        update_intx();
    }
}
// Test BAR memory decode and board aperture bounds for a complete access.
// IMPLEMENTATION NOTE: Placement restrictions belong to this ARM board, not the xHCI spec.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=346
bool UsbXhci::contains(uint64_t a, unsigned w) const {
    return (pciCommand & MemorySpaceEnable) && bar >= MinimumBarAddress && bar < BarAddressLimit &&
           a >= bar && a - bar < BarBytes && w <= BarBytes - (a - bar);
}
// Read sparse capability/operational/port/interrupter/protocol state; unknown offsets return zero.
// Capability encodings advertise xHCI 1.00, eight slots, one interrupter and three full-speed
// ports. https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=347
uint32_t UsbXhci::read_register(unsigned o) const {
    switch (o) {
    case Register::CapabilityLengthVersionRegister:
        return (InterfaceVersion << 16) | OperationalOffset; // version 1.0, operational offset 0x40
    case Register::StructuralParameters1Register:
        return (RootPortCount << 24) | (1 << 8) | MaximumSlots; // ports, interrupters, slots
    case Register::CapabilityParameters1Register:
        return Address64Capability |
               ((ProtocolCapabilityOffset / 4) << 16); // 64-bit addresses, 32-byte contexts
    case Register::DoorbellOffsetRegister:
        return DoorbellOffset; // doorbells
    case Register::RuntimeOffsetRegister:
        return RuntimeOffset; // runtime
    case Register::CommandRegister:
        return usbcmd;
    case Register::StatusRegister:
        return status;
    case Register::PageSizeRegister:
        return PageSize4KiB; // PAGESIZE bit zero means 4 KiB.
    case Register::CommandRingControlLowRegister:
        return crcr;
    case Register::CommandRingControlHighRegister:
        return crcr >> 32;
    case Register::DeviceContextBaseArrayLowRegister:
        return dcbaa;
    case Register::DeviceContextBaseArrayHighRegister:
        return dcbaa >> 32;
    case Register::ConfigureRegister:
        return config;
    case Register::InterrupterManagementRegister:
        return iman;
    case Register::InterrupterModerationRegister:
        return imod;
    case Register::EventRingSegmentTableSizeRegister:
        return erstSize;
    case Register::EventRingSegmentTableBaseLowRegister:
        return erst;
    case Register::EventRingSegmentTableBaseHighRegister:
        return erst >> 32;
    case Register::EventRingDequeueLowRegister:
        return erdp;
    case Register::EventRingDequeueHighRegister:
        return erdp >> 32;
    case Register::SupportedProtocolRegister:
        return Usb2SupportedProtocol; // USB 2.0 Supported Protocol capability
    case Register::ProtocolNameRegister:
        return UsbProtocolName; // "USB "
    case Register::ProtocolPortRangeRegister:
        return 1 | (RootPortCount << 8); // first port, port count
    default:
        if (o == Register::PortStatusControlFirstRegister ||
            o == Register::PortStatusControlSecondRegister ||
            o == Register::PortStatusControlThirdRegister)
            return ports[(o - Register::PortStatusControlFirstRegister) / PortRegisterStride];
        return 0;
    }
}
// Read aligned MMIO lanes in little-endian order, combining halves of 64-bit registers.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=346
uint64_t UsbXhci::read(uint64_t a, unsigned w) {
    auto o = a - bar;
    validate_register_access(o, w, BarBytes);
    uint64_t v = 0;
    for (unsigned i = 0; i < w; ++i)
        v |= uint64_t(uint8_t(read_register((o + i) & ~3u) >> (8 * ((o + i) & 3)))) << (8 * i);
    return v;
}
// Apply command/status/ring/runtime writes or acknowledge/reset fixed ports.
// Split 64-bit addresses retain the opposite half; errors/limitations are described above.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=356
void UsbXhci::write_register(unsigned o, uint32_t v) {
    auto part = [&](uint64_t &r, bool high) {
        r = high ? (r & 0xffffffffull) | (uint64_t(v) << 32) : (r & 0xffffffff00000000ull) | v;
    };
    switch (o) {
    case Register::CommandRegister:
        if (v & HostControllerReset) {
            reset_controller();
            break;
        }
        usbcmd = v & CommandMask;
        status = (status & ~Halted) | ((v & RunStop) ? 0 : Halted);
        update_intx();
        break;
    case Register::StatusRegister:
        status &= ~(v & StatusWriteOneToClearMask);
        update_intx();
        break;
    case Register::CommandRingControlLowRegister:
        part(crcr, false);
        commandRing = {crcr & ~uint64_t(CommandRingAlignmentMask), bool(crcr & Cycle)};
        break;
    case Register::CommandRingControlHighRegister:
        part(crcr, true);
        commandRing = {crcr & ~uint64_t(CommandRingAlignmentMask), bool(crcr & Cycle)};
        break;
    case Register::DeviceContextBaseArrayLowRegister:
        part(dcbaa, false);
        break;
    case Register::DeviceContextBaseArrayHighRegister:
        part(dcbaa, true);
        break;
    case Register::ConfigureRegister:
        config = v & 0xff;
        break;
    case Register::InterrupterManagementRegister:
        iman = (iman & InterruptPending & ~(v & InterruptPending)) | (v & InterruptEnable);
        update_intx();
        break;
    case Register::InterrupterModerationRegister:
        imod = v;
        break;
    case Register::EventRingSegmentTableSizeRegister:
        erstSize = v & 0xffff;
        break;
    case Register::EventRingSegmentTableBaseLowRegister:
        part(erst, false);
        eventBase = 0;
        break;
    case Register::EventRingSegmentTableBaseHighRegister:
        part(erst, true);
        eventBase = 0;
        break;
    case Register::EventRingDequeueLowRegister:
        part(erdp, false);
        erdp &= ~uint64_t(EventHandlerBusy);
        outstanding = 0;
        break;
    case Register::EventRingDequeueHighRegister:
        part(erdp, true);
        break;
    default:
        if (o == Register::PortStatusControlFirstRegister ||
            o == Register::PortStatusControlSecondRegister ||
            o == Register::PortStatusControlThirdRegister) {
            auto &p = ports[(o - Register::PortStatusControlFirstRegister) / PortRegisterStride];
            p &= ~(v & PortChangeMask); // RW1C change bits
            if (v & PortReset) {
                p = (p & ~(PortReset | PortLinkStateMask)) | PortEnabled |
                    PortResetChange; // reset complete, enabled, U0
                if (usbcmd & RunStop)
                    enqueue_event(
                        uint64_t(
                            (o - Register::PortStatusControlFirstRegister) / PortRegisterStride + 1)
                            << 24,
                        Completion::Success << CompletionCodeShift,
                        TrbType::PortStatusChangeEvent << TrbTypeShift);
            }
        }
        break;
    }
}
// Apply dword/qword register or doorbell writes; convert service exceptions into HSE/halted state.
// IMPLEMENTATION NOTE: Byte/word writes throw before service; accepted DMA failures latch
// diagnostics. https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=362
void UsbXhci::write(uint64_t a, uint64_t v, unsigned w) {
    auto o = a - bar;
    validate_register_access(o, w, BarBytes);
    if (w != 4 && w != 8)
        throw std::runtime_error("xHCI writes require dwords");
    try {
        if (o >= DoorbellOffset &&
            o < DoorbellOffset + (MaximumSlots + 1) * DoorbellRegisterBytes) {
            if ((pciCommand & BusMasterEnable) && (usbcmd & RunStop))
                ring_doorbell((o - DoorbellOffset) / DoorbellRegisterBytes, v & 0xff);
        } else {
            write_register(o, v);
            if (w == 8)
                write_register(o + 4, v >> 32);
        }
    } catch (const std::runtime_error &e) {
        failure = e.what();
        status |= HostSystemError;
        usbcmd &= ~RunStop;
        status |= Halted;
        update_intx();
    }
}
// Publish parameter/status/cycle-tagged control to the single event segment and signal its
// interrupter. IMPLEMENTATION NOTE: Reserve one entry; unsupported/full segment state throws and
// halts the controller. https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=391
void UsbXhci::enqueue_event(uint64_t p, uint32_t s, uint32_t c) {
    if (!(pciCommand & BusMasterEnable))
        return;
    if (!eventBase) {
        if (erstSize != 1)
            throw std::runtime_error("xHCI needs one event segment");
        eventBase = read_guest_value(erst, 8);
        eventCount = read_guest_value(erst + 8, 4);
        if (!eventCount || eventCount > MaximumEventEntries ||
            eventBase % (CommandRingAlignmentMask + 1))
            throw std::runtime_error("Invalid xHCI event ring");
        validate_guest_range(eventBase, eventCount * TrbBytes);
        eventIndex = 0;
        eventCycle = true;
    }
    if (outstanding >= eventCount - 1)
        throw std::runtime_error("xHCI event ring full");
    auto a = eventBase + eventIndex * TrbBytes;
    write_guest_value(a, p, 8);
    write_guest_value(a + TrbStatusOffset, s, 4);
    write_guest_value(a + TrbControlOffset, c | unsigned(eventCycle), 4);
    if (++eventIndex == eventCount) {
        eventIndex = 0;
        eventCycle = !eventCycle;
    }
    ++outstanding;
    iman |= InterruptPending;
    status |= EventInterrupt;
    update_intx();
}
// Consume one cycle-owned TRB, following bounded links and Toggle Cycle before advancing.
// Stop without consuming when cycle ownership differs; reject bad pointers or excessive links.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=426
bool UsbXhci::next_transfer_ring_block(Ring &r, std::array<uint32_t, 4> &t, uint64_t &a) {
    for (unsigned n = 0; n < RingWalkBudget; ++n) {
        if (!r.pointer || r.pointer % TrbBytes)
            throw std::runtime_error("Invalid xHCI dequeue pointer");
        validate_guest_range(r.pointer, TrbBytes);
        for (unsigned i = 0; i < 4; ++i)
            t[i] = read_guest_value(r.pointer + i * 4, 4);
        if (bool(t[TrbWord::Control] & Cycle) != r.cycle)
            return false;
        a = r.pointer;
        if (((t[TrbWord::Control] >> TrbTypeShift) & TrbTypeMask) != TrbType::Link) {
            r.pointer += TrbBytes;
            return true;
        }
        r.pointer = (uint64_t(t[TrbWord::ParameterHigh]) << 32 | t[TrbWord::ParameterLow]) &
                    ~uint64_t(TrbAddressMask);
        if (t[TrbWord::Control] & ToggleCycle)
            r.cycle = !r.cycle;
    }
    throw std::runtime_error("Cyclic xHCI link ring");
}
// Publish output slot/endpoint state and an endpoint dequeue pointer/cycle.
// Caller selects a 32-byte context index; GPAs are validated through DCBAA.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=412
void UsbXhci::write_endpoint_context(unsigned slot, unsigned ep, unsigned state) {
    auto out = read_guest_value(dcbaa + slot * 8, 8);
    validate_guest_range(out, EndpointContextCount * ContextBytes);
    auto at = out + ep * ContextBytes;
    write_guest_value(at, (read_guest_value(at, 4) & ~uint64_t(ContextStateMask)) | state, 4);
    if (ep)
        write_guest_value(at + EndpointField::TransferDequeue,
                          slots[slot].endpoints[ep].pointer | slots[slot].endpoints[ep].cycle, 8);
}
// Process a bounded batch of slot/address/configure/evaluate/endpoint commands, emitting completion
// codes. Copy selected contexts only; Configure Endpoint preserves the established
// slot/root-port/EP0 state. https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=448
void UsbXhci::process_command_ring() {
    std::array<uint32_t, 4> t;
    uint64_t a;
    for (unsigned n = 0; n < RingWalkBudget && next_transfer_ring_block(commandRing, t, a); ++n) {
        ++commands;
        unsigned type = (t[TrbWord::Control] >> TrbTypeShift) & TrbTypeMask,
                 id = t[TrbWord::Control] >> SlotIdShift, cc = Completion::Success;
        auto input = (uint64_t(t[TrbWord::ParameterHigh]) << 32 | t[TrbWord::ParameterLow]) &
                     ~uint64_t(TrbAddressMask);
        if (type == TrbType::EnableSlot) {
            id = 1;
            while (id < slots.size() && slots[id].enabled)
                ++id;
            if (id == slots.size() || id > config) {
                cc = Completion::NoSlotsAvailable;
                id = 0;
            } else {
                slots[id] = {};
                slots[id].enabled = true;
            }
        } else if (type == TrbType::NoOpCommand) { // No-op
        } else if (!id || id >= slots.size() || !slots[id].enabled)
            cc = Completion::SlotNotEnabled;
        else {
            auto &s = slots[id];
            if (type == TrbType::DisableSlot)
                s = {};
            else if (type ==
                     TrbType::EvaluateContext) { // Evaluate Context updates selected fields only.
                validate_guest_range(input, (EndpointContextCount + 1) * ContextBytes);
                auto out = read_guest_value(dcbaa + id * 8, 8);
                validate_guest_range(out, EndpointContextCount * ContextBytes);
                auto add = read_guest_value(input + InputControlField::AddFlags, 4);
                if (read_guest_value(input + InputControlField::DropFlags, 4) ||
                    (add & ~uint64_t(AddSlotAndDefaultEndpoint)))
                    cc = Completion::TrbError;
                else {
                    if (add & 1) {
                        write_guest_value(
                            out + SlotField::MaxExitLatencyRootPort,
                            (read_guest_value(out + SlotField::MaxExitLatencyRootPort, 4) &
                             ~uint64_t(SlotMaxExitLatencyMask)) |
                                (read_guest_value(
                                     input + ContextBytes + SlotField::MaxExitLatencyRootPort, 4) &
                                 SlotMaxExitLatencyMask),
                            4);
                        write_guest_value(
                            out + SlotField::InterrupterTarget,
                            (read_guest_value(out + SlotField::InterrupterTarget, 4) &
                             ~uint64_t(SlotInterrupterTargetMask)) |
                                (read_guest_value(
                                     input + ContextBytes + SlotField::InterrupterTarget, 4) &
                                 SlotInterrupterTargetMask),
                            4);
                    }
                    if (add & 2)
                        write_guest_value(
                            out + ContextBytes + EndpointField::MaxPacketSize,
                            (read_guest_value(out + ContextBytes + EndpointField::MaxPacketSize,
                                              4) &
                             ~uint64_t(EndpointMaxPacketSizeMask)) |
                                (read_guest_value(
                                     input + 2 * ContextBytes + EndpointField::MaxPacketSize, 4) &
                                 EndpointMaxPacketSizeMask),
                            4);
                }
            } else if (type == TrbType::AddressDevice || type == TrbType::ConfigureEndpoint) {
                validate_guest_range(input, (EndpointContextCount + 1) * ContextBytes);
                auto out = read_guest_value(dcbaa + id * 8, 8);
                validate_guest_range(out, EndpointContextCount * ContextBytes);
                uint32_t drop = read_guest_value(input + InputControlField::DropFlags, 4),
                         add = read_guest_value(input + InputControlField::AddFlags, 4);
                if (type == TrbType::AddressDevice)
                    add = AddSlotAndDefaultEndpoint;
                if (type == TrbType::AddressDevice) {
                    s.port = (read_guest_value(
                                  input + ContextBytes + SlotField::MaxExitLatencyRootPort, 4) >>
                              16) &
                             255;
                    if (s.port < KeyboardPort || s.port > RootPortCount)
                        cc = Completion::ParameterError;
                }
                if (cc == Completion::Success) {
                    for (unsigned ep = type == TrbType::ConfigureEndpoint ? 2 : SlotContextIndex;
                         ep < EndpointContextCount; ++ep) {
                        if (drop & (1u << ep)) {
                            s.active[ep] = false;
                            write_endpoint_context(id, ep, EndpointState::Disabled);
                        }
                        if (add & (1u << ep)) {
                            for (unsigned j = 0; j < ContextBytes; j += 4)
                                write_guest_value(
                                    out + ep * ContextBytes + j,
                                    read_guest_value(input + (ep + 1) * ContextBytes + j, 4), 4);
                            if (ep) {
                                auto dq = read_guest_value(input + (ep + 1) * ContextBytes +
                                                               EndpointField::TransferDequeue,
                                                           8);
                                s.endpoints[ep] = {dq & ~uint64_t(TrbAddressMask),
                                                   bool(dq & Cycle)};
                                s.active[ep] = false;
                                write_endpoint_context(id, ep, EndpointState::Running);
                            }
                        }
                    }
                    if (type == TrbType::AddressDevice)
                        write_guest_value(out + SlotField::AddressState,
                                          (t[TrbWord::Control] & BlockSetAddress)
                                              ? (SlotState::Default << SlotStateShift)
                                              : (id & 255) |
                                                    (SlotState::Addressed << SlotStateShift),
                                          4);
                    if (type == TrbType::ConfigureEndpoint) {
                        // Configure Endpoint consumes Context Entries, not a
                        // replacement slot address/root port or EP0 context.
                        write_guest_value(
                            out,
                            (read_guest_value(out, 4) & ~ContextEntriesMask) |
                                (read_guest_value(input + ContextBytes + SlotField::ContextEntries,
                                                  4) &
                                 ContextEntriesMask),
                            4);
                        auto state = read_guest_value(out + SlotField::AddressState, 4);
                        write_guest_value(out + SlotField::AddressState,
                                          (state & ~ContextEntriesMask) |
                                              (SlotState::Configured << SlotStateShift),
                                          4);
                    }
                }
            } else if (type == TrbType::ResetEndpoint || type == TrbType::StopEndpoint ||
                       type == TrbType::SetTransferDequeue) {
                unsigned ep = (t[TrbWord::Control] >> EndpointIdShift) & EndpointIdMask;
                if (!ep)
                    cc = Completion::EndpointNotEnabled;
                else {
                    s.active[ep] = false;
                    if (type == TrbType::SetTransferDequeue)
                        s.endpoints[ep] = {input, bool(t[TrbWord::ParameterLow] & Cycle)};
                    write_endpoint_context(id, ep,
                                           type == TrbType::StopEndpoint ? EndpointState::Stopped
                                                                         : EndpointState::Running);
                }
            } else if (type == TrbType::ResetDevice) {
                for (unsigned ep = 2; ep < EndpointContextCount; ++ep) {
                    s.active[ep] = false;
                    write_endpoint_context(id, ep, EndpointState::Disabled);
                }
            } else
                cc = Completion::TrbError;
        }
        enqueue_event(a, cc << CompletionCodeShift,
                      (TrbType::CommandCompletionEvent << TrbTypeShift) | (id << SlotIdShift));
    }
}
// Decode a fixed HID device's setup packet and construct/truncate its control-IN response.
// Unsupported requests/descriptors throw so the endpoint receives Stall Error.
// USB chapter 9 wire definitions:
// https://github.com/torvalds/linux/blob/master/include/uapi/linux/usb/ch9.h#L78
// https://www.usb.org/sites/default/files/documents/hid1_11.pdf#page=61
void UsbXhci::execute_setup_request(Slot &s, uint64_t p) {
    unsigned request = (p >> 8) & 255, value = (p >> 16) & 65535, length = p >> 48;
    const auto &report = s.port == KeyboardPort ? keyboardDescriptor
                         : s.port == MousePort  ? mouseDescriptor
                                                : tabletDescriptor;
    s.control.clear();
    s.controlIn = p & DeviceToHost;
    if (request == UsbRequest::GetDescriptor) {
        switch (value >> 8) {
        case DescriptorType::Device:
            // USB device descriptor: class/subclass/protocol live in the interface;
            // EP0 uses 8 bytes, VID is compatibility identity, PID is the fixed port.
            s.control = {DeviceDescriptorBytes,
                         DescriptorType::Device,
                         uint8_t(UsbDeviceVersion),
                         uint8_t(UsbDeviceVersion >> 8),
                         0,
                         0,
                         0,
                         DefaultControlPacketBytes,
                         uint8_t(UsbVendorIdentity),
                         uint8_t(UsbVendorIdentity >> 8),
                         uint8_t(s.port),
                         0,
                         uint8_t(DeviceReleaseVersion),
                         uint8_t(DeviceReleaseVersion >> 8),
                         0,
                         0,
                         0,
                         1};
            break;
        case DescriptorType::Configuration: {
            s.control = {
                // One configuration, one interface, no string, bus-powered+remote-wakeup, 100 mA.
                ConfigurationDescriptorBytes, DescriptorType::Configuration,
                ConfigurationTotalBytes, 0, 1, 1, 0, ConfigurationAttributes, MaximumPower2mAUnits,
                // Interface zero / alternate zero / one endpoint, HID class. Only
                // keyboard and relative mouse advertise boot-subclass protocols.
                InterfaceDescriptorBytes, DescriptorType::Interface, 0, 0, 1, HidClass,
                uint8_t(s.port == TabletPort ? 0 : BootSubclass),
                uint8_t(s.port == KeyboardPort ? KeyboardInterfaceProtocol
                        : s.port == MousePort  ? MouseInterfaceProtocol
                                               : NoBootProtocol),
                0,
                // HID 1.11, no country, one Report descriptor of the indicated length.
                HidDescriptorBytes, DescriptorType::Hid, uint8_t(HidVersion),
                uint8_t(HidVersion >> 8), 0, 1, DescriptorType::Report, uint8_t(report.size()), 0,
                // Endpoint 1 IN, interrupt, maximum eight bytes, ten millisecond interval.
                EndpointDescriptorBytes, DescriptorType::Endpoint, InterruptInEndpointAddress,
                InterruptEndpointType, InterruptPacketBytes, 0, FullSpeedPollingIntervalMs};
            break;
        }
        case DescriptorType::Hid:
            s.control = {HidDescriptorBytes,
                         DescriptorType::Hid,
                         uint8_t(HidVersion),
                         uint8_t(HidVersion >> 8),
                         0,
                         1,
                         DescriptorType::Report,
                         uint8_t(report.size()),
                         0};
            break;
        case DescriptorType::Report:
            s.control = report;
            break;
        default:
            throw std::runtime_error("Unsupported USB descriptor");
        }
    } else if (request == UsbRequest::SetConfiguration && !(p & RequestTypeMask))
        s.configuration = value;
    else if (request == HidRequest::SetReport && (p & RequestTypeMask) == ClassRequest) {
    } // HID SET_REPORT LEDs
    else if (request == UsbRequest::GetConfiguration)
        s.control = {uint8_t(s.configuration)};
    else if (request == UsbRequest::GetStatus)
        s.control = {0, 0};
    else if (request == HidRequest::SetIdle && (p & RequestTypeMask) == ClassRequest)
        s.idle = value >> 8;
    else if (request == HidRequest::SetProtocol && (p & RequestTypeMask) == ClassRequest)
        s.protocol = value;
    else if (request == HidRequest::GetProtocol && (p & RequestTypeMask) == ClassRequest)
        s.control = {uint8_t(s.protocol)};
    else if (request == HidRequest::GetIdle && (p & RequestTypeMask) == ClassRequest)
        s.control = {uint8_t(s.idle)};
    else if (request == HidRequest::GetReport && (p & RequestTypeMask) == ClassRequest)
        s.control = std::vector<uint8_t>(s.port == KeyboardPort ? KeyboardReportBytes
                                         : s.port == MousePort  ? MouseReportBytes
                                                                : TabletReportBytes);
    else if (request == UsbRequest::ClearFeature || request == UsbRequest::SetFeature ||
             request == UsbRequest::SetAddress || request == UsbRequest::SetInterface ||
             request == UsbRequest::GetInterface) {
    } else
        throw std::runtime_error("Unsupported USB request");
    if (s.control.size() > length)
        s.control.resize(length);
}
// Service bounded control/interrupt-IN TRBs and emit transfer events with residual/short-packet
// status. Empty interrupt queues behave as NAK: restore the saved dequeue and wait for host input.
// Event Data carries the preceding transfer descriptor's actual byte count; IDT OUT data is inline.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=427
void UsbXhci::process_transfer_ring(unsigned id, unsigned ep) {
    auto &s = slots[id];
    auto &r = s.endpoints[ep];
    std::array<uint32_t, 4> t;
    uint64_t a;
    for (unsigned n = 0; n < RingWalkBudget; ++n) {
        auto saved = r;
        if (!next_transfer_ring_block(r, t, a)) {
            s.active[ep] = false;
            return;
        }
        unsigned type = (t[TrbWord::Control] >> TrbTypeShift) & TrbTypeMask,
                 length = t[TrbWord::Status] & TransferLengthMask, actual = 0,
                 cc = Completion::Success;
        uint64_t ptr = uint64_t(t[TrbWord::ParameterHigh]) << 32 | t[TrbWord::ParameterLow];
        if (type == TrbType::Normal && ep == InterruptInContextIndex &&
            pending[s.port - 1].empty()) {
            r = saved;
            return;
        } // USB NAK

        if (type == TrbType::SetupStage && ep == ControlEndpointContextIndex) {
            try {
                execute_setup_request(s, ptr);
            } catch (const std::runtime_error &) {
                cc = Completion::StallError;
            }
        } else if ((type == TrbType::DataStage || type == TrbType::Normal) &&
                   ep == ControlEndpointContextIndex) {
            if (type == TrbType::DataStage ? bool(t[TrbWord::Control] & DataStageIn)
                                           : s.controlIn) {
                actual = std::min<size_t>(length, s.control.size());
                if (actual) {
                    validate_guest_range(ptr, actual);
                    std::copy_n(s.control.begin(), actual, ram.begin() + (ptr - ramBase));
                }
                s.control.erase(s.control.begin(), s.control.begin() + actual);
                if (actual < length)
                    cc = Completion::ShortPacket;
            } else if (t[TrbWord::Control] & ImmediateData) {
                // OUT data may live inline in the TRB, including a zero LED
                // byte. IDT is payload, never a guest physical DMA pointer.
                if (length > sizeof(uint64_t))
                    cc = Completion::TrbError;
                else
                    actual = length;
            } else {
                if (length)
                    validate_guest_range(ptr, length);
                actual = length;
            }
        } else if (type == TrbType::Normal && ep == InterruptInContextIndex) {
            auto &q = pending[s.port - 1];
            actual =
                std::min<size_t>(length, s.port == MousePort && !s.protocol ? BootMouseReportBytes
                                                                            : q.front().size());
            if (actual) {
                validate_guest_range(ptr, actual);
                std::copy_n(q.front().begin(), actual, ram.begin() + (ptr - ramBase));
            }
            q.pop_front();
            ++reports;
            if (actual < length)
                cc = Completion::ShortPacket;
        } else if (type != TrbType::StatusStage && type != TrbType::NoOpTransfer &&
                   type != TrbType::EventData)
            cc = Completion::TrbError;
        ++transfers;
        // Event Data completes the preceding TD, including its short-packet
        // status. Windows uses this completion to size descriptor responses.
        if (type == TrbType::SetupStage || type == TrbType::StatusStage) {
            s.actual[ep] = 0;
            s.shortPacket[ep] = false;
        }
        if (cc == Completion::ShortPacket)
            s.shortPacket[ep] = true;
        if (type == TrbType::EventData && s.shortPacket[ep])
            cc = Completion::ShortPacket;
        s.actual[ep] += actual;
        if ((t[TrbWord::Control] & InterruptOnCompletion) ||
            (cc == Completion::ShortPacket && (t[TrbWord::Control] & InterruptOnShortPacket)) ||
            (cc != Completion::Success && cc != Completion::ShortPacket)) {
            enqueue_event(type == TrbType::EventData ? ptr : a,
                          (cc << CompletionCodeShift) |
                              (type == TrbType::EventData ? s.actual[ep] : length - actual),
                          (TrbType::TransferEvent << TrbTypeShift) | (ep << EndpointIdShift) |
                              (id << SlotIdShift) |
                              (type == TrbType::EventData ? EventDataFlag : 0));
            s.actual[ep] = 0;
        }
        if (!(t[TrbWord::Control] & Chain))
            s.shortPacket[ep] = false;
        write_endpoint_context(
            id, ep, cc == Completion::StallError ? EndpointState::Halted : EndpointState::Running);
        if (cc != Completion::Success && cc != Completion::ShortPacket) {
            s.active[ep] = false;
            return;
        }
    }
}
// Start command work for slot zero or an enabled slot's supported control/interrupt-IN endpoint.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=394
void UsbXhci::ring_doorbell(unsigned id, unsigned ep) {
    if (!id) {
        process_command_ring();
        return;
    }
    if (id >= slots.size() || !slots[id].enabled || !slots[id].port ||
        (ep != ControlEndpointContextIndex && ep != InterruptInContextIndex))
        return;
    slots[id].active[ep] = true;
    process_transfer_ring(id, ep);
}
// Queue a complete host HID report, evict oldest at the host cap, and resume waiting interrupt-IN
// transfers. IMPLEMENTATION NOTE: Trusted frontend input and synchronous service, no physical USB
// polling clock. https://www.usb.org/sites/default/files/documents/hid1_11.pdf#page=20
void UsbXhci::queue_input_report(unsigned port, std::vector<uint8_t> report) {
    auto &q = pending[port - 1];
    if (q.size() == MaximumPendingReports)
        q.pop_front();
    q.push_back(std::move(report));
    if ((pciCommand & BusMasterEnable) && (usbcmd & RunStop)) {
        try {
            for (unsigned id = 1; id < slots.size(); ++id)
                if (slots[id].port == port && slots[id].active[InterruptInContextIndex])
                    process_transfer_ring(id, InterruptInContextIndex);
        } catch (const std::runtime_error &e) {
            failure = e.what();
            status |= HostSystemError | Halted;
            usbcmd &= ~RunStop;
            update_intx();
        }
    }
}
// Update a HID usage and queue modifiers/reserved/six-key report, using ErrorRollOver for more than
// six. IMPLEMENTATION NOTE: Duplicate state transitions are suppressed; descriptor-range gap is
// above. https://www.usb.org/sites/default/files/documents/hid1_11.pdf#page=72
void UsbXhci::receive_key_usage(uint16_t k, bool released) {
    ++keyInputs;
    if (k >= KeyboardUsageCount || keys[k] == !released)
        return;
    keys[k] = !released;
    std::vector<uint8_t> report(KeyboardReportBytes);
    unsigned count = 0;
    for (unsigned i = FirstModifierUsage; i <= LastModifierUsage; ++i)
        if (keys[i])
            report[0] |= 1 << (i - FirstModifierUsage);
    for (unsigned i = FirstNormalKeyUsage; i < FirstModifierUsage; ++i)
        if (keys[i]) {
            if (count < MaximumKeyboardKeys)
                report[KeyboardKeyArrayOffset + count] = i;
            ++count;
        }
    if (count > MaximumKeyboardKeys)
        std::fill(report.begin() + KeyboardKeyArrayOffset, report.end(), ErrorRollOver);
    queue_input_report(KeyboardPort, std::move(report));
}
// Split host counts into signed -127..127 X/Y/wheel reports, retaining all three button bits.
// https://www.usb.org/sites/default/files/documents/hid1_11.pdf#page=71
void UsbXhci::receive_relative_motion(int dx, int dy, int wheel, uint8_t b) {
    ++pointerInputs;
    buttons = b & MouseButtonMask;
    do {
        int x = std::clamp(dx, -MaximumRelativeDelta, MaximumRelativeDelta),
            y = std::clamp(dy, -MaximumRelativeDelta, MaximumRelativeDelta),
            z = std::clamp(wheel, -MaximumRelativeDelta, MaximumRelativeDelta);
        queue_input_report(MousePort, {buttons, uint8_t(x), uint8_t(y), uint8_t(z)});
        dx -= x;
        dy -= y;
        wheel -= z;
    } while (dx || dy || wheel);
}
// Scale host 0..65535 X/Y into 0..32767 and split wheel movement across tablet reports.
// IMPLEMENTATION NOTE: Normalized host coordinates differ from descriptor logical coordinate units.
// https://www.usb.org/sites/default/files/documents/hid1_11.pdf#page=45
void UsbXhci::receive_absolute_motion(int x, int y, int wheel, uint8_t b) {
    ++pointerInputs;
    tabletX = unsigned(std::clamp(x, 0, int(HostAbsoluteMaximum))) * TabletAbsoluteMaximum /
              HostAbsoluteMaximum;
    tabletY = unsigned(std::clamp(y, 0, int(HostAbsoluteMaximum))) * TabletAbsoluteMaximum /
              HostAbsoluteMaximum;
    do {
        int z = std::clamp(wheel, -MaximumRelativeDelta, MaximumRelativeDelta);
        queue_input_report(TabletPort,
                           {uint8_t(b & MouseButtonMask), uint8_t(tabletX), uint8_t(tabletX >> 8),
                            uint8_t(tabletY), uint8_t(tabletY >> 8), uint8_t(z)});
        wheel -= z;
    } while (wheel);
}
// Queue released key/mouse/tablet buttons while retaining the last absolute tablet coordinates.
// IMPLEMENTATION NOTE: Host focus-release lifecycle; wheel/motion bytes are zero.
// https://www.usb.org/sites/default/files/documents/hid1_11.pdf#page=20
void UsbXhci::release_all_input() {
    keys = {};
    buttons = 0;
    queue_input_report(KeyboardPort, std::vector<uint8_t>(KeyboardReportBytes));
    queue_input_report(MousePort, std::vector<uint8_t>(MouseReportBytes));
    queue_input_report(TabletPort, {0, uint8_t(tabletX), uint8_t(tabletX >> 8), uint8_t(tabletY),
                                    uint8_t(tabletY >> 8), 0});
}
} // namespace pc

namespace pc {
// Return bounded host input counts and enabled-slot metadata, excluding raw reports.
// https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=407
std::string UsbXhci::input_state() const {
    std::string result =
        " keys=" + std::to_string(keyInputs) + " pointers=" + std::to_string(pointerInputs);
    for (unsigned id = 1; id < slots.size(); ++id) {
        const auto &slot = slots[id];
        if (slot.enabled)
            result += " slot" + std::to_string(id) + "=port" + std::to_string(slot.port) +
                      ",config" + std::to_string(slot.configuration) + ",waiting" +
                      std::to_string(slot.active[InterruptInContextIndex]);
    }
    return result;
}
} // namespace pc
