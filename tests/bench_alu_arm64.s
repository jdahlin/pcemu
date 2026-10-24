.text
.globl _alu_kernel
.p2align 4
_alu_kernel:
    mov w1, w0
    mov w0, #0
Lalu_loop:
.rept 48
    add w0, w0, #1
.endr
    cmp w0, w1
    b.ne Lalu_loop
    ret
