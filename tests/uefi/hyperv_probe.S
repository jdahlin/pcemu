// Original ARM64 guest probe: both Hyper-V ABIs, memory I/O and PSCI coexistence.
.text
.global efi_main
efi_main:
    stp x29, x30, [sp, #-80]!
    stp x19, x20, [sp, #16]
    ldr x19, [x1, #64] // EFI console
    ldr x20, [x1, #96] // BootServices
    ldr w0, =0x8600ff01
    hvc #0
    cmn x0, #1
    b.eq disabled
    ldr w8, =0x4d32ba58
    cmp x0, x8
    b.ne failed
    ldr w8, =0xcd244764
    cmp x1, x8
    b.ne failed
    ldr w8, =0x8eef6c75
    cmp x2, x8
    b.ne failed
    ldr w8, =0x16597024
    cmp x3, x8
    b.ne failed
    ldr w0, =0x80000000
    hvc #0
    ldr w8, =0x10002
    cmp x0, x8
    b.ne failed
    ldr w0, =0x46000001
    ldr x1, =0x100010050
    mov x2, #-1
    ldr w3, =0xfffffffe
    mov x4, #0x200
    mov x5, #0
    hvc #0
    mov x8, #1
    lsl x8, x8, #32
    cmp x0, x8
    b.ne failed
    ldr x8, =0x000a000000000060
    cmp x6, x8
    b.ne failed
    cbnz x7, failed
    ldr w0, =0x46000001
    ldr x1, =0x100010051
    mov x2, #-1
    ldr w3, =0xfffffffe
    ldr w4, =0x90002
    mov x5, #0
    ldr x6, =0x5043454d55 // Synthetic identity, distinct from Windows
    mov x7, #0
    hvc #0
    mov x8, #1
    lsl x8, x8, #32
    cmp x0, x8
    b.ne failed
    ldr x0, =0x100010050
    mov x1, #-1
    ldr w2, =0xfffffffe
    ldr w3, =0x90003
    mov x4, #0
    hvc #1
    mov x8, #1
    lsl x8, x8, #32
    cmp x0, x8
    b.ne failed
    cbnz x15, failed
    cbnz x16, failed
    // Allocate two physical pages; buffers must not straddle a 4 KiB page.
    mov x0, #0
    mov x1, #2
    mov x2, #2
    add x3, sp, #48
    ldr x8, [x20, #40] // AllocatePages
    blr x8
    cbnz x0, failed
    ldr x21, [sp, #48]
    mov x8, #-1
    str x8, [x21]
    ldr w8, =0xfffffffe
    str x8, [x21, #8]
    ldr w8, =0x90002
    str x8, [x21, #16]
    ldr w0, =0x46000001
    ldr x1, =0x100000050
    mov x2, x21
    add x3, x21, #4096
    hvc #0
    mov x8, #1
    lsl x8, x8, #32
    cmp x0, x8
    b.ne failed
    ldr x8, [x21, #4096]
    ldr x9, =0x5043454d55
    cmp x8, x9
    b.ne failed
    ldr w0, =0x46000001
    mov x1, #0xdead
    hvc #0
    cmp x0, #2
    b.ne failed
    ldr w0, =0x84000000
    hvc #0
    cmp x0, #2
    b.ne failed
    adr x1, success_message
    b finish
disabled:
    adr x1, disabled_message
    b finish
failed:
    adr x1, failure_message
finish:
    mov x0, x19
    ldr x2, [x0, #8]
    blr x2
    ldr w0, =0x84000008
    hvc #0
    b .
.balign 8
success_message:
    .hword 80,67,69,77,85,95,72,89,80,69,82,86,95,80,82,79,66,69,95,79,75,13,10,0
disabled_message:
    .hword 80,67,69,77,85,95,72,89,80,69,82,86,95,68,73,83,65,66,76,69,68,13,10,0
failure_message:
    .hword 80,67,69,77,85,95,72,89,80,69,82,86,95,80,82,79,66,69,95,70,65,73,76,13,10,0
