// Optional Hyper-V VMBus namespace. PPI 16 is reserved for its SynIC SINT.
// No MMIO windows or synthetic channels are advertised by this control bus.
DefinitionBlock ("", "SSDT", 2, "PCEMU ", "VMBUS   ", 1)
{
    Scope (\_SB)
    {
        Device (VMOD)
        {
            Name (_HID, "ACPI0004")
            Name (_UID, Zero)
            Name (_CCA, One)
            Device (VMBS)
            {
                Name (_HID, "VMBus")
                Name (_DDN, "VMBUS")
                Name (_UID, Zero)
                Name (_ADR, Zero)
                Name (STA, 0x0F)
                Method (_STA, 0) { Return (STA) }
                Method (_DIS, 0) { And (STA, 0x0D, STA) }
                Method (_PS0, 0) { Or (STA, 0x0F, STA) }
                Method (_PS3, 0) { Store (Zero, STA) }
                Name (_CRS, ResourceTemplate ()
                {
                    Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive) { 16 }
                })
            }
        }
    }
}
