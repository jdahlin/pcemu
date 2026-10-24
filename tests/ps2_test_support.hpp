#pragma once
#include "machine.hpp"
namespace pc {
class Ps2TestAccess {
  public:
    static I8042Device::State &state(I8042Device &controller) { return controller.state_; }
};
inline I8042Device::State &ps2State(Machine &machine) { return Ps2TestAccess::state(machine.ps2); }
} // namespace pc
