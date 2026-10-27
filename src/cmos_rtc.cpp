// PC/AT firmware exposes a battery-backed real-time clock (RTC) and configuration
// RAM through index port 0x70 and data port 0x71. Software first selects a byte,
// then reads it. Time registers contain calendar fields; separate CMOS RAM bytes
// tell the BIOS which disks, display and memory are installed. CMOS describes the
// chip technology, not a different calendar format.
//
// This model synthesizes time from host local time plus a VM-only offset. Calendar
// fields use binary-coded decimal (BCD): each decimal digit occupies four bits.
// Hardware configuration comes from machine composition rather than writable RAM.
// IMPLEMENTATION NOTE: reset changes only the selected index; VM clock offset is
// retained. Host callbacks and installed-hardware configuration are runtime wiring.
// FIXME: Implement alarm/day-of-week, update-in-progress, interrupt flags, writable
// control/CMOS RAM and alternate time formats before claiming full MC146818A support.
// Reference: IBM Personal Computer AT Technical Reference, 6280070, September 1985,
// Real-Time Clock/CMOS RAM Information (1-56 through 1-68), and Motorola MC146818A.
// https://minuszerodegrees.net/manuals/IBM_5170_Technical_Reference_6280070_SEP85.pdf#page=72
// https://www.scs.stanford.edu/10wi-cs140/pintos/specs/mc146818a.pdf#page=15

