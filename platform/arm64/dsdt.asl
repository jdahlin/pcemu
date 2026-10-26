// Original namespace for the single-CPU PCEmu ARM virtual platform.
DefinitionBlock ("", "DSDT", 2, "PCEMU ", "ARMVIRT ", 1)
{
    Scope (\_SB)
    {
        Device (CPU0)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, Zero)
        }
        Device (PCI0)
        {
            Name (_HID, EisaId ("PNP0A08"))
            Name (_CID, EisaId ("PNP0A03"))
            Name (_SEG, Zero)
            Name (_BBN, Zero)
            Name (_UID, Zero)
            Name (_CCA, One)
            Name (_CRS, ResourceTemplate ()
            {
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    0, 0, 0, 0, 1)
                DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite, 0, 0x10000000, 0x3EFFFFFF,
                    0, 0x2F000000)
            })
            // Separate level-high INTx for boot disk, target disk, network and RNG.
            Name (_PRT, Package () {
                Package () { 0xFFFF, Zero, Zero, 48 },
                Package () { 0x1FFFF, Zero, Zero, 49 },
                Package () { 0x2FFFF, Zero, Zero, 50 },
                Package () { 0x3FFFF, Zero, Zero, 51 },
                Package () { 0x4FFFF, Zero, Zero, 52 },
                Package () { 0x5FFFF, Zero, Zero, 53 },
                Package () { 0x6FFFF, Zero, Zero, 54 },
                Package () { 0x7FFFF, Zero, Zero, 55 },
                Package () { 0x8FFFF, Zero, Zero, 56 }
            })
        }
        Device (COM0)
        {
            Name (_HID, "ARMH0011")
            Name (_UID, Zero)
            Name (_CCA, One)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x09000000, 0x1000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 33 }
            })
        }
        Device (FWCF)
        {
            Name (_HID, "QEMU0002")
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x09020000, 0x18)
            })
        }
        Device (RES0)
        {
            Name (_HID, EisaId ("PNP0C02"))
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x08000000, 0x10000)
                Memory32Fixed (ReadWrite, 0x080A0000, 0x20000)
                Memory32Fixed (ReadWrite, 0x3F000000, 0x100000)
            })
        }
    }
}
