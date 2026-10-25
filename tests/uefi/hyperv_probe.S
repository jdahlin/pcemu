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
    // SyncContextEx flags + sparse HV_VP_SET, with one variable bank word.
    ldr w0, =0x46000001
    ldr x1, =0x3001a
    mov x2, #0
    mov x3, #0
    mov x4, #1
    mov x5, #1
    hvc #0
    cbnz x0, failed
    add x23, x21, #1024
    stp xzr, xzr, [x23]
    mov x8, #1
    stp x8, x8, [x23, #16]
    ldr x0, =0x2001a
    mov x1, x23
    mov x2, #-1
    hvc #1
    cbnz x0, failed
    // The original fixed input also supports memory transport.
    mov x8, #1
    str x8, [x23]
    str xzr, [x23, #8]
    ldr w0, =0x46000001
    mov x1, #0x19
    mov x2, x23
    mov x3, #-1
    hvc #0
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
    // Fixed address-space flush: all processors ignores an invalid mask.
    ldr w0, =0x46000001
    ldr x1, =0x10002
    mov x2, #0x1234
    mov x3, #7
    mov x4, #-1
    hvc #0
    cbnz x0, failed
    // Ex address-space flush through a real GPA sparse processor set.
    add x23, x21, #2048
    mrs x8, ttbr0_el1
    str x8, [x23]
    stp xzr, xzr, [x23, #8]
    mov x8, #1
    stp x8, x8, [x23, #24]
    ldr x0, =0x20013
    mov x1, x23
    mov x2, #-1
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
    // ARM-specific pending-TLB drain has no parameter buffers.
    mov x0, #0xd6
    mov x1, #-1
    mov x2, #-1
    hvc #1
    cbnz x0, failed
    // Own the interrupt configuration for this final fixture section. Keep
    // DAIF.I masked while explicitly acknowledging Group-1 SGIs through IAR.
    msr daifset, #2
    ldr x27, =0x080b0000
    ldr w8, [x27, #0x80]
    orr w8, w8, #0x20
    orr w8, w8, #0x80
    str w8, [x27, #0x80]
    mov w8, #0xa0
    str w8, [x27, #0x100]
    strb wzr, [x27, #0x405]
    strb wzr, [x27, #0x407]
    mov x28, #0x08000000
    ldr w8, [x28]
    orr w8, w8, #3
    orr w8, w8, #0x10
    str w8, [x28]
    mov x8, #1
    msr icc_sre_el1, x8
    isb
    mov x8, #0xff
    msr icc_pmr_el1, x8
    mov x8, #1
    msr icc_igrpen1_el1, x8
    mrs x8, icc_ctlr_el1
    bic x8, x8, #2
    msr icc_ctlr_el1, x8
    isb
    ldr w0, =0x46000001
    ldr x1, =0x1000b
    mov x2, #5
    mov x3, #1
    hvc #0
    cbnz x0, failed
    mrs x9, icc_iar1_el1
    cmp x9, #5
    b.ne failed
    msr icc_eoir1_el1, x9
    isb
    // Ex memory input: vector 7, VTL0, sparse VP0 set.
    add x23, x21, #2560
    mov x8, #7
    str x8, [x23]
    str xzr, [x23, #8]
    mov x8, #1
    stp x8, x8, [x23, #16]
    ldr x0, =0x20015
    mov x1, x23
    mov x2, #-1
    hvc #1
    cbnz x0, failed
    mrs x9, icc_iar1_el1
    cmp x9, #7
    b.ne failed
    msr icc_eoir1_el1, x9
    isb
    // Program an already-due direct synthetic timer through the public
    // SMCCC register ABI. The VM loop must pend SGI 9 before guest resumption.
    ldr w8, [x27, #0x80]
    orr w8, w8, #0x200
    str w8, [x27, #0x80]
    mov w8, #0x200
    str w8, [x27, #0x100]
    strb wzr, [x27, #0x409]
    ldr w4, =0xb0001
    mov x6, #1
    bl crash_write
    ldr w4, =0xb0000
    mov x6, #0x1091
    bl crash_write
    mrs x9, icc_iar1_el1
    cmp x9, #9
    b.ne failed
    msr icc_eoir1_el1, x9
    isb
    // Read back through the legacy ABI: one-shot Enabled must be cleared.
    ldr x0, =0x100010050
    mov x1, #-1
    ldr w2, =0xfffffffe
    ldr w3, =0xb0000
    mov x4, #0
    hvc #1
    mov x8, #1
    lsl x8, x8, #32
    cmp x0, x8
    b.ne failed
    mov x8, #0x1090
    cmp x15, x8
    b.ne failed
    cbnz x16, failed
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
