#include "pic.hpp"
namespace pc {
int PIC8259::highest(uint8_t bits) const {
    if (!bits)
        return -1;
    unsigned first = (lowest + 1) & 7;
    unsigned rotated = (unsigned(bits) | (unsigned(bits) << 8)) >> first;
    return (first + __builtin_ctz(rotated)) & 7;
}
int PIC8259::pending() const {
    int request = highest(irr & ~mask);
    if (request < 0)
        return -1;
    int active = highest(specialMask ? isr & ~mask : isr);
    return active < 0 || rank(request) < rank(active) ? request : -1;
}
void PIC8259::acknowledge(unsigned irq) {
    irr &= ~(1u << irq);
    if (!autoEOI)
        isr |= 1u << irq;
    else if (rotateAuto)
        lowest = irq;
}
void PIC8259::command(uint8_t v) {
    if (v & 0x10) {
        init = 2;
        needICW4 = v & 1;
        single = v & 2;
        irr = isr = mask = 0;
        lowest = 7;
        autoEOI = readISR = pollRead = specialMask = rotateAuto = false;
        return;
    }
    if (v & 8) {
        if (v & 2)
            readISR = v & 1;
        if (v & 4)
            pollRead = true;
        if (v & 0x40)
            specialMask = v & 0x20;
        return;
    }
    unsigned operation = v >> 5;
    if (operation == 0) {
        rotateAuto = false;
        return;
    }
    if (operation == 4) {
        rotateAuto = true;
        return;
    }
    if (operation == 6) {
        lowest = v & 7;
        return;
    }
    if (operation == 1 || operation == 3 || operation == 5 || operation == 7) {
        int irq = (operation & 2) ? int(v & 7) : highest(isr);
        if (irq >= 0) {
            isr &= ~(1u << irq);
            if (operation & 4)
                lowest = irq;
        }
    }
}
void PIC8259::data(uint8_t v) {
    if (init == 2) {
        base = v & 0xf8;
        init = single ? (needICW4 ? 4 : 0) : 3;
    } else if (init == 3)
        init = needICW4 ? 4 : 0;
    else if (init == 4) {
        autoEOI = v & 2;
        init = 0;
    } else
        mask = v;
}
uint8_t PIC8259::read() {
    if (pollRead) {
        pollRead = false;
        int irq = pending();
        if (irq < 0)
            return 0;
        acknowledge(irq);
        return 0x80 | irq;
    }
    return readISR ? isr : irr;
}
} // namespace pc
