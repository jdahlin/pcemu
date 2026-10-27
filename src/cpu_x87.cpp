// Intel 386/387-compatible floating-point execution subset. DOS software uses
// D8..DF ESC instructions to load an eight-register stack, calculate on ST(0),
// inspect comparison/status bits, and save the environment across task switches.
// The Floating-point Status Word (FSW) TOP field maps logical stack positions
// to physical registers. The Tag Word (FTW) classifies each physical register;
// the Control Word (FCW) selects rounding and exception masks. Memory operands go through CPU segmentation and paging;
// #NM (device-not-available) requests OS emulation/context handling when
// CR0.EM/TS is set. The machine owns device/interrupt wiring.
// Instructions execute synchronously here; there is no concurrent coprocessor.
//
// References: Intel 64 and IA-32 Architectures Software Developer's Manual:
// - Vol. 1, Basic Architecture, 253665-081US, September 2023, chapter 8.
//   https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf#page=203
// - Vol. 2, Instruction Set Reference A-Z, 325383-085US, October 2024.
//   https://cdrdv2-public.intel.com/835757/325383-sdm-vol-2abcd.pdf#page=506
// IMPLEMENTATION NOTE: fp[] stores binary64 bit patterns, not guest 80-bit
// register images. Explicit conversion supplies the 10-byte memory format.
// FIXME: implement 387 extended arithmetic, FCW precision/rounding for real
// results, full exception mask/delivery rules, empty-stack faults and pointer
// tracking. Host libm also does not reproduce 387 transcendental precision,
// range reduction or FPREM quotient condition codes. Reserved encodings are
// not all validated. These limits must not be mistaken for full conformance.
// Vol. 1, 8.1.2, 8.1.5, 8.5 and 8.6:
// https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf#page=203
#include "cpu.hpp"
#include "profile.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <algorithm>

