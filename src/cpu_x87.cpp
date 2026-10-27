// x87 stack arithmetic and environment for the 386/387 guest. The interpreter
// executes ESC opcodes; native blocks fall back through the same implementation.
// Registers use binary64 host arithmetic, with explicit guest stack/tags, control
// word rounding for integer stores, and 80-bit memory conversion. This is not
// bit-exact 387 extended arithmetic; unsupported encodings raise #UD.
// Intel SDM volumes 1 (Programming with the x87 FPU) and 2 (instruction reference):
// https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf
// https://cdrdv2-public.intel.com/835757/325383-sdm-vol-2abcd.pdf
#include "cpu.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <algorithm>

namespace pc {
// Execute one decoded x87 instruction, preserving the logical stack mapping in
// FSW.TOP. Intel SDM vol. 1, chapter 8; vol. 2, FLD/FST/FADD/FIST/FSAVE entries.
// https://cdrdv2-public.intel.com/835757/325383-sdm-vol-2abcd.pdf
void CPU::stepX87(uint8_t op, bool a32, int over, bool o32) {
    if (cr[0] & 12) throw Fault{7, 0, false};
    uint8_t m = fetch(1);
    auto operand = decode(m, a32, over);
    unsigned g = (m >> 3) & 7, i = m & 7;
    unsigned top = (fpStatus >> 11) & 7;
    auto slot = [&](unsigned n) { return (top + n) & 7; };
    auto value = [&](unsigned n) { return std::bit_cast<double>(fp[slot(n)]); };
    auto tag = [&](unsigned n, unsigned t) {
        auto shift = slot(n) * 2;
        fpTag = (fpTag & ~(3u << shift)) | (t << shift);
    };
    auto set = [&](unsigned n, double x) {
        fp[slot(n)] = std::bit_cast<uint64_t>(x);
        tag(n, x == 0 ? 1 : !std::isfinite(x) ? 2 : 0);
    };
    auto pop = [&] { tag(0, 3); top = (top + 1) & 7; };
    auto push = [&](double x) { top = (top - 1) & 7; set(0, x); };
    auto finish = [&] { fpStatus = (fpStatus & ~0x3800u) | (top << 11); };
    auto init = [&] {
        fpControl = 0x37f; fpStatus = 0; fpTag = 0xffff; top = 0;
        fpIP = fpData = 0; fpCS = fpDS = fpOpcode = 0;
    };
    auto compare = [&](double x, double y) {
        fpStatus &= ~0x4500u;
        if (std::isnan(x) || std::isnan(y)) fpStatus |= 0x4500;
        else if (x < y) fpStatus |= 0x100;
        else if (x == y) fpStatus |= 0x4000;
    };
    auto roundInteger = [&](double x) {
        switch ((fpControl >> 10) & 3) {
        case 1: return std::floor(x);
        case 2: return std::ceil(x);
        case 3: return std::trunc(x);
        default: { // Round to nearest, ties to even, independent of host fenv.
            double f = std::floor(x), d = x - f;
            return d < .5 ? f : d > .5 ? f + 1 : std::fmod(f, 2) == 0 ? f : f + 1;
        }
        }
    };
    auto read64 = [&](uint32_t at) { return uint64_t(read(at, 4)) | (uint64_t(read(at + 4, 4)) << 32); };
    auto write64 = [&](uint32_t at, uint64_t x) { write(at, uint32_t(x), 4); write(at + 4, uint32_t(x >> 32), 4); };
    auto extendedRead = [&](uint32_t at) {
        auto mantissa = read64(at); auto se = read(at + 8, 2);
        unsigned exponent = se & 0x7fff;
        double x = exponent == 0x7fff ? (mantissa & 0x7fffffffffffffffull ?
            std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity()) :
            std::ldexp(double(mantissa), int(exponent ? exponent : 1) - 16383 - 63);
        return se & 0x8000 ? -x : x;
    };
    auto extendedWrite = [&](uint32_t at, double x) {
        uint16_t se = std::signbit(x) ? 0x8000 : 0;
        uint64_t mantissa = 0;
        if (!std::isfinite(x)) {
            se |= 0x7fff; mantissa = std::isnan(x) ? 0xc000000000000000ull : 0x8000000000000000ull;
        } else if (x != 0) {
            int exponent; double fraction = std::frexp(std::fabs(x), &exponent);
            se |= uint16_t(exponent - 1 + 16383);
            mantissa = uint64_t(std::ldexp(fraction, 64));
        }
        write64(at, mantissa); write(at + 8, se, 2);
    };
    auto arithmetic = [&](unsigned operation, double x, double y) {
        switch (operation) {
        case 0: return x + y;
        case 1: return x * y;
        case 4: return x - y;
        case 5: return y - x;
        case 6: return x / y;
        case 7: return y / x;
        default: return x;
        }
    };
    if (!operand.reg) {
        auto at = operand.linear;
        // Validate the entire memory operand before changing the stack or doing
        // a split store, so ordinary page faults restart without partial results.
        unsigned size = 4;
        if (op == 0xdc || op == 0xdd) size = 8;
        if (op == 0xde || op == 0xdf) size = 2;
        if (op == 0xdb && (g == 5 || g == 7)) size = 10;
        if (op == 0xdf && (g == 5 || g == 7)) size = 8;
        if (op == 0xd9 && (g == 5 || g == 7)) size = 2;
        bool env = (op == 0xd9 && (g == 4 || g == 6)) || (op == 0xdd && (g == 4 || g == 6));
        if (env) size = (o32 ? 28 : 14) + (op == 0xdd ? 80 : 0);
        bool writing = g == 2 || g == 3 || g == 7 || (env && g == 6);
        for (unsigned n = 0; n < size; ++n) physical(at + n, writing);
        if (env) {
            unsigned stride = o32 ? 4 : 2, bytes = o32 ? 28 : 14;
            if (g == 4) {
                fpControl = read(at, 2); fpStatus = read(at + stride, 2); fpTag = read(at + stride * 2, 2);
                top = (fpStatus >> 11) & 7;
                fpIP = read(at + stride * 3, stride); fpCS = read(at + stride * 4, 2);
                fpData = read(at + stride * 5, stride); fpDS = read(at + stride * 6, 2);
                if (o32) fpOpcode = read(at + 18, 2);
                if (op == 0xdd) for (unsigned n = 0; n < 8; ++n) fp[slot(n)] = std::bit_cast<uint64_t>(extendedRead(at + bytes + n * 10));
            } else {
                finish();
                for (unsigned n = 0; n < bytes; ++n) write(at + n, 0, 1);
                write(at, fpControl, 2); write(at + stride, fpStatus, 2); write(at + stride * 2, fpTag, 2);
                write(at + stride * 3, fpIP, stride); write(at + stride * 4, fpCS, 2);
                write(at + stride * 5, fpData, stride); write(at + stride * 6, fpDS, 2);
                if (o32) write(at + 18, fpOpcode, 2);
                if (op == 0xdd) { for (unsigned n = 0; n < 8; ++n) extendedWrite(at + bytes + n * 10, value(n)); init(); }
                else fpControl |= 0x3f;
            }
        } else if (!(op & 1)) {
            double x = op == 0xd8 ? double(std::bit_cast<float>(read(at, 4))) :
                       op == 0xdc ? std::bit_cast<double>(read64(at)) :
                       op == 0xda ? double(int32_t(read(at, 4))) : double(int16_t(read(at, 2)));
            if (g == 2 || g == 3) { compare(value(0), x); if (g == 3) pop(); }
            else set(0, arithmetic(g, value(0), x));
        } else if (op == 0xd9 && g == 5) fpControl = read(at, 2);
        else if (op == 0xd9 && g == 7) write(at, fpControl, 2);
        else if (op == 0xdd && g == 7) { finish(); write(at, fpStatus, 2); }
        else if (g == 0 || (op == 0xdb && g == 5) || (op == 0xdf && g == 5)) {
            double x = op == 0xd9 ? double(std::bit_cast<float>(read(at, 4))) :
                op == 0xdd ? std::bit_cast<double>(read64(at)) :
                op == 0xdb ? (g == 5 ? extendedRead(at) : double(int32_t(read(at, 4)))) :
                g == 5 ? double(int64_t(read64(at))) : double(int16_t(read(at, 2)));
            push(x);
        } else if (g == 2 || g == 3 || g == 7) {
            double x = value(0);
            if (op == 0xd9) write(at, std::bit_cast<uint32_t>(float(x)), 4);
            else if (op == 0xdd) write64(at, std::bit_cast<uint64_t>(x));
            else if (op == 0xdb && g == 7) extendedWrite(at, x);
            else {
                x = roundInteger(x);
                unsigned bits = op == 0xdb ? 32 : g == 7 ? 64 : 16;
                double bound = std::ldexp(1., bits - 1);
                bool invalid = !std::isfinite(x) || x < -bound || x >= bound;
                uint64_t result = invalid ? uint64_t(1) << (bits - 1) : uint64_t(int64_t(x));
                if (invalid) fpStatus |= 1;
                if (bits == 64) write64(at, result); else write(at, uint32_t(result), bits / 8);
            }
            if (g != 2) pop();
        } else throw Fault{6, 0, false};
    } else if (op == 0xd8 || op == 0xdc || op == 0xde) {
        if (op == 0xde && m == 0xd9) { compare(value(0), value(1)); pop(); pop(); }
        else if (g == 2 || g == 3) { compare(value(0), value(i)); if (g == 3) pop(); }
        else if (op == 0xd8) set(0, arithmetic(g, value(0), value(i)));
        else { set(i, arithmetic(g >= 4 ? g ^ 1 : g, value(i), value(0))); if (op == 0xde) pop(); }
    } else if (op == 0xd9) {
        if (g == 0) { double x = value(i); push(x); }
        else if (g == 1) { double x = value(0), y = value(i); set(0, y); set(i, x); }
        else switch (m) {
        case 0xd0: break;
        case 0xe0: set(0, -value(0)); break;
        case 0xe1: set(0, std::fabs(value(0))); break;
        case 0xe4: compare(value(0), 0); break;
        case 0xe5: {
            double x = value(0); fpStatus &= ~0x4700u;
            if (std::signbit(x)) fpStatus |= 0x200;
            fpStatus |= ((fpTag >> (slot(0) * 2)) & 3) == 3 ? 0x4100 :
                std::isnan(x) ? 0x100 : std::isinf(x) ? 0x500 : x == 0 ? 0x4000 : 0x400;
            break;
        }
        case 0xe8: push(1); break;
        case 0xe9: push(std::log2(10.)); break;
        case 0xea: push(std::log2(std::exp(1.))); break;
        case 0xeb: push(std::acos(-1.)); break;
        case 0xec: push(std::log10(2.)); break;
        case 0xed: push(std::log(2.)); break;
        case 0xee: push(0); break;
        case 0xf0: set(0, std::exp2(value(0)) - 1); break;
        case 0xf1: set(1, value(1) * std::log2(value(0))); pop(); break;
        case 0xf2: set(0, std::tan(value(0))); push(1); fpStatus &= ~0x400u; break;
        case 0xf3: set(1, std::atan2(value(1), value(0))); pop(); break;
        case 0xf5: set(0, std::remainder(value(0), value(1))); fpStatus &= ~0x400u; break;
        case 0xf6: top = (top - 1) & 7; break;
        case 0xf7: top = (top + 1) & 7; break;
        case 0xf8: set(0, std::fmod(value(0), value(1))); fpStatus &= ~0x400u; break;
        case 0xf9: set(1, value(1) * std::log2(value(0) + 1)); pop(); break;
        case 0xfa: set(0, std::sqrt(value(0))); break;
        case 0xfb: { double x = value(0); set(0, std::sin(x)); push(std::cos(x)); fpStatus &= ~0x400u; break; }
        case 0xfc: set(0, roundInteger(value(0))); break;
        case 0xfd: set(0, value(0) * std::exp2(std::trunc(value(1)))); break;
        case 0xfe: set(0, std::sin(value(0))); fpStatus &= ~0x400u; break;
        case 0xff: set(0, std::cos(value(0))); fpStatus &= ~0x400u; break;
        default: throw Fault{6, 0, false};
        }
    } else if (op == 0xdd) {
        if (g == 0) tag(i, 3);
        else if (g == 2 || g == 3) { set(i, value(0)); if (g == 3) pop(); }
        else if (g == 4 || g == 5) { compare(value(0), value(i)); if (g == 5) pop(); }
        else throw Fault{6, 0, false};
    } else if (op == 0xdb && m == 0xe3) init();
    else if (op == 0xdb && m == 0xe2) fpStatus &= ~0x80ffu;
    else if (op == 0xdb && (m == 0xe0 || m == 0xe1 || m == 0xe4)) {} // 287 compatibility control.
    else if (op == 0xda && m == 0xe9) { compare(value(0), value(1)); pop(); pop(); }
    else if (op == 0xdf && m == 0xe0) { finish(); CPU::set(AX, fpStatus, 2); }
    else throw Fault{6, 0, false};
    finish();
}
} // namespace pc
