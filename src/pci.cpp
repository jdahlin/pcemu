#include "pci.hpp"
namespace pc {
uint32_t PCI::read(uint16_t bdf, unsigned offset, unsigned width) const {
    return bdf == 0x10 ? display_.read(offset, width) : 0xffffffffu;
}
void PCI::write(uint16_t bdf, unsigned offset, uint32_t value, unsigned width) {
    if (bdf == 0x10) display_.write(offset, value, width);
}
uint32_t PCI::in(uint16_t port, unsigned width) const {
    if (port == 0xcf8)
        return address;
    if (!(address & 0x80000000u) || port < 0xcfc || unsigned(port - 0xcfc) + width > 4)
        return 0xffffffffu;
    return read((address >> 8) & 0xffff, (address & 0xfc) + (port - 0xcfc), width);
}
void PCI::out(uint16_t port, uint32_t value, unsigned width) {
    if (port == 0xcf8 && width == 4) {
        address = value & 0x80fffffc;
        return;
    }
    if ((address & 0x80000000u) && port >= 0xcfc && unsigned(port - 0xcfc) + width <= 4)
        write((address >> 8) & 0xffff, (address & 0xfc) + (port - 0xcfc), value, width);
}
} // namespace pc
