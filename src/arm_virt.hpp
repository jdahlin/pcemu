#pragma once
#include "arm_devices.hpp"
#include "arm_fw_cfg.hpp"
#include "hvf_machine.hpp"
#include "virtio_blk.hpp"
#include "virtio_pci.hpp"
#include <string>
namespace pc {
// Concrete ARM firmware platform. Keep x86 CPU/device internals out of this path.
class ArmVirtMachine {
  public:
    struct Config {
        std::string firmware, dtb, variableTemplate, variables;
        std::string disk, diskOverlay;
        std::string virtioTransport = "pci";
    };
    explicit ArmVirtMachine(const Config &config);
    ~ArmVirtMachine();
    ArmVirtMachine(const ArmVirtMachine &) = delete;
    ArmVirtMachine &operator=(const ArmVirtMachine &) = delete;
    void run(std::chrono::milliseconds slice);
    void input(const std::string &text) { uart.input(text); }
    const std::string &output() const { return uart.output(); }
    void saveVariables();
    std::string diskPath() const { return disk ? disk->path : std::string{}; }
    uint64_t diskRequests() const { return block ? block->requests : 0; }
    uint64_t exits() const { return vm.exits(); }
    std::string framebuffer() const;
    void saveFramebuffer(const std::string &path) const;
  private:
    static std::vector<uint8_t> load(const std::string &path, size_t maximum);
    Config config;
    HvfMachine vm;
    ArmNorFlash nor;
    ArmFwCfg fwCfg;
    ArmPl011 uart;
    std::unique_ptr<Disk> disk;
    std::unique_ptr<VirtioBlock> block;
    std::unique_ptr<VirtioPciBlock> pci;
    int variableLock = -1, diskLock = -1;
};
} // namespace pc
