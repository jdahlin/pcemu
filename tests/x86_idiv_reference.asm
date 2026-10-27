; RAM-only ROM reproducer for the remaining 386EX byte-IDIV discrepancy.
; Intel 80386 PRM 3.2.4 and IDIV: quotient outside -128..127 raises #DE.
; https://pdos.csail.mit.edu/6.828/2018/readings/i386/IDIV.htm
; The two overflow inputs are hardware vectors F6.7#469 and F6.7#516.
; The final case checks that the valid -128 quotient is accepted.
bits 16
org 0
start:
    cli
    xor ax,ax
    mov ds,ax
    mov ss,ax
    mov sp,0x8000
    mov word [0],divide_error
    mov word [2],0xf000
    mov byte [0x200],0
    mov ax,0x648c
    mov dl,0xb7
    idiv dl
    cmp ax,0x648c
    jne fail
    cmp byte [0x200],1
    jne fail
    mov byte [0x200],0
    mov ax,0x9c71
    mov dl,0x47
    idiv dl
    cmp ax,0x9c71
    jne fail
    cmp byte [0x200],1
    jne fail
    mov byte [0x200],0
    mov ax,0xff80
    mov dl,1
    idiv dl
    cmp ax,0x0080
    jne fail
    cmp byte [0x200],0
    jne fail
    mov al,'P'
    out 0xe9,al
    mov dx,0x190
    mov al,0xff
    out dx,al
    hlt
fail:
    mov al,'F'
    out 0xe9,al
    mov dx,0x190
    mov al,0xfe
    out dx,al
    hlt
divide_error:
    push bp
    mov bp,sp
    inc byte [0x200]
    add word [ss:bp+2],2 ; Skip the two-byte faulting IDIV; saved AX remains intact.
    pop bp
    iret
times 0xfff0-($-$$) db 0
jmp 0xf000:start
times 65536-($-$$) db 0
