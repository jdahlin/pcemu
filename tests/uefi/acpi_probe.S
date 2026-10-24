// Read the firmware's published ACPI configuration, after relocation and copy.
// Check the RSDP, XSDT, every root table and the FADT's DSDT. No fixed table GPA.
.text
.global efi_main
efi_main:
    stp x29, x30, [sp, #-80]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    mov w8, #48
    ldr x19, [x1, #64]  // ConOut
    ldr x20, [x1, #104] // NumberOfTableEntries
    ldr x21, [x1, #112] // ConfigurationTable
    cmp x20, #128
    b.hi failed
find_acpi:
    cbz x20, failed
    ldp x2, x3, [x21]
    adr x4, acpi_guid
    ldp x5, x6, [x4]
    cmp x2, x5
    ccmp x3, x6, #0, eq
    b.eq found_acpi
    add x21, x21, #24
    sub x20, x20, #1
    b find_acpi
found_acpi:
    mov w8, #49
    ldr x22, [x21, #16]
    mov x0, x22
    mov x1, #36
    bl check_range
    adr x2, rsdp_signature
    ldr x2, [x2]
    ldr x3, [x22]
    cmp x2, x3
    b.ne failed
    ldrb w2, [x22, #15]
    cmp w2, #2
    b.lo failed
    mov x0, x22
    mov x1, #20
    bl sum_bytes
    cbnz w2, failed
    mov x0, x22
    mov x1, #36
    bl sum_bytes
    cbnz w2, failed
    mov w8, #50
    ldr x22, [x22, #24]
    mov x0, x22
    bl check_table
    ldr w2, [x22]
    adr x3, xsdt_signature
    ldr w3, [x3]
    cmp w2, w3
    b.ne failed
    sub x20, x1, #36
    tst x20, #7
    b.ne failed
    lsr x20, x20, #3
    cmp x20, #32
    b.hi failed
    add x21, x22, #36
    mov w23, #0
root_loop:
    mov w8, #51
    cbz x20, finished
    ldr x22, [x21], #8
    mov x0, x22
    bl check_table
    ldr w2, [x22]
    adr x3, required_signatures
    mov w4, #1
    mov w5, #6
signature_loop:
    ldr w6, [x3], #4
    cmp w2, w6
    b.ne next_signature
    orr w23, w23, w4
    cmp w4, #1 // FADT: check its 64-bit DSDT pointer too.
    b.ne next_root
    cmp x1, #148
    b.lo failed
    mov w8, #52
    ldr x0, [x22, #140]
    mov x24, x0
    bl check_table
    ldr w2, [x24]
    adr x3, dsdt_signature
    ldr w3, [x3]
    cmp w2, w3
    b.ne failed
    b next_root
next_signature:
    lsl w4, w4, #1
    subs w5, w5, #1
    b.ne signature_loop
next_root:
    sub x20, x20, #1
    b root_loop
finished:
    mov w8, #53
    cmp w23, #63
    b.ne failed
    mov w8, #54
    mov w0, #0x84000000 // PSCI_VERSION
    hvc #0
    cmp w0, #2
    b.ne failed
    mov w0, #0x0003
    movk w0, #0xc400, lsl #16 // CPU_ON64, CPU0 is already on.
    mov x1, #0
    mov x2, #0
    hvc #0
    cmn x0, #4
    b.ne failed
    mov w0, #0x0004
    movk w0, #0xc400, lsl #16 // AFFINITY_INFO64
    hvc #0
    cbnz x0, failed
    mov w8, #55
    mrs x0, ID_AA64DFR0_EL1
    ubfx x0, x0, #8, #4
    cmp x0, #1
    b.ne failed
    mrs x22, PMCCNTR_EL0
    mov x0, #1000
counter_delay:
    subs x0, x0, #1
    b.ne counter_delay
    mrs x23, PMCCNTR_EL0
    mov w8, #56
    cmp x23, x22
    b.ls failed
    mov x24, #0
    adr x1, success_text
    b print_result
failed:
    mov x24, #1
    str w8, [sp, #64]
    adr x1, failure_text
print_result:
    mov x0, x19
    ldr x2, [x0, #8]
    blr x2
    cbnz x24, print_stage
    mov w0, #0x0008
    movk w0, #0x8400, lsl #16 // SYSTEM_OFF must stop execution, not return.
    hvc #0
    b .
print_stage:
    mov x0, x19
    add x1, sp, #64
    ldr x2, [x0, #8]
    blr x2
    mov x0, #0
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x29, x30, [sp], #80
    ret
check_table:
    // Leaf routines deliberately branch to failed with the original stack.
    mov x7, x30
    mov x1, #36
    bl check_range
    ldr w1, [x0, #4]
    cmp x1, #36
    b.lo failed
    cmp x1, #65536
    b.hi failed
    bl check_range
    bl sum_bytes
    cbnz w2, failed
    br x7
check_range:
    mov x2, #0x40000000
    cmp x0, x2
    b.lo failed
    mov x2, #0xc0000000
    cmp x0, x2
    b.hs failed
    sub x2, x2, x0
    cmp x1, x2
    b.hi failed
    ret
sum_bytes:
    mov x3, x0
    mov x4, x1
    mov w2, #0
sum_loop:
    ldrb w5, [x3], #1
    add w2, w2, w5
    subs x4, x4, #1
    b.ne sum_loop
    and w2, w2, #255
    ret
.balign 8
acpi_guid:
    .word 0x8868e871
    .hword 0xe4f1, 0x11d3
    .byte 0xbc, 0x22, 0, 0x80, 0xc7, 0x3c, 0x88, 0x81
rsdp_signature: .ascii "RSD PTR "
xsdt_signature: .ascii "XSDT"
dsdt_signature: .ascii "DSDT"
required_signatures: .ascii "FACPAPICGTDTMCFGSPCRDBG2"
success_text:
    .hword 13,10,80,67,69,77,85,95,65,67,80,73,95,79,75,13,10,0
failure_text:
    .hword 13,10,80,67,69,77,85,95,65,67,80,73,95,70,65,73,76,69,68,13,10,0
