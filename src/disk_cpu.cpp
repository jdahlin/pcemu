#include "disk.hpp"
#include "cpu.hpp"
namespace pc {
bool Disk::transfer(CPU &cpu, bool wr, uint64_t lba, unsigned count, uint32_t buffer) {
    if (!count || lba >= sectors || count > sectors - lba)
        return false;
    std::vector<uint8_t> data(size_t(count) * 512);
    if (wr) {
        for (size_t n = 0; n < data.size(); n++)
            data[n] = cpu.read(buffer + n, 1);
        return write(lba, count, data.data());
    }
    if (!read(lba, count, data.data()))
        return false;
    for (size_t n = 0; n < data.size(); n++)
        cpu.write(buffer + n, data[n], 1);
    return true;
}
} // namespace pc
