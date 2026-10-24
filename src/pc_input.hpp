#pragma once
#include <cstdint>
namespace pc {
class I8042Device;
// Translate an automation text/key value to physical make/break scan bytes.
void enqueuePcKey(I8042Device &controller, uint16_t key);
} // namespace pc
