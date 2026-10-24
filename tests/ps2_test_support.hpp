#pragma once
#include "machine.hpp"
namespace pc {
class Ps2TestAccess {
  public:
    static Ps2Controller::State &state(Ps2Controller &controller) { return controller.state_; }
};
inline Ps2Controller::State &ps2State(Machine &machine) { return Ps2TestAccess::state(machine.ps2); }
} // namespace pc
