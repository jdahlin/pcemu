// ATA (AT Attachment) exposes a disk through task-file registers and a PIO
// (programmed input/output) data stream. The guest selects device 0, supplies a
// sector count and either a CHS (cylinder/head/sector) address or a 28-bit LBA
// (logical block address), then writes a command. Read/write commands transfer
// one 512-byte sector at a time through the same Data register, advance the
// address/count registers and signal the machine's primary disk interrupt.
// Reading Status acknowledges that interrupt; Alternate Status does not.
// IDENTIFY DEVICE instead returns a word-oriented description of this disk.
//
// IMPLEMENTATION NOTE: Backing I/O completes synchronously. There is one device,
// no ATAPI packet device, DMA, 48-bit addressing, cable timing or asynchronous BSY.
// Byte and DWORD data accesses serialize bytes through the same stream for guest
// compatibility; they do not address consecutive task-file ports.
// COMPATIBILITY: READ VERIFY checks bounds without reading media; power commands,
// SET FEATURES, FLUSH CACHE and retired RECALIBRATE simply complete successfully.
// Power state and cache/feature settings are not modeled. Disk::write owns actual
// persistence; successful FLUSH CACHE adds no separate durability operation.
// FIXME: Implement the specified command effects/error paths and reset/diagnostic
// signatures before claiming full ATA-4 conformance. IDENTIFY retains a sparse
// compatibility response, including its legacy capability word and version claim.
// Reference: T13/1153D revision 18, ATA/ATAPI-4 (19 August 1998), clauses 7-9.
// https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=55
// https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=93
// https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=226

