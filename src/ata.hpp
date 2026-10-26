#pragma once
#include <array>
#include <cstdint>
namespace pc {
struct Disk;
// Primary-channel ATA PIO task file. Commands follow the ATA-4 PIO protocols;
// the data register transfers words, not consecutive I/O-port addresses.
class ATA {
  public:
    Disk *disk = nullptr;
    bool irq = false;
    uint8_t control = 0;
    // Bind one disk as device 0; initialize the retained task-file reset signature.
    // IMPLEMENTATION NOTE: PC board initialization around ATA-4, 9.1.
    // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=226
    explicit ATA(Disk *d = nullptr) : disk(d) {}
    // Report the pending device interrupt after applying Device Control.nIEN.
    // ATA-4, 7.9 Device Control register.
    // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=61
    bool interrupt_level() const {
        return irq && !(control & InterruptDisable);
    }
    // Rebind backing storage and clear transfer/interrupt state to initial values.
    // IMPLEMENTATION NOTE: Host reset around ATA-4, 9.1 signature.
    // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=226
    void reset(Disk *backing) {
        *this = ATA(backing);
    }
    // Read a task-file register or consume bytes from the current PIO data block.
    uint32_t read_register(uint16_t port, int width);
    // Write task-file parameters, begin commands, reset or supply PIO data bytes.
    void write_register(uint16_t port, uint32_t value, int width);

