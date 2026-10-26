// Headless integration: real PCI ECAM/BAR accesses, CORB/RIRB DMA, speaker
// verbs, BDL playback, completion status, position DMA and controller reset.
.text
.global efi_main
efi_main:
    stp x29, x30, [sp, #-48]!
    stp x19, x20, [sp, #16]
    str x21, [sp, #32]
    ldr x19, [x1, #64] // EFI console
    sub sp, sp, #2048
    add x21, sp, #127
    and x21, x21, #-128 // DMA buffers in guest stack RAM, 128-byte aligned.
    mov x20, #0x3f04
    lsl x20, x20, #16 // ECAM bus 0, slot 8, function 0.
    ldr w0, [x20]
    mov w1, #0x8086
    movk w1, #0x293e, lsl #16
    cmp w0, w1
    b.ne failed
    ldr w0, [x20, #8]
    mov w1, #3
    movk w1, #0x0403, lsl #16
    cmp w0, w1
    b.ne failed
    mov w0, #-1
    str w0, [x20, #0x10]
    ldr w0, [x20, #0x10]
    mov w1, #0xc004
    movk w1, #0xffff, lsl #16
    cmp w0, w1
    b.ne failed
    mov w0, #0x20000000
    str w0, [x20, #0x10]
    str wzr, [x20, #0x14]
    mov w0, #6
    strh w0, [x20, #4]
    mov x20, #0x20000000
    str wzr, [x20, #8]
    mov w0, #1
    str w0, [x20, #8]
    ldrh w0, [x20]
    mov w1, #0x1001
    cmp w0, w1
    b.ne failed
    // Root vendor query at CORB index 1; response goes to RIRB index 1.
    mov w0, #0xf0000
    str w0, [x21, #4]
    str x21, [x20, #0x40]
    add x0, x21, #128
    str x0, [x20, #0x50]
    mov w0, #1
    strh w0, [x20, #0x5a]
    mov w0, #3
    strb w0, [x20, #0x5c]
    mov w0, #2
    strb w0, [x20, #0x4c]
    dmb sy
    mov w0, #1
    strh w0, [x20, #0x48]
    ldrh w0, [x20, #0x58]
    cmp w0, #1
    b.ne failed
    ldr w0, [x21, #136]
    mov w1, #0x11
    movk w1, #0x1af4, lsl #16
    cmp w0, w1
    b.ne failed
    // Stop CORB, assign DAC stream 1/channel 0 with the immediate interface.
    strb wzr, [x20, #0x4c]
    mov w0, #0x610
    movk w0, #0x27, lsl #16
    str w0, [x20, #0x60] // NID2, SET_CONVERTER_STREAM_CHANNEL 706h, payload10h.
    mov w0, #3
    strh w0, [x20, #0x68]
    ldrh w0, [x20, #0x68]
    cmp w0, #2
    b.ne failed
    // Two 192-byte buffers (one millisecond each), both interrupt on completion.
    add x0, x21, #512
    str x0, [x21, #256]
    add x0, x21, #768
    str x0, [x21, #272]
    mov w0, #192
    str w0, [x21, #264]
    str w0, [x21, #280]
    mov w0, #1
    str w0, [x21, #268]
    str w0, [x21, #284]
    // Repeated nonzero stereo PCM samples, to exercise the optional SDL sink.
    mov w0, #0x1000
    movk w0, #0x1000, lsl #16
    mov x1, #0
fill_samples:
    add x2, x21, #512
    str w0, [x2, x1]
    add x2, x21, #768
    str w0, [x2, x1]
    add x1, x1, #4
    cmp x1, #192
    b.lo fill_samples
    add x0, x21, #256
    str x0, [x20, #0x98]
    mov w0, #384
    str w0, [x20, #0x88]
    mov w0, #1
    strh w0, [x20, #0x8c]
    mov w0, #0x11
    strh w0, [x20, #0x92]
    add x0, x21, #1025
    str x0, [x20, #0x70]
    dmb sy
    mov w0, #0x1e
    movk w0, #0x10, lsl #16
    str w0, [x20, #0x80]
    // Finite polling: completion must appear, with no descriptor/FIFO error.
    mov x1, #0xffff
poll_completion:
    ldrb w0, [x20, #0x83]
    tst w0, #0x18
    b.ne failed
    tbnz w0, #2, completed
    subs x1, x1, #1
    b.ne poll_completion
    b failed
completed:
    // Stop before comparing RAM position against the LPIB register/alias.
    strb wzr, [x20, #0x80]
    ldr w0, [x20, #0x84]
    ldr w1, [x21, #1024]
    cmp w0, w1
    b.ne failed
    mov x2, #0x2084
    ldr w1, [x20, x2]
    cmp w0, w1
    b.ne failed
    cmp w0, #384
    b.hs failed
    tst w0, #3
    b.ne failed
    str wzr, [x20, #8]
    ldr w0, [x20, #0x84]
    cbnz w0, failed
    adr x1, success_text
    b print_and_off
failed:
    adr x1, failure_text
print_and_off:
    mov x0, x19
    ldr x2, [x19, #8]
    blr x2
    mov w0, #8
    movk w0, #0x8400, lsl #16 // PSCI SYSTEM_OFF.
    hvc #0
    b .
.balign 2
success_text:
    .hword 'P','C','E','M','U','_','H','D','A','_','O','K',13,10,0
failure_text:
    .hword 'P','C','E','M','U','_','H','D','A','_','F','A','I','L',13,10,0
