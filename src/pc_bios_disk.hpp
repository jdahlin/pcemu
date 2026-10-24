#pragma once
#include <cstdint>
namespace pc {
struct Disk;
struct CPU;
// Firmware transfer adapter; storage devices have no dependency on a guest CPU.
bool biosDiskTransfer(Disk &disk, CPU &cpu, bool write, uint64_t lba, unsigned count, uint32_t buffer);
} // namespace pc
