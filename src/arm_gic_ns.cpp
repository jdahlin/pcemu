#include "arm_gic_ns.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>
namespace pc {
ArmGicNonsecureView::ArmGicNonsecureView(Read read, Write write)
    : load(std::move(read)), store(std::move(write)), typer(0), interrupts(0) {
    if (!load || !store) throw std::invalid_argument("GIC view requires a native backend");
    typer = uint32_t(load(4));
    if ((typer & 0x400) || (load(0) & 0x50) != 0x50)
        throw std::runtime_error("GIC view requires a single-security-state affinity-routed backend");
    interrupts = std::min(1020u, ((typer & 31) + 1) * 32);
}
void ArmGicNonsecureView::validate(uint64_t offset, unsigned width) {
    if ((width != 1 && width != 2 && width != 4 && width != 8) ||
        offset >= 0x10000 || width > 0x10000 - offset || offset % width)
        throw std::invalid_argument("Invalid GIC distributor access");
}
uint32_t ArmGicNonsecureView::bitmapMask(unsigned first) const {
    if (first < 32 || first >= interrupts) return 0;
    auto count = std::min(32u, interrupts - first);
    return count == 32 ? UINT32_MAX : (1u << count) - 1;
}
uint64_t ArmGicNonsecureView::read(uint64_t offset, unsigned width) const {
    validate(offset, width);
    if (width == 4) {
        if (offset == 0) return load(0) & 0x80000012u; // RWP, ARE_NS, EnableGrp1A.
        if (offset == 4) return typer | 0x400u;
        if (offset == 8 || (offset >= 0xffd0 && offset < 0x10000))
            return uint32_t(load(uint32_t(offset)));
        if (offset >= 0x100 && offset < 0x400) {
            auto mask = bitmapMask(unsigned(offset & 0x7f) * 8);
            return mask ? uint32_t(load(uint32_t(offset))) & mask : 0;
        }
        if (offset >= 0xc00 && offset < 0xd00) {
            unsigned first = unsigned(offset - 0xc00) * 4;
            if (first < 32 || first >= interrupts) return 0;
            unsigned bits = std::min(16u, interrupts - first) * 2;
            auto mask = bits == 32 ? UINT32_MAX : (1u << bits) - 1;
            return uint32_t(load(uint32_t(offset))) & mask;
        }
    }
    if ((width == 1 || width == 4) && offset >= 0x400 && offset < 0x800) {
        auto irq = unsigned(offset - 0x400);
        if (irq < 32 || irq + width > interrupts) return 0;
        auto value = load(uint32_t(offset & ~3ull));
        return (value >> ((offset & 3) * 8)) & (width == 1 ? 0xffu : UINT32_MAX);
    }
    if ((width == 4 || width == 8) && offset >= 0x6000 && offset < 0x8000) {
        auto irq = unsigned(offset - 0x6000) / 8;
        if (irq < 32 || irq >= interrupts) return 0;
        auto value = load(uint32_t(offset & ~7ull));
        return width == 8 ? value : uint32_t(value >> ((offset & 7) * 8));
    }
    // Secure group assignment, legacy affinity registers, unsupported optional
    // registers and reserved space are RAZ/WI in this nonsecure view.
    return 0;
}
void ArmGicNonsecureView::write(uint64_t offset, uint64_t value, unsigned width) {
    validate(offset, width);
    if (width == 4) {
        if (offset == 0) {
            // With affinity routing enabled the writable nonsecure alias is
            // bit 1. Preserve native DS/ARE and keep Group 0 disabled.
            store(0, (load(0) & ~3ull) | (value & 2)); return;
        }
        if (offset >= 0x100 && offset < 0x400) {
            auto mask = bitmapMask(unsigned(offset & 0x7f) * 8);
            // W1S/W1C writes must not be merged with a read of the old bitmap.
            if (mask) store(uint32_t(offset), uint32_t(value) & mask);
            return;
        }
        if (offset >= 0xc00 && offset < 0xd00) {
            auto first = unsigned(offset - 0xc00) * 4;
            if (first < 32 || first >= interrupts) return;
            unsigned bits = std::min(16u, interrupts - first) * 2;
            auto mask = bits == 32 ? UINT32_MAX : (1u << bits) - 1;
            store(uint32_t(offset), uint32_t(value) & mask); return;
        }
    }
    if ((width == 1 || width == 4) && offset >= 0x400 && offset < 0x800) {
        auto irq = unsigned(offset - 0x400);
        if (irq < 32 || irq + width > interrupts) return;
        auto at = uint32_t(offset & ~3ull);
        if (width == 1) {
            auto shift = unsigned(offset & 3) * 8;
            value = (load(at) & ~(0xffull << shift)) | ((value & 0xff) << shift);
        }
        store(at, uint32_t(value)); return;
    }
    if ((width == 4 || width == 8) && offset >= 0x6000 && offset < 0x8000) {
        auto irq = unsigned(offset - 0x6000) / 8;
        if (irq < 32 || irq >= interrupts) return;
        auto at = uint32_t(offset & ~7ull);
        if (width == 4) {
            auto shift = unsigned(offset & 7) * 8;
            value = (load(at) & ~(uint64_t(UINT32_MAX) << shift)) | (uint64_t(uint32_t(value)) << shift);
        }
        store(at, value);
    }
}
}