#include "cmos_rtc.hpp"
#include <ctime>
#include <utility>
namespace pc {
namespace {
// IBM RTC and CMOS address maps; offsets select bytes, not CPU memory addresses.
// https://minuszerodegrees.net/manuals/IBM_5170_Technical_Reference_6280070_SEP85.pdf#page=72
// https://minuszerodegrees.net/manuals/IBM_5170_Technical_Reference_6280070_SEP85.pdf#page=73
enum class Register : uint8_t {
    // Seconds field, BCD 00 through 59.
    Seconds = 0x00,
    // Minutes field, BCD 00 through 59.
    Minutes = 0x02,
    // Hours field; this model always uses the 24-hour BCD format.
    Hours = 0x04,
    // Calendar day within the current month, starting at one.
    DateOfMonth = 0x07,
    // Calendar month, January through December encoded 01 through 12.
    Month = 0x08,
    // Last two decimal digits of the year; no century is returned here.
    Year = 0x09,
    // Divider/rate selection and Update In Progress status.
    StatusRegisterA = 0x0a,
    // Calendar format and interrupt/update controls.
    StatusRegisterB = 0x0b,
    // Valid RAM and Time status.
    StatusRegisterD = 0x0d,
    // Drive A/B type nibbles in BIOS configuration RAM.
    DisketteDriveType = 0x10,
    // Drive C/D type nibbles; type 15 selects an extended type byte.
    FixedDiskType = 0x12,
    // Display, diskette and math-coprocessor presence flags.
    EquipmentByte = 0x14,
    // Low byte of conventional memory size, in KiB.
    LowBaseMemoryByte = 0x15,
    // High byte of conventional memory size, in KiB.
    HighBaseMemoryByte = 0x16,
    // Low byte of RAM above the first MiB, in KiB.
    LowExpansionMemoryByte = 0x17,
    // High byte of RAM above the first MiB, in KiB.
    HighExpansionMemoryByte = 0x18,
    // POST copy of the low expansion-memory byte.
    LowExpansionMemoryMirror = 0x30,
    // POST copy of the high expansion-memory byte.
    HighExpansionMemoryMirror = 0x31,
};
// Status-register fields and BIOS equipment/memory encodings.
// https://minuszerodegrees.net/manuals/IBM_5170_Technical_Reference_6280070_SEP85.pdf#page=74
// https://minuszerodegrees.net/manuals/IBM_5170_Technical_Reference_6280070_SEP85.pdf#page=80
// Port 0x70 bit 7 controls board NMI masking, not the CMOS index.
constexpr uint8_t RegisterIndexMask = 0x7f;
// Register A DV2:DV0 = 010 selects the 32.768-kHz oscillator.
constexpr uint8_t Divider32768Hz = 0x20;
// Register A RS3:RS0 = 0110 selects 1024 Hz; no IRQ is generated here.
constexpr uint8_t PeriodicRate1024Hz = 0x06;
// Register B bit 1 selects 24-hour time; DM=0 selects BCD.
constexpr uint8_t TwentyFourHourMode = 0x02;
// Register D VRT=1 reports clock/RAM contents valid.
constexpr uint8_t ValidRamAndTime = 0x80;
// COMPATIBILITY: Later BIOS type 4 means a 1.44-MB (1440-KiB) diskette in drive A.
constexpr uint8_t DriveAType1440KiB = 0x40;
// Drive C nibble 15 says to consult the extended disk type at index 0x19.
constexpr uint8_t DriveCExtendedType = 0xf0;
// Equipment bits 5:4 = 10 advertise an 80-column color display.
constexpr uint8_t ColorDisplay80Columns = 0x20;
// Equipment bit 0 reports that a diskette drive is installed.
constexpr uint8_t DiskettePresent = 0x01;
// Equipment bit 1 reports the installed math coprocessor.
constexpr uint8_t MathCoprocessorPresent = 0x02;
// COMPATIBILITY: Conventional memory is always advertised as 640 KiB.
constexpr uint32_t BaseMemoryKiB = 640;
// BIOS memory-size configuration fields count kibibytes, not bytes.
constexpr size_t BytesPerKiB = 1024;
// Expansion memory excludes conventional memory and the first-MiB adapter area.
constexpr size_t FirstMiBBytes = 1024 * BytesPerKiB;
// Memory-size words are split into eight-bit low/high CMOS bytes.
constexpr unsigned ByteBits = 8;
// Retain only the low byte when exposing a multi-byte memory-size field.
constexpr unsigned ByteMask = 0xff;
// Encode one two-digit calendar field as tens in bits 7:4 and units in bits 3:0.
// Motorola MC146818A, Time, Calendar and Alarm data modes.
// https://www.scs.stanford.edu/10wi-cs140/pintos/specs/mc146818a.pdf#page=12
uint32_t encode_binary_coded_decimal(int value) {
    return (value / 10) * 16 + value % 10;
}
} // namespace

// Bind an injectable seconds source; without one use host wall-clock seconds.
// IMPLEMENTATION NOTE: Clock sourcing is host policy, supporting the RTC fields.
// IBM Technical Reference, Real-Time Clock Information.
// https://minuszerodegrees.net/manuals/IBM_5170_Technical_Reference_6280070_SEP85.pdf#page=73
CmosRtc::CmosRtc(std::function<int64_t()> clock) : seconds(std::move(clock)) {
    if (!seconds)
        seconds = [] { return int64_t(std::time(nullptr)); };
}

// Return the selected RTC/calendar or BIOS configuration byte.
// IBM Technical Reference, CMOS address map and I/O Operations.
// https://minuszerodegrees.net/manuals/IBM_5170_Technical_Reference_6280070_SEP85.pdf#page=72
// https://minuszerodegrees.net/manuals/IBM_5170_Technical_Reference_6280070_SEP85.pdf#page=84
uint32_t CmosRtc::read_data_register() const {
    std::time_t current = time_seconds();
    // IMPLEMENTATION NOTE: Use the host timezone; this is not a guest timezone setting.
    auto t = *std::localtime(&current);
    switch (static_cast<Register>(state.index & RegisterIndexMask)) {
    case Register::Seconds:
        return encode_binary_coded_decimal(t.tm_sec);
    case Register::Minutes:
        return encode_binary_coded_decimal(t.tm_min);
    case Register::Hours:
        return encode_binary_coded_decimal(t.tm_hour);
    case Register::DateOfMonth:
        return encode_binary_coded_decimal(t.tm_mday);
    case Register::Month:
        return encode_binary_coded_decimal(t.tm_mon + 1);
    case Register::Year:
        return encode_binary_coded_decimal(t.tm_year % 100);
    // Status registers A/B/D: fixed divider, 24-hour BCD, valid clock.
    // https://minuszerodegrees.net/manuals/IBM_5170_Technical_Reference_6280070_SEP85.pdf#page=74
    // https://www.scs.stanford.edu/10wi-cs140/pintos/specs/mc146818a.pdf#page=16
    case Register::StatusRegisterA:
        return Divider32768Hz | PeriodicRate1024Hz;
    case Register::StatusRegisterB:
        return TwentyFourHourMode;
    case Register::StatusRegisterD:
        return ValidRamAndTime;
    // BIOS device types and equipment, not calendar-chip hardware fields.
    // https://minuszerodegrees.net/manuals/IBM_5170_Technical_Reference_6280070_SEP85.pdf#page=77
    // https://minuszerodegrees.net/manuals/IBM_5170_Technical_Reference_6280070_SEP85.pdf#page=78
    case Register::DisketteDriveType:
        return config.floppy ? DriveAType1440KiB : 0;
    case Register::FixedDiskType:
        // FIXME: Supply the extended type byte (0x19) when advertising type 15.
        return config.disk ? DriveCExtendedType : 0;
    case Register::EquipmentByte:
        return ColorDisplay80Columns | MathCoprocessorPresent | (config.floppy ? DiskettePresent : 0);
    // BIOS memory sizes use low-byte/high-byte words, in KiB.
    // https://minuszerodegrees.net/manuals/IBM_5170_Technical_Reference_6280070_SEP85.pdf#page=80
    // https://minuszerodegrees.net/manuals/IBM_5170_Technical_Reference_6280070_SEP85.pdf#page=81
    case Register::LowBaseMemoryByte:
        return BaseMemoryKiB & ByteMask;
    case Register::HighBaseMemoryByte:
        return BaseMemoryKiB >> ByteBits;
    case Register::LowExpansionMemoryByte:
    case Register::LowExpansionMemoryMirror:
        return ((config.ramBytes - FirstMiBBytes) / BytesPerKiB) & ByteMask;
    case Register::HighExpansionMemoryByte:
    case Register::HighExpansionMemoryMirror:
        return ((config.ramBytes - FirstMiBBytes) / BytesPerKiB) >> ByteBits;
    default:
        // COMPATIBILITY: Unimplemented/reserved RTC and RAM bytes read as zero.
        return 0;
    }
}
} // namespace pc
