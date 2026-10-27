; SPDX-License-Identifier: GPL-2.0-only
; NASM port of DOSEMU2 test/func_memory_hma.py:
; memory_hma_freespace and memory_hma_alloc, revision
; 7e7f3da9596ce8035274843f61dc6c71e9733d3a (DOSEMU2 contributors).
; Original source SHA256:
; 6eab4ba82b32d824afaa8b531824974b875237af4c82592b8b5ac48bbd3065f7
; See tests/vendor/dosemu2/COPYING. Uses the same DOS multiplex calls and
; availability checks. FreeDOS kernel 2043's INT2F/4A02 does not round BX:
; kernel/inthndlr.c preserves 35 and advances firstAvailableBuf by 35. Assert
; this FreeDOS contract and query the new offset rather than falsely treating
; the DOSEMU/MS-DOS 48-byte expectation as an emulator failure.
; No dosemu-specific host API or C runtime is needed.
bits 16
org 100h
    mov ax,4a01h
    int 2fh
    test bx,bx
    jz fail
    mov ax,es
    cmp ax,0ffffh
    jne fail
    cmp di,0ffffh
    je fail
    mov [free_before],bx
    mov dx,available
    call print
    mov ax,4a02h
    mov bx,35
    int 2fh
    mov [actual_bx],bx
    mov [actual_di],di
    mov [actual_es],es
    cmp bx,35
    jne fail
    mov ax,es
    cmp ax,0ffffh
    jne fail
    cmp di,0ffffh
    je fail
    mov ax,4a01h
    int 2fh
    mov ax,[actual_di]
    add ax,35
    cmp di,ax
    jne fail
    mov ax,[free_before]
    sub ax,35
    cmp bx,ax
    jne fail
    mov dx,allocated
    call print
    mov dx,complete
    call print
    mov ax,4c00h
    int 21h
print:
    mov ah,9
    int 21h
    ret
fail:
    mov dx,failed
    call print
    mov ax,[actual_bx]
    call hex16
    mov dx,es_label
    call print
    mov ax,[actual_es]
    call hex16
    mov dx,di_label
    call print
    mov ax,[actual_di]
    call hex16
    mov dx,newline
    call print
    mov ax,4c01h
    int 21h
available db 'PASS HMA-AVAILABLE',13,10,'$'
allocated db 'PASS HMA-ALLOC-35-FREEDOS',13,10,'$'
complete db 'PASS PROBE-COMPLETE',13,10,'$'
hex16:
    mov cx,4
.digit:
    rol ax,4
    push ax
    mov dl,al
    and dl,15
    add dl,'0'
    cmp dl,'9'
    jbe .emit
    add dl,7
.emit:
    mov ah,2
    int 21h
    pop ax
    loop .digit
    ret
free_before dw 0
actual_bx dw 0
actual_es dw 0
actual_di dw 0
failed db 'FAIL DOSEMU-HMA BX=','$'
es_label db ' ES=','$'
di_label db ' DI=','$'
newline db 13,10,'$'
