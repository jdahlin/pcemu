; RAM-only boot-sector integration probe. Runs unchanged under PCEmu and QEMU;
; it checks the installed vector, real INT 10h calls and VGA text memory.
bits 16
org 0x7c00
start:
    cli
    xor ax,ax
    mov ss,ax
    mov sp,0x7c00
    mov ds,ax
    cld
    mov bp,1
    mov ax,[0x42]
    cmp ax,0xc000
    jb fail
    cmp ax,0xc400
    jae fail
    mov ax,3
    int 0x10
    mov ax,0x0200
    xor bx,bx
    mov dx,0x0205
    int 0x10
    mov ax,0x0951
    mov bl,0x1e
    mov cx,3
    int 0x10
    mov bp,2
    mov ax,0x0800
    int 0x10
    cmp ax,0x1e51
    jne fail
    mov bp,3
    mov ax,0x0300
    int 0x10
    cmp dx,0x0205
    jne fail
    mov bp,4
    mov ax,0xb800
    mov es,ax
    cmp word [es:2*160+5*2],0x1e51
    jne fail
    cmp word [es:2*160+7*2],0x1e51
    jne fail
    mov bp,5
    mov ax,0x12
    int 0x10
    mov ax,0x0c0b
    xor bx,bx
    mov cx,639
    mov dx,479
    int 0x10
    mov ax,0x0d00
    int 0x10
    cmp al,11
    jne fail
    mov bp,6
    mov ax,0x13
    int 0x10
    mov ax,0x0cfe
    mov cx,319
    mov dx,199
    int 0x10
    mov ax,0x0d00
    int 0x10
    cmp al,0xfe
    jne fail
    mov ax,3
    int 0x10
    mov si,pass_text
    call print
    mov al,0x10
    out 0xf4,al ; QEMU isa-debug-exit: exit status 33.
    hlt
    jmp $
fail:
    mov ax,bp
    add al,'0'
    out 0xe9,al
    mov si,fail_text
    call print
    mov al,0x11
    out 0xf4,al
    hlt
    jmp $
; Print through the tested BIOS and the emulator-independent debug console.
print:
    lodsb
    test al,al
    jz .done
    out 0xe9,al
    mov ah,0x0e
    xor bx,bx
    int 0x10
    jmp print
.done:
    ret
pass_text db 'PCEMU-VGA-PASS',0
fail_text db 'PCEMU-VGA-FAIL',0
times 510-($-$$) db 0
dw 0xaa55