#include "ata.hpp"
#include "disk.hpp"
#include <algorithm>
#include <cstring>
namespace pc {
namespace {
// IDENTIFY DEVICE response word indices, ATA-4 8.12 Table 11.
// https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=95
// https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=96
enum IdentifyWord : unsigned {
    // Non-removable ATA device; bit 6 set.
    GeneralConfiguration = 0,
    // Translated logical cylinder count.
    LogicalCylinders = 1,
    // Translated logical heads per cylinder.
    LogicalHeads = 3,
    // Translated logical sectors per track.
    LogicalSectorsPerTrack = 6,
    // Serial number: 20 space-padded ASCII characters.
    SerialNumber = 10,
    // Firmware revision: 8 space-padded ASCII characters.
    FirmwareRevision = 23,
    // Model name: 40 space-padded ASCII characters.
    ModelNumber = 27,
    // High byte 80h; low byte zero retains no MULTIPLE command support.
    MultipleSectorCapability = 47,
    // COMPATIBILITY: Legacy LBA capability bit 9; other ATA-4 required bits absent.
    Capabilities = 49,
    // Bit 0 marks current geometry words 54-58 valid.
    FieldValidity = 53,
    // Current translated cylinder count.
    CurrentLogicalCylinders = 54,
    // Current translated head count.
    CurrentLogicalHeads = 55,
    // Current translated sectors per track.
    CurrentLogicalSectorsPerTrack = 56,
    // Current CHS capacity, low 16 bits.
    CurrentCapacityLow = 57,
    // Current CHS capacity, high 16 bits.
    CurrentCapacityHigh = 58,
    // Total LBA sector count, low 16 bits.
    UserAddressableSectorsLow = 60,
    // Total LBA sector count, high 16 bits.
    UserAddressableSectorsHigh = 61,
    // Bit 4 advertises ATA/ATAPI-4; sparse response is not full conformance.
    MajorVersion = 80,
};
} // namespace

// End the transfer with Error/status set and a pending device interrupt.
// ATA/ATAPI-4, 7.11 Error and 7.15 Status registers.
// https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=62
void ATA::complete_with_error(uint8_t code) {
    error = code;
    status = ErrorStatus;
    remaining = 0;
    irq = true;
}
// Read the addressed backing sector into the PIO buffer; report bounds/media errors.
// ATA/ATAPI-4, 9.7 PIO data in.
// https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=240
bool ATA::load_read_sector() {
    if (!disk || lba >= disk->sectors) {
        complete_with_error(IdNotFound);
        return false;
    }
    if (!disk->read(lba, 1, buffer.data())) {
        complete_with_error(UncorrectableData);
        return false;
    }
    position = 0;
    status = DataReadyStatus;
    irq = true;
    return true;
}
// Consume one sector and update count/address registers in the selected CHS/LBA mode.
// ATA/ATAPI-4, 8.27 READ SECTOR(S), command outputs.
// https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=154
void ATA::advance_sector_address() {
    lba++;
    count = --remaining;
    // LBA packs bits [27:24] in Device/Head, then three task-file address bytes.
    // In CHS mode sectors are one-based, heads/cylinders zero-based.
    if (device & LbaAddressing) {
        sector = lba;
        cylLow = lba >> 8;
        cylHigh = lba >> 16;
        device = (device & DeviceModeMask) | ((lba >> 24) & HeadOrLbaHighMask);
    } else {
        unsigned track = lba / spt;
        sector = lba % spt + 1;
        device = (device & DeviceModeMask) | (track % heads);
        unsigned cylinder = track / heads;
        cylLow = cylinder;
        cylHigh = cylinder >> 8;
    }
    position = 0;
}
// Prepare one IDENTIFY response block and raise its PIO data-ready interrupt.
// ATA/ATAPI-4, 8.12 IDENTIFY DEVICE.
// https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=93
void ATA::prepare_identify_data() {
    buffer.fill(0);
    // Store one IDENTIFY word little-endian in the PIO buffer (8.12, Table 11).
    // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=95
    auto write_identify_word = [&](unsigned i, uint16_t v) {
        buffer[i * WordBytes] = v;
        buffer[i * WordBytes + 1] = v >> BitsPerByte;
    };
    // ATA text swaps bytes within each word and space-pads fixed-width fields.
    // ATA-4, 8.12: IDENTIFY DEVICE data format.
    // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=94
    auto write_identify_text = [&](unsigned i, unsigned n, const char *s) {
        auto len = std::strlen(s);
        for (unsigned j = 0; j < n; j++)
            buffer[i * WordBytes + (j ^ 1)] = j < len ? s[j] : ' ';
    };
    unsigned cylinders =
        std::min<uint64_t>(MaximumIdentifyCylinders, disk->sectors / (heads * spt));
    write_identify_word(IdentifyWord::GeneralConfiguration, 1u << 6);
    write_identify_word(IdentifyWord::LogicalCylinders, cylinders);
    write_identify_word(IdentifyWord::LogicalHeads, heads);
    write_identify_word(IdentifyWord::LogicalSectorsPerTrack, spt);
    write_identify_text(IdentifyWord::SerialNumber, 20, "PCEMU0000000000000001");
    write_identify_text(IdentifyWord::FirmwareRevision, 8, "0.1");
    write_identify_text(IdentifyWord::ModelNumber, 40, "PCEMU ORIGINAL ATA PIO DISK");
    write_identify_word(IdentifyWord::MultipleSectorCapability, 0x80u << 8);
    write_identify_word(IdentifyWord::Capabilities, 1u << 9);
    write_identify_word(IdentifyWord::FieldValidity, 1);
    write_identify_word(IdentifyWord::CurrentLogicalCylinders, cylinders);
    write_identify_word(IdentifyWord::CurrentLogicalHeads, heads);
    write_identify_word(IdentifyWord::CurrentLogicalSectorsPerTrack, spt);
    auto chs = uint32_t(cylinders * heads * spt);
    write_identify_word(IdentifyWord::CurrentCapacityLow, chs);
    write_identify_word(IdentifyWord::CurrentCapacityHigh, chs >> BitsPerWord);
    write_identify_word(IdentifyWord::UserAddressableSectorsLow, disk->sectors);
    write_identify_word(IdentifyWord::UserAddressableSectorsHigh, disk->sectors >> BitsPerWord);
    write_identify_word(IdentifyWord::MajorVersion, 1u << 4);
    position = 0;
    remaining = 1;
    identify = true;
    writing = false;
    status = DataReadyStatus;
    irq = true;
}
// Decode the Command register, validate parameters and start/complete the operation.
// ATA/ATAPI-4, clause 8 Command descriptions (compatibility subset).
// https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=68
void ATA::execute_command(uint8_t cmd) {
    irq = false;
    error = 0;
    position = 0;
    remaining = 0;
    identify = false;
    writing = false;
    if (!disk || (device & DeviceOne)) {
        status = 0;
        return;
    }
    if (cmd == Command::IdentifyDevice) {
        prepare_identify_data();
        return;
    }
    if (cmd == Command::ReadSectors || cmd == Command::ReadSectorsWithoutRetry ||
        cmd == Command::WriteSectors || cmd == Command::WriteSectorsWithoutRetry ||
        cmd == Command::ReadVerifySectors || cmd == Command::ReadVerifySectorsWithoutRetry) {
        remaining = count ? count : MaximumSectorCount;
        if (device & LbaAddressing)
            lba = uint64_t(device & HeadOrLbaHighMask) << 24 | uint32_t(cylHigh) << 16 |
                  uint32_t(cylLow) << 8 | sector;
        else {
            if (!sector || sector > spt || (device & HeadOrLbaHighMask) >= heads) {
                complete_with_error(IdNotFound);
                return;
            }
            lba = (uint64_t(uint16_t(cylLow) | (cylHigh << 8)) * heads +
                   (device & HeadOrLbaHighMask)) *
                      spt +
                  sector - 1;
        }
        if (lba >= disk->sectors || remaining > disk->sectors - lba) {
            complete_with_error(IdNotFound);
            return;
        }
        if (cmd == Command::ReadVerifySectors || cmd == Command::ReadVerifySectorsWithoutRetry) {
            while (remaining)
                advance_sector_address();
            status = ReadyStatus;
            irq = true;
            return;
        }
        writing = cmd == Command::WriteSectors || cmd == Command::WriteSectorsWithoutRetry;
        if (writing) {
            status = DataReadyStatus;
            buffer.fill(0);
        } else
            load_read_sector();
        return;
    }
    if (cmd == Command::InitializeDeviceParameters) {
        unsigned heads = (device & HeadOrLbaHighMask) + 1;
        if (count) {
            this->heads = heads;
            spt = count;
        }
        status = ReadyStatus;
        irq = true;
        return;
    }
    // COMPATIBILITY: Report a passing device 0 without restoring the full signature.
    if (cmd == Command::ExecuteDeviceDiagnostic) {
        error = DiagnosticPassed;
        status = ReadyStatus;
        irq = true;
        return;
    }
    if (cmd == Command::FlushCache || cmd == Command::SetFeatures ||
        cmd == Command::StandbyImmediate || cmd == Command::IdleImmediate || cmd == Command::Idle ||
        cmd == Command::Recalibrate) {
        status = ReadyStatus;
        irq = true;
        return;
    }
    if (cmd == Command::CheckPowerMode) {
        count = ActiveOrIdlePowerMode;
        status = ReadyStatus;
        irq = true;
        return;
    }
    complete_with_error();
}
// Read task-file state or consume PIO bytes; Status alone clears the pending interrupt.
// ATA/ATAPI-4, 7.3 Alternate Status / clause 7 register descriptions.
// https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=57
uint32_t ATA::read_register(uint16_t p, int w) {
    if (!disk || (device & DeviceOne))
        return 0;
    if (p == Register::DataRegister) {
        if (!(status & DataRequest) || writing)
            // No readable DRQ block: all data lines high at the requested bus width.
            return w == 1 ? UINT8_MAX : w == 2 ? UINT16_MAX : UINT32_MAX;
        uint32_t value = 0;
        for (int i = 0; i < w; i++) {
            value |= uint32_t(buffer[position++]) << (i * BitsPerByte);
            if (position == SectorBytes) {
                if (identify) {
                    remaining = 0;
                    status = ReadyStatus;
                    position = 0;
                } else {
                    advance_sector_address();
                    if (remaining)
                        load_read_sector();
                    else
                        status = ReadyStatus;
                }
            }
        }
        return value;
    }
    switch (p) {
    case Register::ErrorRegister:
        return error;
    case Register::SectorCountRegister:
        return count;
    case Register::SectorNumberRegister:
        return sector;
    case Register::CylinderLowRegister:
        return cylLow;
    case Register::CylinderHighRegister:
        return cylHigh;
    case Register::DeviceHeadRegister:
        return device;
    case Register::StatusRegister:
        irq = false;
        return status;
    case Register::AlternateStatusRegister:
        return status;
    default:
        // Unimplemented primary-channel register: undriven byte reads high.
        return UINT8_MAX;
    }
}
// Apply reset/selection/task-file writes or consume one PIO output block into storage.
// ATA/ATAPI-4, 9.8 PIO data out / 9.3 software reset.
// https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=243
void ATA::write_register(uint16_t p, uint32_t value, int w) {
    if (p == Register::DeviceControlRegister) {
        // Falling SRST completes our immediate reset; no hardware reset delay is modeled.
        bool reset = (control & SoftwareReset) && !(value & SoftwareReset);
        control = value;
        if (value & SoftwareReset) {
            status = Busy;
            irq = false;
        }
        if (reset) {
            error = DiagnosticPassed;
            count = sector = 1;
            cylLow = cylHigh = 0;
            device = InitialDeviceHead;
            status = ReadyStatus;
            remaining = 0;
            position = 0;
        }
        return;
    }
    if (p == Register::DeviceHeadRegister) {
        device = value;
        return;
    }
    if (p == Register::DataRegister) {
        if (!(status & DataRequest) || !writing)
            return;
        for (int i = 0; i < w; i++) {
            buffer[position++] = value >> (i * BitsPerByte);
            if (position == SectorBytes) {
                if (!disk->write(lba, 1, buffer.data())) {
                    complete_with_error(UncorrectableData);
                    return;
                }
                advance_sector_address();
                status = remaining ? DataReadyStatus : ReadyStatus;
                irq = true;
            }
        }
        return;
    }
    switch (p) {
    case Register::FeaturesRegister:
        features = value;
        break;
    case Register::SectorCountRegister:
        count = value;
        break;
    case Register::SectorNumberRegister:
        sector = value;
        break;
    case Register::CylinderLowRegister:
        cylLow = value;
        break;
    case Register::CylinderHighRegister:
        cylHigh = value;
        break;
    case Register::CommandRegister:
        execute_command(value);
        break;
    default:
        break;
    }
}
} // namespace pc
