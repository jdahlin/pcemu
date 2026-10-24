.text
.globl _alu_kernel
.p2align 4
_alu_kernel:
    xorl %eax, %eax
Lalu_loop:
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax
    addl $1, %eax

    cmpl %edi, %eax
    jne Lalu_loop
    ret
