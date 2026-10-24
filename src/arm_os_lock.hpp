#pragma once
#include <cstdint>
namespace pc {
// Software OS-lock status/control for trapped EL1 debug-register accesses.
// Breakpoint/watchpoint execution remains owned by Hypervisor.framework.
class ArmOsLock {
  public:
    bool access(uint64_t syndrome, uint64_t &value);
  private:
    bool locked = true, doubleLocked = false;
};
} // namespace pc
