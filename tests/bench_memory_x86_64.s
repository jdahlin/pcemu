.text
.globl _memory_kernel
.p2align 4
_memory_kernel:
Lmemory_loop:
    movl 0(%rdi), %eax
    addl $1, %eax
    movl %eax, 0(%rdi)
    movl 4(%rdi), %eax
    addl $1, %eax
    movl %eax, 4(%rdi)
    movl 8(%rdi), %eax
    addl $1, %eax
    movl %eax, 8(%rdi)
    movl 12(%rdi), %eax
    addl $1, %eax
    movl %eax, 12(%rdi)
    movl 16(%rdi), %eax
    addl $1, %eax
    movl %eax, 16(%rdi)
    movl 20(%rdi), %eax
    addl $1, %eax
    movl %eax, 20(%rdi)
    movl 24(%rdi), %eax
    addl $1, %eax
    movl %eax, 24(%rdi)
    movl 28(%rdi), %eax
    addl $1, %eax
    movl %eax, 28(%rdi)
    movl 32(%rdi), %eax
    addl $1, %eax
    movl %eax, 32(%rdi)
    movl 36(%rdi), %eax
    addl $1, %eax
    movl %eax, 36(%rdi)
    movl 40(%rdi), %eax
    addl $1, %eax
    movl %eax, 40(%rdi)
    movl 44(%rdi), %eax
    addl $1, %eax
    movl %eax, 44(%rdi)
    movl 48(%rdi), %eax
    addl $1, %eax
    movl %eax, 48(%rdi)
    movl 52(%rdi), %eax
    addl $1, %eax
    movl %eax, 52(%rdi)
    movl 56(%rdi), %eax
    addl $1, %eax
    movl %eax, 56(%rdi)
    movl 60(%rdi), %eax
    addl $1, %eax
    movl %eax, 60(%rdi)
    decl %esi
    jne Lmemory_loop
    ret
