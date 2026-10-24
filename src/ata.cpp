#include "ata.hpp"
#include "disk.hpp"
#include <algorithm>
#include <cstring>
namespace pc {
void ATA::fail(uint8_t code) {
    error = code;
    status = 0x51;
    remaining = 0;
    irq = true;
}
bool ATA::load() {
    if (!disk || lba >= disk->sectors) {
        fail(0x10);
        return false;
    }
    if (!disk->read(lba, 1, buffer.data())) {
        fail(0x40);
        return false;
    }
    position = 0;
    status = 0x58;
    irq = true;
    return true;
}
void ATA::advance() {
    lba++;
    count = --remaining;
    if (device & 0x40) {
        sector = lba;
        cylLow = lba >> 8;
        cylHigh = lba >> 16;
        device = (device & 0xf0) | ((lba >> 24) & 15);
    } else {
        unsigned track = lba / spt;
        sector = lba % spt + 1;
        device = (device & 0xf0) | (track % heads);
        unsigned cylinder = track / heads;
        cylLow = cylinder;
        cylHigh = cylinder >> 8;
    }
    position = 0;
}
void ATA::identifyData() {
    buffer.fill(0);
    auto word = [&](unsigned i, uint16_t v) {
        buffer[i * 2] = v;
        buffer[i * 2 + 1] = v >> 8;
    };
    auto text = [&](unsigned i, unsigned n, const char *s) {
        auto len = std::strlen(s);
        for (unsigned j = 0; j < n; j++)
            buffer[i * 2 + (j ^ 1)] = j < len ? s[j] : ' ';
    };
    unsigned cylinders = std::min<uint64_t>(16383, disk->sectors / (heads * spt));
    word(0, 0x40);
    word(1, cylinders);
    word(3, heads);
    word(6, spt);
    text(10, 20, "PCEMU0000000000000001");
    text(23, 8, "0.1");
    text(27, 40, "PCEMU ORIGINAL ATA PIO DISK");
    word(47, 0x8000);
    word(49, 0x0200);
    word(53, 1);
    word(54, cylinders);
    word(55, heads);
    word(56, spt);
    auto chs = uint32_t(cylinders * heads * spt);
    word(57, chs);
    word(58, chs >> 16);
    word(60, disk->sectors);
    word(61, disk->sectors >> 16);
    word(80, 0x10);
    position = 0;
    remaining = 1;
    identify = true;
    writing = false;
    status = 0x58;
    irq = true;
}
void ATA::command(uint8_t cmd) {
    irq = false;
    error = 0;
    position = 0;
    remaining = 0;
    identify = false;
    writing = false;
    if (!disk || (device & 0x10)) {
        status = 0;
        return;
    }
    if (cmd == 0xec) {
        identifyData();
        return;
    }
    if (cmd == 0x20 || cmd == 0x21 || cmd == 0x30 || cmd == 0x31 || cmd == 0x40 || cmd == 0x41) {
        remaining = count ? count : 256;
        if (device & 0x40)
            lba = uint64_t(device & 15) << 24 | uint32_t(cylHigh) << 16 | uint32_t(cylLow) << 8 |
                  sector;
        else {
            if (!sector || sector > spt || (device & 15) >= heads) {
                fail(0x10);
                return;
            }
            lba = (uint64_t(uint16_t(cylLow) | (cylHigh << 8)) * heads + (device & 15)) * spt +
                  sector - 1;
        }
        if (lba >= disk->sectors || remaining > disk->sectors - lba) {
            fail(0x10);
            return;
        }
        if (cmd == 0x40 || cmd == 0x41) {
            while (remaining)
                advance();
            status = 0x50;
            irq = true;
            return;
        }
        writing = cmd == 0x30 || cmd == 0x31;
        if (writing) {
            status = 0x58;
            buffer.fill(0);
        } else
            load();
        return;
    }
    if (cmd == 0x91) {
        unsigned heads = (device & 15) + 1;
        if (count) {
            this->heads = heads;
            spt = count;
        }
        status = 0x50;
        irq = true;
        return;
    }
    if (cmd == 0x90) {
        error = 1;
        status = 0x50;
        irq = true;
        return;
    }
    if (cmd == 0xe7 || cmd == 0xef || cmd == 0xe0 || cmd == 0xe1 || cmd == 0xe3 || cmd == 0x10) {
        status = 0x50;
        irq = true;
        return;
    }
    if (cmd == 0xe5) {
        count = 255;
        status = 0x50;
        irq = true;
        return;
    }
    fail();
}
uint32_t ATA::in(uint16_t p, int w) {
    if (!disk || (device & 0x10))
        return 0;
    if (p == 0x1f0) {
        if (!(status & 8) || writing)
            return w == 1 ? 255 : w == 2 ? 65535 : 0xffffffffu;
        uint32_t value = 0;
        for (int i = 0; i < w; i++) {
            value |= uint32_t(buffer[position++]) << (i * 8);
            if (position == 512) {
                if (identify) {
                    remaining = 0;
                    status = 0x50;
                    position = 0;
                } else {
                    advance();
                    if (remaining)
                        load();
                    else
                        status = 0x50;
                }
            }
        }
        return value;
    }
    switch (p) {
    case 0x1f1:
        return error;
    case 0x1f2:
        return count;
    case 0x1f3:
        return sector;
    case 0x1f4:
        return cylLow;
    case 0x1f5:
        return cylHigh;
    case 0x1f6:
        return device;
    case 0x1f7:
        irq = false;
        return status;
    case 0x3f6:
        return status;
    default:
        return 0xff;
    }
}
void ATA::out(uint16_t p, uint32_t value, int w) {
    if (p == 0x3f6) {
        bool reset = (control & 4) && !(value & 4);
        control = value;
        if (value & 4) {
            status = 0x80;
            irq = false;
        }
        if (reset) {
            error = 1;
            count = sector = 1;
            cylLow = cylHigh = 0;
            device = 0xa0;
            status = 0x50;
            remaining = 0;
            position = 0;
        }
        return;
    }
    if (p == 0x1f6) {
        device = value;
        return;
    }
    if (p == 0x1f0) {
        if (!(status & 8) || !writing)
            return;
        for (int i = 0; i < w; i++) {
            buffer[position++] = value >> (i * 8);
            if (position == 512) {
                if (!disk->write(lba, 1, buffer.data())) {
                    fail(0x40);
                    return;
                }
                advance();
                status = remaining ? 0x58 : 0x50;
                irq = true;
            }
        }
        return;
    }
    switch (p) {
    case 0x1f1:
        features = value;
        break;
    case 0x1f2:
        count = value;
        break;
    case 0x1f3:
        sector = value;
        break;
    case 0x1f4:
        cylLow = value;
        break;
    case 0x1f5:
        cylHigh = value;
        break;
    case 0x1f7:
        command(value);
        break;
    default:
        break;
    }
}
} // namespace pc
