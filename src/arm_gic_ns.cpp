// The Generic Interrupt Controller (GIC) Distributor routes shared peripheral
// interrupts (SPIs, IDs 32..1019) to CPUs and holds their enable, pending, active,
// priority, trigger-mode and affinity state. Private interrupts (SGIs/PPIs, IDs
// 0..31) belong to Redistributors when affinity routing is enabled. The native
// Hypervisor.framework GIC performs delivery and acknowledgement; this adapter
// translates guest Distributor accesses rather than emulating another controller.
//
// The backend exposes one Security state with affinity routing enabled. The guest
// sees a Non-secure view of a two-state GIC: SecurityExtn is reported, secure group
// assignment is hidden, Group 0 stays disabled, and Group 1's affinity-aware
// enable aliases the native Group 1 bit. The machine must assign every exposed
// interrupt to native Group 1 before using this view. Guest physical base and
// hidden native base are board mappings, not architectural fixed addresses.
//
// IMPLEMENTATION NOTE: Only the listed ordinary SPI register ranges pass through;
// unsupported/reserved accesses read zero and ignore writes. Optional features in
// native TYPER are passed through, so this is a bounded firmware compatibility
// view, not complete virtualization of every advertised GIC extension. Backend
// errors propagate to the host caller. Width/alignment/range errors throw before
// any native access. The host controls security assignment; no secure guest exists.
// Arm GIC architecture register descriptions (Arm text mirrored by Lund University):
// GICD_CTLR Security-state views, GICD_TYPER and per-interrupt registers.
// https://df.lth.se/~getz/ARM/SysReg/ext-gicd_ctlr.html
// https://df.lth.se/~getz/ARM/SysReg/ext-gicd_typer.html

