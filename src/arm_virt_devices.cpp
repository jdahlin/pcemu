#include "arm_virt.hpp"
#include <stdexcept>
namespace pc {
void ArmVirtMachine::checkDeviceErrors() const {
    if (gpu && !gpu->error().empty()) throw std::runtime_error("Virtio GPU requires reset: " + gpu->error());
    if (balloon && !balloon->error().empty()) throw std::runtime_error("Virtio balloon requires reset: " + balloon->error());
    for (auto *device : {block.get(), extraBlock.get()})
        if (device && !device->error().empty())
            throw std::runtime_error("Virtio device requires reset: " + device->error());
    if (net && !net->error().empty()) throw std::runtime_error("Virtio network requires reset: " + net->error());
    if (rng && !rng->error().empty()) throw std::runtime_error("Virtio entropy requires reset: " + rng->error());
    if (serial && !serial->error().empty()) throw std::runtime_error("Virtio serial requires reset: " + serial->error());
}
void ArmVirtMachine::bindDevices() {
    mmio.clear();
    if(gicView) mmio.map("hyperv-gic",ArmGicNonsecureView::guestBase,0x10000,
        [this](MmioAccess &a,uint64_t offset) {a.value=gicView->read(offset,a.width);},
        [this](MmioAccess &a,uint64_t offset) {gicView->write(offset,a.value,a.width);});
    pciBus.clear();
    auto attach = [this](const char *name, unsigned slot, auto *device) {
        if (!device) return;
        pciBus.attach(name, slot,
            [device](uint64_t o, unsigned w) { return device->configRead(o, w); },
            [device](uint64_t o, uint64_t v, unsigned w) { device->configWrite(o, v, w); },
            [device](uint64_t a, unsigned w) { return device->contains(a, w); },
            [device](uint64_t a, unsigned w) { return device->read(a, w); },
            [device](uint64_t a, uint64_t v, unsigned w) { device->write(a, v, w); });
    };
    attach("virtio-block", 0, pci.get());
    attach("virtio-extra-block", 1, extraPci.get());
    attach("virtio-net", 2, netPci.get());
    attach("virtio-rng", 3, rngPci.get());
    attach("usb-xhci", 4, usb.get());
    attach("virtio-serial", 5, serialPci.get());
    attach("virtio-balloon", 6, balloonPci.get());
    attach("virtio-gpu", 7, gpuPci.get());
    // NVMe had precedence over virtio BARs in the original platform decoder.
    attach("nvme", 0, nvme.get());

    mmio.map("pl031", 0x09010000, 0x1000,
        [this](MmioAccess &a, uint64_t offset) {
            if (offset % a.width) throw std::runtime_error("Unaligned PL031 access");
            if (!a.discard) a.value = rtc.read_register(offset & ~3u) >> (8 * (offset & 3));
            updateRtcInterrupt();
        }, [this](MmioAccess &a, uint64_t offset) {
            if (offset % a.width) throw std::runtime_error("Unaligned PL031 access");
            if (offset % 4 || a.width != 4) throw std::runtime_error("PL031 writes require 32 bits");
            rtc.write_register(offset, uint32_t(a.value));
            updateRtcInterrupt();
        }, 1 | 2 | 4);
    mmio.map("fw-cfg", 0x09020000, 0x18,
        [this](MmioAccess &a, uint64_t o) { a.value = fwCfg.read_register(o, a.width); },
        [this](MmioAccess &a, uint64_t o) { fwCfg.write_register(o, a.value, a.width); });
    mmio.map("nor-flash", 0x04000000, 0xc0000, {},
        [this](MmioAccess &a, uint64_t o) {
            nor.write_command_or_data(o, uint32_t(a.value));
            auto [begin, end] = nor.take_dirty_range();
            if (end > begin) vm.writeMemory(0x04000000 + begin, nor.bytes().subspan(begin, end - begin));
        }, 4);
    mmio.map("pci-ecam", 0x3f000000, 0x100000,
        [this](MmioAccess &a, uint64_t o) {
            a.value = pciBus.read_configuration(o, a.width);
            checkDeviceErrors();
        }, [this](MmioAccess &a, uint64_t o) {
            pciBus.write_configuration(o, a.value, a.width);
            checkDeviceErrors();
        });
    mmio.mapDynamic("pci-bars",
        [this](const MmioAccess &a) { return pciBus.contains(a.address, a.width); },
        [this](MmioAccess &a, uint64_t) {
            if (a.write) pciBus.write_bar(a.address, a.value, a.width);
            else a.value = *pciBus.read_bar(a.address, a.width);
            checkDeviceErrors();
        });
    mmio.map("virtio-mmio", 0x0a000000, 0x200,
        [this](MmioAccess &a, uint64_t o) {
            auto *device = pci ? nullptr : block.get();
            a.value = device ? device->read(o, a.width) : (o == 0 ? 0x74726976u : o == 4 ? 2u : 0u);
            if (block && !block->error().empty())
                throw std::runtime_error("Virtio device requires reset: " + block->error());
        }, [this](MmioAccess &a, uint64_t o) {
            if (!pci && block) block->write(o, a.value, a.width);
            if (block && !block->error().empty())
                throw std::runtime_error("Virtio device requires reset: " + block->error());
        });
    mmio.map("pl011", 0x09000000, 0x1000,
        [this](MmioAccess &a, uint64_t o) {
            if (o % a.width) throw std::runtime_error("Unaligned PL011 access");
            a.value = uart.read_register(o);
            vm.setSPI(33, uart.uartintr_asserted());
        }, [this](MmioAccess &a, uint64_t o) {
            if (o % a.width) throw std::runtime_error("Unaligned PL011 access");
            uart.write_register(o, uint32_t(a.value));
            vm.setSPI(33, uart.uartintr_asserted());
        }, 1 | 2 | 4);
}
bool ArmVirtMachine::display(std::vector<uint32_t> &pixels, unsigned &width, unsigned &height, bool compositeCursor) const {
    if (gpu && gpu->active()) {
        displayWasGpu = true;
        return gpu->changedPixels(pixels, width, height, compositeCursor);
    }
    if (displayWasGpu) {
        displayWasGpu = false;
        if (ramfb.read_pixels(pixels, width, height)) return true;
        width = height = 0;
        return false;
    }
    return ramfb.changed_pixels(pixels, width, height);
}
std::string ArmVirtMachine::usbReport() const {
    return usb ? "commands=" + std::to_string(usb->commands) +
        " transfers=" + std::to_string(usb->transfers) + " reports=" + std::to_string(usb->reports) + usb->inputState() + " error=" + usb->error() : "disabled";
}
} // namespace pc
