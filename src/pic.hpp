#pragma once
#include <cstdint>
namespace pc {
// 8259A fixed/rotating priority resolver, ICWs, OCW2/3, IRR/ISR and polling.
struct PIC8259 {
    uint8_t mask = 0xff, irr = 0, isr = 0, base = 8, init = 0, lowest = 7;
    bool single = false, needICW4 = false, autoEOI = false, readISR = false, pollRead = false,
         specialMask = false, rotateAuto = false;
    void raise(unsigned irq) {
        irr |= uint8_t(1u << (irq & 7));
    }
    int pending() const;
    void acknowledge(unsigned irq);
    void command(uint8_t value);
    void data(uint8_t value);
    uint8_t read();

  private:
    int rank(unsigned irq) const {
        return (irq - lowest - 1) & 7;
    }
    int highest(uint8_t bits) const;
};
} // namespace pc
