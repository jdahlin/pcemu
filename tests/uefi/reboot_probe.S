// Persist a marker through EFI variables, reset, then verify the second boot.
.text
.global efi_main
efi_main:
    stp x29, x30, [sp, #-112]!
    stp x19, x20, [sp, #16]
    ldr x19, [x1, #64]
    ldr x20, [x1, #88]
    mov x2, #8
    str x2, [sp, #48]
    adr x0, variable_name
    adr x1, vendor_guid
    mov x2, #0
    add x3, sp, #48
    add x4, sp, #64
    ldr x5, [x20, #72] // GetVariable
    blr x5
    cbz x0, second_boot
    movz x2, #14
    movk x2, #0x8000, lsl #48 // EFI_NOT_FOUND
    cmp x0, x2
    b.ne failed
    mov x2, #1
    str x2, [sp, #64]
    adr x0, variable_name
    adr x1, vendor_guid
    mov x2, #7          // NV | BS | RT
    mov x3, #8
    add x4, sp, #64
    ldr x5, [x20, #88] // SetVariable
    blr x5
    cbnz x0, failed
    adr x1, reset_message
    bl print
    mov x0, #0         // EfiResetCold
    mov x1, #0
    mov x2, #0
    mov x3, #0
    ldr x4, [x20, #104] // ResetSystem
    blr x4
    b failed
second_boot:
    ldr x2, [sp, #48]
    cmp x2, #8
    b.ne failed
    ldr x2, [sp, #64]
    cmp x2, #1
    b.ne failed
    adr x0, variable_name
    adr x1, vendor_guid
    mov x2, #0
    mov x3, #0
    mov x4, #0
    ldr x5, [x20, #88]
    blr x5
    cbnz x0, failed
    adr x1, success_message
    bl print
    b poweroff
failed:
    adr x1, failure_message
    bl print
poweroff:
    movz x0, #8
    movk x0, #0x8400, lsl #16
    hvc #0
    b .
print:
    stp x29, x30, [sp, #-16]!
    mov x0, x19
    ldr x2, [x0, #8]
    blr x2
    ldp x29, x30, [sp], #16
    ret
.balign 8
vendor_guid:
    .word 0x7841b225
    .hword 0xf63e, 0x48b1
    .byte 0xb1,0x8f,0x39,0xbc,0x87,0x59,0x21,0x43
variable_name:
    .hword 80,67,69,77,85,82,101,98,111,111,116,84,101,115,116,0
reset_message:
    .hword 80,67,69,77,85,95,82,69,83,69,84,95,82,69,81,85,69,83,84,13,10,0
success_message:
    .hword 80,67,69,77,85,95,82,69,66,79,79,84,95,79,75,13,10,0
failure_message:
    .hword 80,67,69,77,85,95,82,69,66,79,79,84,95,70,65,73,76,13,10,0