namespace pc {
namespace x87 {
// FSW.TOP (bits 13:11) selects ST(0), C0/C2/C3 (bits 8/10/14) hold compare
// results. C1 (bit 9) also reports FXAM's sign. IE (bit 0) records an invalid
// conversion; FCW.RC (bits 11:10) selects integer rounding below. Vol. 1, 8.1.3:
// https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf#page=206
constexpr uint16_t TopMask = 0x3800, ComparisonMask = 0x4500;
constexpr uint16_t Less = 0x100, Equal = 0x4000, Sign = 0x200, ClassMask = 0x4700;
constexpr uint16_t InvalidOperation = 1;
// FINIT's 037Fh masks six exceptions, selects nearest-even rounding and requests
// a 64-bit significand (the precision request is stored but not honored here).
// FNCLEX clears exception flags, stack fault, error summary and busy, retaining
// TOP and comparison codes. Vol. 2 FINIT/FNINIT and FCLEX/FNCLEX entries:
// https://cdrdv2-public.intel.com/835757/325383-sdm-vol-2abcd.pdf#page=506
constexpr uint16_t InitialControl = 0x37f, ExceptionMasks = 0x3f;
// Clear bits 7:0 and 15, retaining condition codes and TOP; FCLEX/FNCLEX:
// https://cdrdv2-public.intel.com/835757/325383-sdm-vol-2abcd.pdf#page=481
constexpr uint16_t ClearExceptions = 0x80ff;
// FTW uses two bits per physical register, independent of TOP. Vol. 1, 8.1.7:
// https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf#page=210
constexpr unsigned ValidTag = 0, ZeroTag = 1, SpecialTag = 2, EmptyTag = 3;
constexpr uint16_t EmptyTags = 0xffff;
// Double-extended memory has a 64-bit explicit significand and 15-bit biased
// exponent plus sign; infinity has exponent 7FFFh and only the integer bit set.
// Vol. 1, 4.2.2 Floating-Point Data Types, Tables 4-2/4-3:
// https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf#page=92
constexpr int ExtendedBias = 16383;
constexpr unsigned ExtendedExponent = 0x7fff, ExtendedSign = 0x8000;
constexpr uint64_t IntegerBit = 0x8000000000000000ull, FractionMask = 0x7fffffffffffffffull;
constexpr uint64_t QuietNaNSignificand = 0xc000000000000000ull;
}
// Execute an ESC instruction after its primary opcode has been fetched. EM/TS
// cause #NM before ModR/M/operand accesses; successful operations update TOP,
// tags and memory. Translated execution supplies an already decoded operand. Vol. 2, FLD
// exceptions and FINIT/FNINIT; Vol. 1, 8.1 register-stack organization:
// https://cdrdv2-public.intel.com/835757/325383-sdm-vol-2abcd.pdf#page=513
void CPU::execute_x87_instruction(uint8_t op, bool a32, int over, bool o32) {
    using namespace x87;
    // CR0.EM (bit 2) requests software emulation; TS (bit 3) requests lazy
    // context handling. MP only changes FWAIT's behavior, not these ESC checks.
    if (cr[0] & 12) throw Fault{7, 0, false};
    uint8_t m = fetch(1);
    profile::x87(op, m);
    auto operand = decode(m, a32, over);
    execute_x87_decoded(op,m,operand,o32);
}
// Arithmetic/environment semantics shared after decoding. No CPU::step, byte
// fetch or ModR/M decoding occurs here; JIT callers supply the dynamic operand.
void CPU::execute_x87_decoded(uint8_t op,uint8_t m,Operand operand,bool o32) {
    using namespace x87;
    unsigned g = (m >> 3) & 7, i = m & 7;
    unsigned top = (fpStatus >> 11) & 7;
    // Logical ST(i) wraps modulo eight from TOP; tags remain physical.
    // Vol. 1, 8.1.2/8.1.7:
    // https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf#page=203
    auto physical_slot = [&](unsigned n) { return (top + n) & 7; };
    // IMPLEMENTATION NOTE: decode our binary64 storage without type-punning.
    auto stack_value = [&](unsigned n) { return std::bit_cast<double>(fp[physical_slot(n)]); };
    // Replace one physical two-bit tag; this does not move TOP.
    // Vol. 1, 8.1.7:
    // https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf#page=210
    auto set_tag = [&](unsigned n, unsigned t) {
        auto shift = physical_slot(n) * 2;
        fpTag = (fpTag & ~(3u << shift)) | (t << shift);
    };
    // Write a logical stack element and classify it for FTW. FIXME: denormals
    // and unsupported 80-bit values need faithful 387 classification (8.1.7).
    // https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf#page=210
    auto set_stack_value = [&](unsigned n, double x) {
        fp[physical_slot(n)] = std::bit_cast<uint64_t>(x);
        set_tag(n, x == 0 ? ZeroTag : !std::isfinite(x) ? SpecialTag : ValidTag);
    };
    // Pop marks the departing physical register empty before advancing TOP.
    // Push decrements TOP before writing. Vol. 1, 8.1.2:
    // https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf#page=203
    auto pop_stack = [&] { set_tag(0, EmptyTag); top = (top + 1) & 7; };
    auto push_stack = [&](double x) { top = (top - 1) & 7; set_stack_value(0, x); };
    // Publish the local TOP without disturbing sticky exceptions or C flags.
    // Vol. 1, 8.1.3.1:
    // https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf#page=206
    auto publish_top = [&] { fpStatus = (fpStatus & ~TopMask) | (top << 11); };
    // Reset control/status/tag state after FNINIT or a completed FNSAVE.
    // COMPATIBILITY: use later-x87 pointer clearing; Intel387 FNINIT retained
    // its instruction/data pointers. Vol. 2, FINIT IA-32 compatibility note:
    // https://cdrdv2-public.intel.com/835757/325383-sdm-vol-2abcd.pdf#page=506
    auto initialize = [&] {
        fpControl = InitialControl; fpStatus = 0; fpTag = EmptyTags; top = 0;
        fpIP = fpData = 0; fpCS = fpDS = fpOpcode = 0;
    };
    // Comparisons encode unordered=111, less=001, equal=100, greater=000 in
    // C3/C2/C0. FIXME: ordered-vs-unordered NaN exception behavior is missing.
    // Vol. 1, Table 8-1:
    // https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf#page=207
    auto compare = [&](double x, double y) {
        fpStatus &= ~ComparisonMask;
        if (std::isnan(x) || std::isnan(y)) fpStatus |= ComparisonMask;
        else if (x < y) fpStatus |= Less;
        else if (x == y) fpStatus |= Equal;
    };
    // Decode RC: nearest-even, down, up, truncate. Compute ties explicitly so
    // a host floating-point rounding setting cannot change FIST/FRNDINT policy.
    // Vol. 1, 8.1.5.3; Vol. 2 FIST/FISTP:
    // https://cdrdv2-public.intel.com/835757/325383-sdm-vol-2abcd.pdf#page=508
    auto round_integer = [&](double x) {
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
    // IMPLEMENTATION NOTE: CPU bus accessors accept at most four bytes. Split
    // little-endian qwords only after the complete operand is preflighted below.
    auto read_qword = [&](uint32_t at) { return uint64_t(read(at, 4)) | (uint64_t(read(at + 4, 4)) << 32); };
    auto write_qword = [&](uint32_t at, uint64_t x) { write(at, uint32_t(x), 4); write(at + 4, uint32_t(x >> 32), 4); };
    // Decode m80real's explicit integer bit, sign and exponent into binary64.
    // FIXME: this loses extended range/precision and canonicalizes NaN payloads.
    // Vol. 1, 4.2.2:
    // https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf#page=92
    auto read_extended = [&](uint32_t at) {
        auto mantissa = read_qword(at); auto se = read(at + 8, 2);
        unsigned exponent = se & ExtendedExponent;
        double x = exponent == ExtendedExponent ? (mantissa & FractionMask ?
            std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity()) :
            std::ldexp(double(mantissa), int(exponent ? exponent : 1) - ExtendedBias - 63);
        return se & ExtendedSign ? -x : x;
    };
    // Encode our binary64 value as m80real without host long-double layout
    // assumptions. Vol. 1, 4.2.2 (host values are the limited subset above):
    // https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf#page=92
    auto write_extended = [&](uint32_t at, double x) {
        uint16_t se = std::signbit(x) ? ExtendedSign : 0;
        uint64_t mantissa = 0;
        if (!std::isfinite(x)) {
            se |= ExtendedExponent; mantissa = std::isnan(x) ? QuietNaNSignificand : IntegerBit;
        } else if (x != 0) {
            int exponent; double fraction = std::frexp(std::fabs(x), &exponent);
            se |= uint16_t(exponent - 1 + ExtendedBias);
            mantissa = uint64_t(std::ldexp(fraction, 64));
        }
        write_qword(at, mantissa); write(at + 8, se, 2);
    };
    // ModR/M.reg selects add/multiply/subtract/reverse/divide/reverse. The
    // caller supplies operand order; DC/DE register forms invert SUB/DIV sense.
    // Vol. 2, FDIV/FDIVP/FIDIV and FSUB/FSUBP/FISUB opcode tables:
    // https://cdrdv2-public.intel.com/835757/325383-sdm-vol-2abcd.pdf#page=494
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
        // IMPLEMENTATION NOTE: preflight every byte with the CPU page walker
        // before stack changes or split writes. It may set paging A/D bits, but
        // an ordinary missing-page fault cannot leave half a stored m64/m80.
        // This is a restart policy, not the 387 bus transaction sequence.
        // FIXME: segment-limit checks must eventually cover the entire operand.
        // Vol. 2 FST/FSTP protected-mode exceptions:
        // https://cdrdv2-public.intel.com/835757/325383-sdm-vol-2abcd.pdf#page=547
        unsigned size = 4;
        if (op == 0xdc || op == 0xdd) size = 8;
        if (op == 0xde || op == 0xdf) size = 2;
        if (op == 0xdb && (g == 5 || g == 7)) size = 10;
        if (op == 0xdf && (g == 5 || g == 7)) size = 8;
        if ((op == 0xd9 && (g == 5 || g == 7)) || (op == 0xdd && g == 7)) size = 2;
        bool env = (op == 0xd9 && (g == 4 || g == 6)) || (op == 0xdd && (g == 4 || g == 6));
        if (env) size = (o32 ? 28 : 14) + (op == 0xdd ? 80 : 0);
        bool writing = (op & 1) && (g == 2 || g == 3 || g == 7 || (env && g == 6));
        for (unsigned n = 0; n < size; ++n) physical(at + n, writing);
        if (env) {
            // FSAVE adds eight 10-byte values in logical ST(0)..ST(7) order;
            // FTW remains physical. Operand size selects 14/28-byte environment
            // and therefore 94/108-byte full state. FNSTENV masks exceptions
            // only after storing the old FCW; FNSAVE resets only after writing.
            // Vol. 1, 8.1.10, Figures 8-9/8-11; Vol. 2 FSAVE/FNSAVE:
            // https://cdrdv2-public.intel.com/789574/253665-sdm-vol-1.pdf#page=213
            // FIXME: pointer fields currently use the protected-mode layout
            // even in real mode; reserved bits/pointer tracking need fidelity.
            unsigned stride = o32 ? 4 : 2, bytes = o32 ? 28 : 14;
            if (g == 4) {
                fpControl = read(at, 2); fpStatus = read(at + stride, 2); fpTag = read(at + stride * 2, 2);
                top = (fpStatus >> 11) & 7;
                fpIP = read(at + stride * 3, stride); fpCS = read(at + stride * 4, 2);
                fpData = read(at + stride * 5, stride); fpDS = read(at + stride * 6, 2);
                if (o32) fpOpcode = read(at + 18, 2);
                if (op == 0xdd) for (unsigned n = 0; n < 8; ++n) fp[physical_slot(n)] = std::bit_cast<uint64_t>(read_extended(at + bytes + n * 10));
            } else {
                publish_top();
                for (unsigned n = 0; n < bytes; ++n) write(at + n, 0, 1);
                write(at, fpControl, 2); write(at + stride, fpStatus, 2); write(at + stride * 2, fpTag, 2);
                write(at + stride * 3, fpIP, stride); write(at + stride * 4, fpCS, 2);
                write(at + stride * 5, fpData, stride); write(at + stride * 6, fpDS, 2);
                if (o32) write(at + 18, fpOpcode, 2);
                if (op == 0xdd) { for (unsigned n = 0; n < 8; ++n) write_extended(at + bytes + n * 10, stack_value(n)); initialize(); }
                else fpControl |= ExceptionMasks;
            }
        } else if (!(op & 1)) {
            double x = op == 0xd8 ? double(std::bit_cast<float>(read(at, 4))) :
                       op == 0xdc ? std::bit_cast<double>(read_qword(at)) :
                       op == 0xda ? double(int32_t(read(at, 4))) : double(int16_t(read(at, 2)));
            if (g == 2 || g == 3) { compare(stack_value(0), x); if (g == 3) pop_stack(); }
            else set_stack_value(0, arithmetic(g, stack_value(0), x));
        } else if (op == 0xd9 && g == 5) fpControl = read(at, 2);
        else if (op == 0xd9 && g == 7) write(at, fpControl, 2);
        else if (op == 0xdd && g == 7) { publish_top(); write(at, fpStatus, 2); }
        else if (g == 0 || (op == 0xdb && g == 5) || (op == 0xdf && g == 5)) {
            double x = op == 0xd9 ? double(std::bit_cast<float>(read(at, 4))) :
                op == 0xdd ? std::bit_cast<double>(read_qword(at)) :
                op == 0xdb ? (g == 5 ? read_extended(at) : double(int32_t(read(at, 4)))) :
                g == 5 ? double(int64_t(read_qword(at))) : double(int16_t(read(at, 2)));
            push_stack(x);
        } else if (g == 2 || g == 3 || g == 7) {
            double x = stack_value(0);
            if (op == 0xd9) write(at, std::bit_cast<uint32_t>(float(x)), 4);
            else if (op == 0xdd) write_qword(at, std::bit_cast<uint64_t>(x));
            else if (op == 0xdb && g == 7) write_extended(at, x);
            else {
                x = round_integer(x);
                unsigned bits = op == 0xdb ? 32 : g == 7 ? 64 : 16;
                double bound = std::ldexp(1., bits - 1);
                // Masked invalid conversion stores the width-specific signed
                // indefinite (8000h/80000000h/8000000000000000h) and sets IE.
                // FIXME: unmasked IE must suppress completion and be delivered.
                // Vol. 2 FIST/FISTP, Table 3-36:
                // https://cdrdv2-public.intel.com/835757/325383-sdm-vol-2abcd.pdf#page=508
                bool invalid = !std::isfinite(x) || x < -bound || x >= bound;
                uint64_t result = invalid ? uint64_t(1) << (bits - 1) : uint64_t(int64_t(x));
                if (invalid) fpStatus |= InvalidOperation;
                if (bits == 64) write_qword(at, result); else write(at, uint32_t(result), bits / 8);
            }
            if (g != 2) pop_stack();
        } else throw Fault{6, 0, false};
    } else if (op == 0xd8 || op == 0xdc || op == 0xde) {
        if (op == 0xde && m == 0xd9) { compare(stack_value(0), stack_value(1)); pop_stack(); pop_stack(); }
        else if (g == 2 || g == 3) { compare(stack_value(0), stack_value(i)); if (g == 3) pop_stack(); }
        else if (op == 0xd8) set_stack_value(0, arithmetic(g, stack_value(0), stack_value(i)));
        else { set_stack_value(i, arithmetic(g >= 4 ? g ^ 1 : g, stack_value(i), stack_value(0))); if (op == 0xde) pop_stack(); }
    } else if (op == 0xd9) {
        if (g == 0) { double x = stack_value(i); push_stack(x); }
        else if (g == 1) { double x = stack_value(0), y = stack_value(i); set_stack_value(0, y); set_stack_value(i, x); }
        else switch (m) {
        case 0xd0: break;
        case 0xe0: set_stack_value(0, -stack_value(0)); break;
        case 0xe1: set_stack_value(0, std::fabs(stack_value(0))); break;
        case 0xe4: compare(stack_value(0), 0); break;
        case 0xe5: {
            double x = stack_value(0); fpStatus &= ~ClassMask;
            if (std::signbit(x)) fpStatus |= Sign;
            fpStatus |= ((fpTag >> (physical_slot(0) * 2)) & 3) == 3 ? 0x4100 :
                std::isnan(x) ? 0x100 : std::isinf(x) ? 0x500 : x == 0 ? 0x4000 : 0x400;
            break;
        }
        case 0xe8: push_stack(1); break;
        case 0xe9: push_stack(std::log2(10.)); break;
        case 0xea: push_stack(std::log2(std::exp(1.))); break;
        case 0xeb: push_stack(std::acos(-1.)); break;
        case 0xec: push_stack(std::log10(2.)); break;
        case 0xed: push_stack(std::log(2.)); break;
        case 0xee: push_stack(0); break;
        case 0xf0: set_stack_value(0, std::exp2(stack_value(0)) - 1); break;
        case 0xf1: set_stack_value(1, stack_value(1) * std::log2(stack_value(0))); pop_stack(); break;
        case 0xf2: set_stack_value(0, std::tan(stack_value(0))); push_stack(1); fpStatus &= ~0x400u; break;
        case 0xf3: set_stack_value(1, std::atan2(stack_value(1), stack_value(0))); pop_stack(); break;
        case 0xf5: set_stack_value(0, std::remainder(stack_value(0), stack_value(1))); fpStatus &= ~0x400u; break;
        case 0xf6: top = (top - 1) & 7; break;
        case 0xf7: top = (top + 1) & 7; break;
        case 0xf8: set_stack_value(0, std::fmod(stack_value(0), stack_value(1))); fpStatus &= ~0x400u; break;
        case 0xf9: set_stack_value(1, stack_value(1) * std::log2(stack_value(0) + 1)); pop_stack(); break;
        case 0xfa: set_stack_value(0, std::sqrt(stack_value(0))); break;
        case 0xfb: { double x = stack_value(0); set_stack_value(0, std::sin(x)); push_stack(std::cos(x)); fpStatus &= ~0x400u; break; }
        case 0xfc: set_stack_value(0, round_integer(stack_value(0))); break;
        case 0xfd: set_stack_value(0, stack_value(0) * std::exp2(std::trunc(stack_value(1)))); break;
        case 0xfe: set_stack_value(0, std::sin(stack_value(0))); fpStatus &= ~0x400u; break;
        case 0xff: set_stack_value(0, std::cos(stack_value(0))); fpStatus &= ~0x400u; break;
        default: throw Fault{6, 0, false};
        }
    } else if (op == 0xdd) {
        if (g == 0) set_tag(i, EmptyTag);
        else if (g == 2 || g == 3) { set_stack_value(i, stack_value(0)); if (g == 3) pop_stack(); }
        else if (g == 4 || g == 5) { compare(stack_value(0), stack_value(i)); if (g == 5) pop_stack(); }
        else throw Fault{6, 0, false};
    } else if (op == 0xdb && m == 0xe3) initialize();
    else if (op == 0xdb && m == 0xe2) fpStatus &= ~ClearExceptions;
    else if (op == 0xdb && (m == 0xe0 || m == 0xe1 || m == 0xe4)) {} // 287 compatibility control.
    else if (op == 0xda && m == 0xe9) { compare(stack_value(0), stack_value(1)); pop_stack(); pop_stack(); }
    else if (op == 0xdf && m == 0xe0) { publish_top(); CPU::set(AX, fpStatus, 2); }
    else throw Fault{6, 0, false};
    publish_top();
}
} // namespace pc
