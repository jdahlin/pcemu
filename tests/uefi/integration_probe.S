// Verify EFI time and entropy protocols supplied by the emulated platform.
.text
.global efi_main
efi_main:
    stp x29, x30, [sp, #-160]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    ldr x19, [x1, #64]  // ConOut
    ldr x20, [x1, #96]  // BootServices
    ldr x2, [x1, #88]   // RuntimeServices
    ldr x2, [x2, #24]   // GetTime
    add x0, sp, #48
    mov x1, #0
    blr x2
    cbnz x0, failed
    ldrh w2, [sp, #48]  // Year
    cmp w2, #2026
    b.lo failed
    cmp w2, #2100
    b.hs failed
    ldrb w2, [sp, #50]  // Month
    cmp w2, #1
    b.lo failed
    cmp w2, #12
    b.hi failed
    adr x0, rng_guid
    mov x1, #0
    add x2, sp, #64
    ldr x3, [x20, #320] // LocateProtocol
    blr x3
    cbnz x0, failed
    ldr x21, [sp, #64]
    mov x0, x21
    mov x1, #0
    mov x2, #32
    add x3, sp, #80
    ldr x4, [x21, #8]   // GetRNG
    blr x4
    cbnz x0, failed
    mov x0, x21
    mov x1, #0
    mov x2, #32
    add x3, sp, #112
    ldr x4, [x21, #8]
    blr x4
    cbnz x0, failed
    add x2, sp, #80
    add x3, sp, #112
    mov x4, #32
    mov w5, #0
compare:
    ldrb w6, [x2], #1
    ldrb w7, [x3], #1
    eor w6, w6, w7
    orr w5, w5, w6
    subs x4, x4, #1
    b.ne compare
    cbz w5, failed
    adr x1, success_message
    b print
failed:
    adr x1, failure_message
print:
    mov x0, x19
    ldr x2, [x0, #8]
    blr x2
    movz x0, #0x0008
    movk x0, #0x8400, lsl #16
    hvc #0             // PSCI SYSTEM_OFF
    b .
.balign 8
rng_guid:
    .word 0x3152bca5
    .hword 0xeade, 0x433d
    .byte 0x86,0x2e,0xc0,0x1c,0xdc,0x29,0x1f,0x44
success_message:
    .hword 80,67,69,77,85,95,84,73,77,69,95,82,78,71,95,79,75,13,10,0
failure_message:
    .hword 80,67,69,77,85,95,84,73,77,69,95,82,78,71,95,70,65,73,76,13,10,0
