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
    ldr x8, =0x001a000000000062
    cmp x6, x8
    b.ne failed
    cmp x7, #256 // ARM64 feature bit 72, not x64 CPUID EDX:10.
    b.ne failed
    ldr w0, =0x46000001
    ldr x1, =0x100010050
    mov x2, #-1
    ldr w3, =0xfffffffe
    ldr w4, =0x90006
    mov x5, #0
    hvc #0
    mov x8, #1
    lsl x8, x8, #32
    cmp x0, x8
    b.ne failed
    ldr w8, =0x31237648
    cmp x6, x8
    b.ne failed
    cbnz x7, failed
    // Read the shared clock before identity, in both fast ABIs.
    ldr w0, =0x46000001
    ldr x1, =0x100010050
    mov x2, #-1
    ldr w3, =0xfffffffe
    ldr w4, =0x90004
    mov x5, #0
    hvc #0
    mov x8, #1
    lsl x8, x8, #32
    cmp x0, x8
    b.ne failed
    cbnz x7, failed
    mov x23, x6
    mov x0, #10000 // EFI Stall: at least 10 ms, must advance by 100,000 ticks.
    ldr x8, [x20, #248]
    blr x8
    ldr x0, =0x100010050
    mov x1, #-1
    ldr w2, =0xfffffffe
    ldr w3, =0x90004
    mov x4, #0
    hvc #1
    mov x8, #1
    lsl x8, x8, #32
    cmp x0, x8
    b.ne failed
    cbnz x16, failed
    sub x8, x15, x23
    ldr x9, =100000
    cmp x8, x9
    b.lo failed
    ldr x9, =10000000 // Generous upper bound: detects nanoseconds used as ticks.
    cmp x8, x9
    b.hs failed
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
    mov x1, #1 // EfiLoaderCode: this fixture also executes self-modified code.
    mov x2, #2
    add x3, sp, #48
    ldr x8, [x20, #40] // AllocatePages
    blr x8
    cbnz x0, failed
    ldr x21, [sp, #48]
    // No-input extended-capability query: use an invalid ignored input GPA.
    // Output is exactly eight bytes; preserve the surrounding sentinel.
    mov x8, #-1
    str x8, [x21, #256]
    str x8, [x21, #264]
    ldr w0, =0x46000001
    mov x1, #0x8001
    mov x2, #-1
    add x3, x21, #256
    hvc #0
    cbnz x0, failed
    ldr x8, [x21, #256]
    cbnz x8, failed
    ldr x8, [x21, #264]
    cmn x8, #1
    b.ne failed
    ldr x0, =0x18001
    mov x15, #-1
    mov x16, #-1
    hvc #1
    cbnz x0, failed
    cbnz x15, failed
    cbnz x16, failed
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
    // Register the second allocated page; host must clear its pending state.
    ldr w0, =0x46000001
    ldr x1, =0x100010051
    mov x2, #-1
    ldr w3, =0xfffffffe
    ldr w4, =0x90015
    mov x5, #0
    add x6, x21, #4096
    orr x6, x6, #1
    mov x7, #0
    hvc #0
    mov x8, #1
    lsl x8, x8, #32
    cmp x0, x8
    b.ne failed
    ldr x8, [x21, #4096]
    cbnz x8, failed
    ldr w0, =0x46000001
    ldr x1, =0x100010050
    mov x2, #-1
    ldr w3, =0xfffffffe
    ldr w4, =0x90015
    mov x5, #0
    hvc #0
    mov x8, #1
    lsl x8, x8, #32
    cmp x0, x8
    b.ne failed
    add x8, x21, #4096
    orr x8, x8, #1
    cmp x6, x8
    b.ne failed
    mrs x22, vbar_el1
    ldr w0, =0x46000001
    ldr x1, =0x100010051
    mov x2, #-1
    ldr w3, =0xfffffffe
    ldr w4, =0x90016
    mov x5, #0
    mov x6, #1
    mov x7, #0
    hvc #0
    mov x8, #1
    lsl x8, x8, #32
    cmp x0, x8
    b.ne failed
    // Architectural maintenance remains a native ARM operation.
    dsb ishst
    tlbi vmalle1is
    dsb ish
    isb
    ldr w0, =0x46000001
    ldr x1, =0x100010051
    mov x2, #-1
    ldr w3, =0xfffffffe
    ldr w4, =0x40400
    mov x5, #0
    mov x6, x21
    mov x7, #0
    hvc #0
    mov x8, #1
    lsl x8, x8, #32
    cmp x0, x8
    b.ne failed
    mrs x8, vbar_el1
    cmp x8, x22
    b.ne failed
    ldr x0, =0x100010050
    mov x1, #-1
    ldr w2, =0xfffffffe
    ldr w3, =0x40400
    mov x4, #0
    hvc #1
    mov x8, #1
    lsl x8, x8, #32
    cmp x0, x8
    b.ne failed
    cmp x15, x21
    b.ne failed
    // The platform must execute a real instruction barrier while preserving
    // the firmware's enabled MMU and vector base.
    mrs x25, sctlr_el1
    tbz x25, #0, failed
    add x24, x21, #1536
    ldr w8, =0xd2800029 // mov x9,#1
    str w8, [x24]
    ldr w8, =0xd65f03c0 // ret
    str w8, [x24, #4]
    dc cvau, x24
    dsb ish
    ic ivau, x24
    dsb ish
    isb
    blr x24
    cmp x9, #1
    b.ne failed
    ldr w8, =0xd2800049 // mov x9,#2
    str w8, [x24]
    dc cvau, x24
    dsb ish
    ic ivau, x24
    dsb ish
    ldr x0, =0x10019
    mov x1, #1
    mov x2, #0
    hvc #1
    cbnz x0, failed
    mrs x8, sctlr_el1
    cmp x8, x25
    b.ne failed
    mrs x8, vbar_el1
    cmp x8, x22
    b.ne failed
    blr x24
    cmp x9, #2
    b.ne failed
    // Notify a synthetic crash without stopping the guest. Capture a RAM
    // message now; host diagnostics must survive subsequent guest mutation.
    add x23, x21, #512
    ldr w8, =0x4d454350 // "PCEM"
    str w8, [x23]
    ldr w0, =0x46000001
    ldr x1, =0x100010050
    mov x2, #-1
    ldr w3, =0xfffffffe
    mov x4, #0x215
    mov x5, #0
    hvc #0
    mov x8, #1
    lsl x8, x8, #32
    cmp x0, x8
    b.ne failed
    mov x8, #0xc000000000000000
    cmp x6, x8
    b.ne failed
    cbnz x7, failed
    mov x4, #0x210
    mov x6, #0x42
    bl crash_write
    mov x4, #0x213
    mov x6, x23
    bl crash_write
    mov x4, #0x214
    mov x6, #4
    bl crash_write
    mov x4, #0x215
    mov x6, #0xc000000000000000
    bl crash_write
    str wzr, [x23]
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
crash_write:
    ldr w0, =0x46000001
    ldr x1, =0x100010051
    mov x2, #-1
    ldr w3, =0xfffffffe
    mov x5, #0
    mov x7, #0
    hvc #0
    mov x8, #1
    lsl x8, x8, #32
    cmp x0, x8
    b.ne failed
    ret
.balign 8
success_message:
    .hword 80,67,69,77,85,95,72,89,80,69,82,86,95,80,82,79,66,69,95,79,75,13,10,0
disabled_message:
    .hword 80,67,69,77,85,95,72,89,80,69,82,86,95,68,73,83,65,66,76,69,68,13,10,0
failure_message:
    .hword 80,67,69,77,85,95,72,89,80,69,82,86,95,80,82,79,66,69,95,70,65,73,76,13,10,0
