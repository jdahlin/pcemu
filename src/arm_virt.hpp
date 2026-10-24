#pragma once
#include "arm_devices.hpp"
#include "hvf_machine.hpp"
#include <string>
namespace pc {
// Concrete ARM firmware platform. Keep x86 CPU/device internals out of this path.
class ArmVirtMachine {
  public:
    struct Config {
        std::string firmware, dtb, variableTemplate, variables;
    };
    explicit ArmVirtMachine(const Config &config);
    ~ArmVirtMachine();
    ArmVirtMachine(const ArmVirtMachine &) = delete;
    ArmVirtMachine &operator=(const ArmVirtMachine &) = delete;
    void run(std::chrono::milliseconds slice);
    void input(const std::string &text) { uart.input(text); }
    const std::string &output() const { return uart.output(); }
    void saveVariables();
    uint64_t exits() const { return vm.exits(); }
  private:
    static std::vector<uint8_t> load(const std::string &path, size_t maximum);
    Config config;
    HvfMachine vm;
    ArmNorFlash nor;
    ArmPl011 uart;
    int variableLock = -1;
};
} // namespace pc