#include "arm_gic_ns.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>
namespace pc {
namespace {
// GIC Distributor register offsets; array endpoints are exclusive range bounds.
enum Register : uint32_t {
    ControlRegister = 0x0000, // GICD_CTLR: group enables and affinity routing.
    TypeRegister = 0x0004,    // GICD_TYPER: capabilities and supported interrupt count.
    ImplementerIdentificationRegister = 0x0008, // GICD_IIDR: backend identity.
    InterruptSetEnableRegisters = 0x0100,       // GICD_ISENABLER: write-one-to-set enables.
    // The six 0x80-byte bitmap banks from 0x100 through 0x3ff are:
    // ISENABLER 0x100 (set enable), ICENABLER 0x180 (clear enable),
    // ISPENDR 0x200 (set pending), ICPENDR 0x280 (clear pending),
    // ISACTIVER 0x300 (set active), ICACTIVER 0x380 (clear active).
    // Each has one bit per interrupt; writing zero leaves the bit unchanged.
    // https://df.lth.se/~getz/ARM/SysReg/ext-gicd_isenablern.html
    InterruptPriorityRegisters = 0x0400,      // GICD_IPRIORITYR: byte-sized interrupt priorities.
    InterruptTargetRegisters = 0x0800,        // GICD_ITARGETSR: legacy CPU targeting (hidden here).
    InterruptConfigurationRegisters = 0x0c00, // GICD_ICFGR: two-bit edge/level fields.
    InterruptGroupModifierRegisters = 0x0d00, // GICD_IGRPMODR: secure group state (hidden).
    InterruptRoutingRegisters = 0x6000,       // GICD_IROUTER: 64-bit per-SPI affinity routes.
    InterruptRoutingRegistersEnd = 0x8000,    // End of the ordinary IROUTER address range.
    IdentificationRegisters = 0xffd0,         // GICD_PIDR/CIDR component/peripheral identity.
};
// Distributor frame occupies 64 KiB in the GIC memory map.
constexpr uint64_t DistributorFrameBytes = 0x10000;
// GICD_TYPER.SecurityExtn bit 10 reports support for two Security states.
// https://df.lth.se/~getz/ARM/SysReg/ext-gicd_typer.html
constexpr uint32_t SecurityExtension = 1u << 10;
// GICD_TYPER.ITLinesNumber[4:0] encodes interrupt count in groups of 32, minus 1.
constexpr uint32_t InterruptLinesNumberMask = 0x1f;
// Ordinary SPIs start at 32; IDs 1020..1023 are reserved special CPU-interface results.
constexpr unsigned FirstSharedPeripheralInterrupt = 32;
constexpr unsigned OrdinaryInterruptLimit = 1020;
// Bitmap registers contain 32 interrupt bits. Each set/clear bank spans 0x80
// bytes; offset within a bank times eight yields the first interrupt ID.
// https://df.lth.se/~getz/ARM/SysReg/ext-gicd_isenablern.html
constexpr unsigned InterruptsPerBitmap = 32, BitmapBankOffsetMask = 0x7f;
// GICD_ICFGR stores sixteen two-bit trigger-mode fields per 32-bit register.
// https://df.lth.se/~getz/ARM/SysReg/ext-gicd_icfgrn.html
constexpr unsigned InterruptsPerConfigurationRegister = 16, TriggerFieldBits = 2,
                   ConfigurationInterruptsPerByte = 4;
// GICD_CTLR in single-state view: DS[6] disables Security, ARE[4] enables affinity.
// Both must already be enabled; this adapter never changes backend routing mode.
// https://df.lth.se/~getz/ARM/SysReg/ext-gicd_ctlr.html
constexpr uint64_t DisableSecurity = 1ull << 6, NativeAffinityRouting = 1ull << 4;
// Native CTLR Group0/Group1 enable bits. Keep Group0 disabled; Group1 maps to
// the guest's Non-secure EnableGrp1A bit when affinity routing is enabled.
constexpr uint64_t GroupEnableMask = 3, Group1Enable = 2;
// Non-secure CTLR exposes RWP[31], ARE_NS[4] and EnableGrp1A[1]. Other bits
// are hidden; the native ARE bit already occupies the guest alias's bit position.
constexpr uint32_t NonsecureControlReadMask = 0x80000012;
// Backend reads/writes use aligned 32-bit priority words and 64-bit route words.
// Byte/half-route accesses select little-endian lanes within these native values.
constexpr unsigned PriorityWordBytes = 4, RouteRegisterBytes = 8, BitsPerByte = 8;
constexpr uint64_t PriorityWordLaneMask = PriorityWordBytes - 1;
constexpr uint64_t RouteRegisterLaneMask = RouteRegisterBytes - 1;
} // namespace
// Bind and validate the native single-state, affinity-routed backend; cache its SPI limit.
// Arm GIC Distributor TYPER register description.
// https://df.lth.se/~getz/ARM/SysReg/ext-gicd_typer.html
ArmGicNonsecureView::ArmGicNonsecureView(Read read, Write write)
    : load(std::move(read)), store(std::move(write)), typer(0), interrupts(0) {
    if (!load || !store)
        throw std::invalid_argument("GIC view requires a native backend");
    typer = uint32_t(load(Register::TypeRegister));
    if ((typer & SecurityExtension) ||
        (load(Register::ControlRegister) & (DisableSecurity | NativeAffinityRouting)) !=
            (DisableSecurity | NativeAffinityRouting))
        throw std::runtime_error(
            "GIC view requires a single-security-state affinity-routed backend");
    interrupts = std::min(OrdinaryInterruptLimit,
                          ((typer & InterruptLinesNumberMask) + 1) * InterruptsPerBitmap);
}
// Reject unsupported widths, misalignment or accesses outside the Distributor frame.
// IMPLEMENTATION NOTE: Host access validation around GICD registers.
// https://df.lth.se/~getz/ARM/SysReg/ext-gicd_ctlr.html
void ArmGicNonsecureView::validate_access(uint64_t offset, unsigned width) {
    if ((width != 1 && width != 2 && width != 4 && width != 8) || offset >= DistributorFrameBytes ||
        width > DistributorFrameBytes - offset || offset % width)
        throw std::invalid_argument("Invalid GIC distributor access");
}
// Select implemented SPI bits in a bitmap word, excluding private and special IDs.
// Arm GIC Distributor ISENABLERN register description.
// https://df.lth.se/~getz/ARM/SysReg/ext-gicd_isenablern.html
uint32_t ArmGicNonsecureView::shared_interrupt_bitmap_mask(unsigned first) const {
    if (first < FirstSharedPeripheralInterrupt || first >= interrupts)
        return 0;
    auto count = std::min(InterruptsPerBitmap, interrupts - first);
    return count == InterruptsPerBitmap ? UINT32_MAX : (1u << count) - 1;
}
// Read a filtered Non-secure Distributor register; hidden/unsupported fields read zero.
// Arm GIC Distributor CTLR register description.
// https://df.lth.se/~getz/ARM/SysReg/ext-gicd_ctlr.html
uint64_t ArmGicNonsecureView::read_register(uint64_t offset, unsigned width) const {
    validate_access(offset, width);
    if (width == 4) {
        if (offset == Register::ControlRegister)
            return load(Register::ControlRegister) &
                   NonsecureControlReadMask; // RWP, ARE_NS, EnableGrp1A.
        if (offset == Register::TypeRegister)
            return typer | SecurityExtension;
        if (offset == Register::ImplementerIdentificationRegister ||
            (offset >= Register::IdentificationRegisters && offset < DistributorFrameBytes))
            return uint32_t(load(uint32_t(offset)));
        if (offset >= Register::InterruptSetEnableRegisters &&
            offset < Register::InterruptPriorityRegisters) {
            auto mask =
                shared_interrupt_bitmap_mask(unsigned(offset & BitmapBankOffsetMask) * BitsPerByte);
            return mask ? uint32_t(load(uint32_t(offset))) & mask : 0;
        }
        if (offset >= Register::InterruptConfigurationRegisters &&
            offset < Register::InterruptGroupModifierRegisters) {
            unsigned first = unsigned(offset - Register::InterruptConfigurationRegisters) *
                             ConfigurationInterruptsPerByte;
            if (first < FirstSharedPeripheralInterrupt || first >= interrupts)
                return 0;
            unsigned bits =
                std::min(InterruptsPerConfigurationRegister, interrupts - first) * TriggerFieldBits;
            auto mask = bits == InterruptsPerBitmap ? UINT32_MAX : (1u << bits) - 1;
            return uint32_t(load(uint32_t(offset))) & mask;
        }
    }
    if ((width == 1 || width == 4) && offset >= Register::InterruptPriorityRegisters &&
        offset < Register::InterruptTargetRegisters) {
        auto irq = unsigned(offset - Register::InterruptPriorityRegisters);
        if (irq < FirstSharedPeripheralInterrupt || irq + width > interrupts)
            return 0;
        auto value = load(uint32_t(offset & ~PriorityWordLaneMask));
        return (value >> ((offset & PriorityWordLaneMask) * BitsPerByte)) &
               (width == 1 ? UINT8_MAX : UINT32_MAX);
    }
    if ((width == 4 || width == 8) && offset >= Register::InterruptRoutingRegisters &&
        offset < Register::InterruptRoutingRegistersEnd) {
        auto irq = unsigned(offset - Register::InterruptRoutingRegisters) / RouteRegisterBytes;
        if (irq < FirstSharedPeripheralInterrupt || irq >= interrupts)
            return 0;
        auto value = load(uint32_t(offset & ~RouteRegisterLaneMask));
        return width == 8 ? value
                          : uint32_t(value >> ((offset & RouteRegisterLaneMask) * BitsPerByte));
    }
    // Secure group assignment, legacy affinity registers, unsupported optional
    // registers and reserved space read as zero and ignore writes in this Non-secure view.
    return 0;
}
// Forward supported SPI state writes while preserving host security and native affinity mode.
// Arm GIC Distributor CTLR register description.
// https://df.lth.se/~getz/ARM/SysReg/ext-gicd_ctlr.html
void ArmGicNonsecureView::write_register(uint64_t offset, uint64_t value, unsigned width) {
    validate_access(offset, width);
    if (width == 4) {
        if (offset == Register::ControlRegister) {
            // With affinity routing enabled the writable nonsecure alias is
            // bit 1. Preserve native DS/ARE and keep Group 0 disabled.
            store(Register::ControlRegister,
                  (load(Register::ControlRegister) & ~GroupEnableMask) | (value & Group1Enable));
            return;
        }
        if (offset >= Register::InterruptSetEnableRegisters &&
            offset < Register::InterruptPriorityRegisters) {
            auto mask =
                shared_interrupt_bitmap_mask(unsigned(offset & BitmapBankOffsetMask) * BitsPerByte);
            // Write-one-to-set/clear operations must not merge an old bitmap:
            // doing so would also set/clear unrelated pending, active or enabled IDs.
            if (mask)
                store(uint32_t(offset), uint32_t(value) & mask);
            return;
        }
        if (offset >= Register::InterruptConfigurationRegisters &&
            offset < Register::InterruptGroupModifierRegisters) {
            auto first = unsigned(offset - Register::InterruptConfigurationRegisters) *
                         ConfigurationInterruptsPerByte;
            if (first < FirstSharedPeripheralInterrupt || first >= interrupts)
                return;
            unsigned bits =
                std::min(InterruptsPerConfigurationRegister, interrupts - first) * TriggerFieldBits;
            auto mask = bits == InterruptsPerBitmap ? UINT32_MAX : (1u << bits) - 1;
            store(uint32_t(offset), uint32_t(value) & mask);
            return;
        }
    }
    if ((width == 1 || width == 4) && offset >= Register::InterruptPriorityRegisters &&
        offset < Register::InterruptTargetRegisters) {
        auto irq = unsigned(offset - Register::InterruptPriorityRegisters);
        if (irq < FirstSharedPeripheralInterrupt || irq + width > interrupts)
            return;
        auto at = uint32_t(offset & ~PriorityWordLaneMask);
        if (width == 1) {
            auto shift = unsigned(offset & PriorityWordLaneMask) * BitsPerByte;
            value = (load(at) & ~(uint64_t(UINT8_MAX) << shift)) | ((value & UINT8_MAX) << shift);
        }
        store(at, uint32_t(value));
        return;
    }
    if ((width == 4 || width == 8) && offset >= Register::InterruptRoutingRegisters &&
        offset < Register::InterruptRoutingRegistersEnd) {
        auto irq = unsigned(offset - Register::InterruptRoutingRegisters) / RouteRegisterBytes;
        if (irq < FirstSharedPeripheralInterrupt || irq >= interrupts)
            return;
        auto at = uint32_t(offset & ~RouteRegisterLaneMask);
        if (width == 4) {
            auto shift = unsigned(offset & RouteRegisterLaneMask) * BitsPerByte;
            value = (load(at) & ~(uint64_t(UINT32_MAX) << shift)) |
                    (uint64_t(uint32_t(value)) << shift);
        }
        store(at, value);
    }
}
} // namespace pc