  private:
    friend class SnapshotCodec;
    // ATA/ATAPI-4, section 7 register map. These are PC primary-channel port
    // assignments; ATA specifies chip-select/address lines, not these PC ports.
    // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=55
    enum Register : uint16_t {
        DataRegister = 0x1f0,         // Sector data stream, normally transferred as 16-bit words.
        ErrorRegister = 0x1f1,        // Read: command error/diagnostic code.
        FeaturesRegister = 0x1f1,     // Write: command-specific feature parameter.
        SectorCountRegister = 0x1f2,  // Number of sectors; zero encodes 256 for these commands.
        SectorNumberRegister = 0x1f3, // CHS sector number or LBA bits [7:0].
        CylinderLowRegister = 0x1f4,  // Cylinder low byte or LBA bits [15:8].
        CylinderHighRegister = 0x1f5, // Cylinder high byte or LBA bits [23:16].
        DeviceHeadRegister = 0x1f6,   // Device selection, addressing mode and head/LBA high nibble.
        StatusRegister = 0x1f7,       // Read: status and acknowledge the pending interrupt.
        CommandRegister = 0x1f7,      // Write: begin the selected command.
        AlternateStatusRegister = 0x3f6, // Read status without acknowledging interrupt.
        DeviceControlRegister = 0x3f6,   // Write software reset and interrupt disable.
    };
    // Command encodings: ATA/ATAPI-4, clause 8. The paired 20/21, 30/31 and
    // 40/41 codes have identical behavior in ATA-4 (legacy retry distinction).
    enum Command : uint8_t {
        Recalibrate = 0x10, // Retired command retained as a successful compatibility no-op.
        // Read sectors through the PIO input stream.
        // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=154
        ReadSectors = 0x20,
        // Same ATA-4 operation; legacy no-retry encoding.
        // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=154
        ReadSectorsWithoutRetry = 0x21,
        // Write sectors supplied through the PIO output stream.
        // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=223
        WriteSectors = 0x30,
        // Same ATA-4 operation; legacy no-retry encoding.
        // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=223
        WriteSectorsWithoutRetry = 0x31,
        // Verify media without transferring sector data to the host.
        // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=157
        ReadVerifySectors = 0x40,
        // Same ATA-4 operation; legacy no-retry encoding.
        // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=157
        ReadVerifySectorsWithoutRetry = 0x41,
        // Run device diagnostics and return a diagnostic code.
        // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=87
        ExecuteDeviceDiagnostic = 0x90,
        // Set the translated CHS head and sectors-per-track geometry.
        // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=124
        InitializeDeviceParameters = 0x91,
        // Enter standby immediately (model completes without changing power).
        // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=209
        StandbyImmediate = 0xe0,
        // Enter idle immediately (model completes without changing power).
        // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=122
        IdleImmediate = 0xe1,
        // Enter idle and set standby timer (model does neither).
        // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=119
        Idle = 0xe3,
        // Return the current power mode in Sector Count.
        // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=81
        CheckPowerMode = 0xe5,
        // Flush cached writes (model adds no backing flush operation).
        // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=89
        FlushCache = 0xe7,
        // Return the 512-byte device identification data block.
        // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=93
        IdentifyDevice = 0xec,
        // Configure command-specific features (model accepts without applying).
        // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=177
        SetFeatures = 0xef,
    };
    // Status register, 7.15: BSY, DRDY, DRQ and ERR. Bit 4 is the obsolete
    // drive-seek-complete indication retained for older software (0x50 baseline).
    static constexpr uint8_t Busy = 0x80, DeviceReady = 0x40, LegacySeekComplete = 0x10,
                             DataRequest = 0x08, CommandError = 0x01;
    static constexpr uint8_t ReadyStatus = DeviceReady | LegacySeekComplete;
    static constexpr uint8_t DataReadyStatus = ReadyStatus | DataRequest;
    static constexpr uint8_t ErrorStatus = ReadyStatus | CommandError;
    // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=65
    // Device Control, 7.9: nIEN masks the output pin without clearing pending IRQ;
    // SRST holds reset active until software clears it.
    static constexpr uint8_t InterruptDisable = 0x02, SoftwareReset = 0x04;
    // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=61
    // Device/Head command fields: DEV selects device 1, LBA selects linear
    // addressing, low nibble is CHS head or the upper four bits of a 28-bit LBA.
    static constexpr uint8_t DeviceOne = 0x10, LbaAddressing = 0x40, HeadOrLbaHighMask = 0x0f,
                             DeviceModeMask = 0xf0;
    // COMPATIBILITY: Select device 0 with historical fixed bits 7/5 set.
    static constexpr uint8_t InitialDeviceHead = 0xa0;
    // Error register: ABRT rejects a command, IDNF means address not found,
    // UNC reports an unrecoverable media/backing I/O error (7.11 / clause 8).
    // https://www.mit.edu/afs/sipb/contrib/doc/specs/protocol/ata/d1153r18.pdf#page=62
    static constexpr uint8_t AbortedCommand = 0x04, IdNotFound = 0x10, UncorrectableData = 0x40;
    // EXECUTE DEVICE DIAGNOSTIC error-code value 01h means device 0 passed.
    static constexpr uint8_t DiagnosticPassed = 0x01;
    // ATA sector transfers and IDENTIFY blocks contain 256 16-bit words.
    static constexpr unsigned SectorBytes = 512, WordBytes = 2, BitsPerByte = 8, BitsPerWord = 16,
                              MaximumSectorCount = 256;
    // IMPLEMENTATION NOTE: Default translated CHS geometry used by this board.
    static constexpr unsigned DefaultHeads = 16, DefaultSectorsPerTrack = 63;
    // IDENTIFY logical cylinder field is limited to 16383 for translated geometry.
    static constexpr unsigned MaximumIdentifyCylinders = 16383;
    // CHECK POWER MODE sector-count result FFh means active or idle (8.6).
    static constexpr uint8_t ActiveOrIdlePowerMode = 0xff;

    // ATA disk signature uses sector count/number 1 and cylinder bytes 0 (9.1).
    uint8_t error = DiagnosticPassed, count = 1, sector = 1, cylLow = 0, cylHigh = 0,
            device = InitialDeviceHead, status = ReadyStatus, features = 0;
    std::array<uint8_t, SectorBytes> buffer{};
    unsigned position = 0, remaining = 0, heads = DefaultHeads, spt = DefaultSectorsPerTrack;
    uint64_t lba = 0;
    bool writing = false, identify = false;
    // Start a command after clearing the previous transfer state.
    void execute_command(uint8_t command);
    // End a command with Error/status and a pending interrupt.
    void complete_with_error(uint8_t code = AbortedCommand);
    // Fetch one backing sector into the PIO read buffer.
    bool load_read_sector();
    // Consume one sector and update the selected CHS/LBA task-file address.
    void advance_sector_address();
    // Build the IDENTIFY words and begin their PIO data-in transfer.
    void prepare_identify_data();
};
} // namespace pc
