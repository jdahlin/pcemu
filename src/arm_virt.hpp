#pragma once
#include "arm_pl031.hpp"
#include "arm_hyperv.hpp"
#include "arm_os_lock.hpp"
#include "arm_psci.hpp"
#include "arm_nor_flash.hpp"
#include "arm_pl011.hpp"
#include "arm_mmio.hpp"
#include "arm_gic_ns.hpp"
#include "arm_fw_cfg.hpp"
#include "arm_ramfb.hpp"
#include "mmio_space.hpp"
#include "pci_ecam.hpp"
#include "hvf_machine.hpp"
#include "virtio_blk.hpp"
#include "virtio_pci.hpp"
#include "virtio_net.hpp"
#include "virtio_rng.hpp"
#include "network.hpp"
#include "nvme.hpp"
#include <string>
namespace pc {
// Concrete ARM firmware platform. Keep x86 CPU/device internals out of this path.
class ArmVirtMachine {
  public:
    static constexpr size_t ramBytes = size_t(2) * 1024 * 1024 * 1024;
    struct Config {
        unsigned cpus = 1;
        size_t memoryBytes = ramBytes;
        std::string firmware, dtb, variableTemplate, variables;
        std::string disk, diskOverlay;
        std::string extraDisk, extraDiskOverlay;
        bool network = false;
        bool hyperv = false;
        bool hypervSynic = false;
        uint16_t sshPort = 0;
        std::string networkCapture;
        bool hypervVmbus = false;
        std::string virtioTransport = "pci";
        std::string diskController = "virtio";
        std::string acpiDirectory = "build/arm64-fw/acpi";
    };
    explicit ArmVirtMachine(const Config &config);
    ~ArmVirtMachine();
    ArmVirtMachine(const ArmVirtMachine &) = delete;
    ArmVirtMachine &operator=(const ArmVirtMachine &) = delete;
    void run(std::chrono::milliseconds slice);
    void input(const std::string &text) { uart.input(text); vm.setSPI(33, uart.irq()); }
    const std::string &output() const { return uart.output(); }
    void saveVariables();
    std::string diskPath() const { return disk ? disk->path : std::string{}; }
    uint64_t diskRequests() const { return nvme ? nvme->requests : block ? block->requests : 0; }
    uint64_t networkTransmitted() const { return net ? net->transmitted : 0; }
    uint64_t networkReceived() const { return net ? net->received : 0; }
    uint64_t extraDiskRequests() const { return extraBlock ? extraBlock->requests : 0; }
    uint64_t exits() const { return vm.exits(); }
    bool resetRequested() const { return powerAction == ArmPowerAction::Reset; }
    bool stopped() const { return powerAction != ArmPowerAction::None; }
    const char *stopReason() const { return powerAction == ArmPowerAction::Off ? "power-off" :
        powerAction == ArmPowerAction::Reset ? "reset-requested" : "running"; }
    std::string framebuffer() const { return ramfb.mode(); }
    void saveFramebuffer(const std::string &path) const { ramfb.save(path); }
    std::string cpuState() const;
    std::string hypervReport() const;
    // Trusted host queries, between run slices with all VPs stopped.
    bool enumerateGuestKvp(ArmHyperVKvpCodec::Pool pool,uint32_t index) { return hyperv.enumerateKvp(pool,index); }
    std::optional<ArmHyperVKvp::Result> takeGuestKvpResult() { return hyperv.takeKvpResult(); }
    // Call between run slices, with all VPs stopped. Never forces host poweroff.
    bool requestGuestShutdown(ArmHyperVShutdown::Action action = ArmHyperVShutdown::Action::PowerOff) {
        return hyperv.requestShutdown(action);
    }
  private:
    static std::vector<uint8_t> load(const std::string &path, size_t maximum);
    Config config;
    HvfMachine vm;
    ArmNorFlash nor;
    ArmFwCfg fwCfg;
    ArmRamFb ramfb;
    MmioSpace mmio;
    std::unique_ptr<ArmGicNonsecureView> gicView;
    PciEcamBus pciBus;
    void bindDevices();
    void checkDeviceErrors() const;
    ArmPl011 uart;
    ArmPl031 rtc;
    bool rtcInterruptLevel = false;
    void updateRtcInterrupt();
    std::vector<ArmOsLock> osLocks;
    ArmHyperV hyperv;
    ArmPowerAction powerAction = ArmPowerAction::None;
    std::unique_ptr<Disk> disk;
    std::unique_ptr<VirtioBlock> block;
    std::unique_ptr<VirtioPciBlock> pci;
    std::unique_ptr<Nvme> nvme;
    std::unique_ptr<Disk> extraDisk;
    std::unique_ptr<VirtioBlock> extraBlock;
    std::unique_ptr<VirtioPciDevice> extraPci, netPci, rngPci;
    std::unique_ptr<VirtioRng> rng;
    std::unique_ptr<VirtioNet> net;
    std::unique_ptr<Network> gateway;
    int variableLock = -1, diskLock = -1, extraDiskLock = -1;
};
} // namespace pc
