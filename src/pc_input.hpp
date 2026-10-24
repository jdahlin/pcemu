#pragma once
#include <cstdint>
namespace pc {
class Ps2Controller;
// Translate an automation text/key value to physical make/break scan bytes.
void enqueuePcKey(Ps2Controller &controller, uint16_t key);
} // namespace pc
