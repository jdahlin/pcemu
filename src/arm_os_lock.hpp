#pragma once
#include <cstdint>
namespace pc {
// Software OS-lock status/control for trapped EL1 debug-register accesses.
// Breakpoint/watchpoint execution remains owned by Hypervisor.framework.
class ArmOsLock {
  public:
    // Read/write a supported OS lock register from a trapped instruction syndrome.
    // False means unhandled; reads replace value and writes consume its control bit.
    bool access_system_register(uint64_t syndrome, uint64_t &value);

  private:
    bool locked = true, doubleLocked = false;
};
} // namespace pc
