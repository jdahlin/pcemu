bits 16
org 100h
    push cs
    pop es
    mov di, info
    mov dword [info], 'VBE2'
    mov ax, 4f00h
    int 10h
    mov [result], ax
    mov ax, 4f01h
    mov cx, 118h
    mov di, modeinfo
    int 10h
    mov [result+2], ax
    mov ah, 3ch
    xor cx,cx
    mov dx, filename
    int 21h
    jc done
    mov bx, ax
    mov ah,40h
    mov cx,finish-result
    mov dx,result
    int 21h
    mov ah,3eh
    int 21h
done:
    mov ax,4c00h
    int 21h
filename db 'C:\PCEMU\VBE.BIN',0
result dw 0,0
info times 512 db 0
modeinfo times 256 db 0
finish:
