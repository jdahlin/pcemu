.text
.globl _memory_kernel
.p2align 4
_memory_kernel:
Lmemory_loop:
    ldr w2, [x0, #0]
    add w2, w2, #1
    str w2, [x0, #0]
    ldr w2, [x0, #4]
    add w2, w2, #1
    str w2, [x0, #4]
    ldr w2, [x0, #8]
    add w2, w2, #1
    str w2, [x0, #8]
    ldr w2, [x0, #12]
    add w2, w2, #1
    str w2, [x0, #12]
    ldr w2, [x0, #16]
    add w2, w2, #1
    str w2, [x0, #16]
    ldr w2, [x0, #20]
    add w2, w2, #1
    str w2, [x0, #20]
    ldr w2, [x0, #24]
    add w2, w2, #1
    str w2, [x0, #24]
    ldr w2, [x0, #28]
    add w2, w2, #1
    str w2, [x0, #28]
    ldr w2, [x0, #32]
    add w2, w2, #1
    str w2, [x0, #32]
    ldr w2, [x0, #36]
    add w2, w2, #1
    str w2, [x0, #36]
    ldr w2, [x0, #40]
    add w2, w2, #1
    str w2, [x0, #40]
    ldr w2, [x0, #44]
    add w2, w2, #1
    str w2, [x0, #44]
    ldr w2, [x0, #48]
    add w2, w2, #1
    str w2, [x0, #48]
    ldr w2, [x0, #52]
    add w2, w2, #1
    str w2, [x0, #52]
    ldr w2, [x0, #56]
    add w2, w2, #1
    str w2, [x0, #56]
    ldr w2, [x0, #60]
    add w2, w2, #1
    str w2, [x0, #60]
    subs w1, w1, #1
    b.ne Lmemory_loop
    ret
