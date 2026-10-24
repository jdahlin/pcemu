bits 32
section .entry
extern bench_entry
global _start
_start:
    call bench_entry
    ret
section .note.GNU-stack noalloc noexec nowrite progbits
