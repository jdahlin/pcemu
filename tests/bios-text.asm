bits 16
org 0x7c00
xor ax,ax
mov ds,ax
mov ss,ax
mov sp,0x7c00
mov ax,0x0e41
mov bx,0x0007
int 0x10
mov al,'!'
out 0xe9,al
hlt
times 510-($-$$) db 0
dw 0xaa55
