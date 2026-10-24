// Original position-independent AArch64 UEFI application. EFI entry receives
// ImageHandle in X0 and EFI_SYSTEM_TABLE in X1. ConOut is at offset 64;
// EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL.OutputString is at offset 8.
.text
.global efi_main
efi_main:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    ldr x0, [x1, #64]
    ldr x2, [x0, #8]
    adr x1, 1f
    blr x2
    mov x0, #0
    ldp x29, x30, [sp], #16
    ret
.balign 2
1:
    .hword 13,10,80,67,69,77,85,95,69,70,73,95,66,79,79,84,95,79,75,13,10,0
