; Tiny QEMU-only BIOS loader for the unchanged benchmark COM. No DOS fixture.
; INT 13h extensions load LBA 1 at the COM entry's conventional segment:offset.
bits 16
org 7c00h
    cli
    xor ax,ax
    mov ds,ax
    mov ss,ax
    mov sp,7c00h
    sti
    mov si,packet
    mov ah,42h
    int 13h
    jc failed
    mov ax,1000h
    mov ds,ax
    mov es,ax
    jmp 1000h:100h
failed:
    mov al,0ffh
    out 0e7h,al
    cli
    hlt
    jmp failed
align 4
packet:
    db 16,0
    dw SECTORS
    dw 100h,1000h
    dq 1
times 510-($-$$) db 0
dw 0aa55h
