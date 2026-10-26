; 16-bit Windows display DDI (Pascal far calls), no imports or relocations.
; SetCursor uploads CURSORSHAPE; Windows no longer calls the DIB software cursor.
; Movement stays entirely with the host compositor. Installed only in PCEmu's
; pinned qemumini.drv by tools/stage_host_cursor.py.
bits 16
set_cursor:
    push bp
    mov bp, sp
    pushf
    pusha
    push ds
    lds si, [bp+6]
    mov dx, 0x5659
    mov ax, ds
    or ax, si
    jz .hide
    mov ax, 0xc501
    out dx, ax
    inc dx
    cld
    mov cx, 12
    mov bx, si
.header:
    lodsb
    out dx, al
    loop .header
    ; Size validated by the receiver; reject excessive/unknown shapes here too.
    mov ax, [bx+4]
    dec ax
    cmp ax, 63
    ja .hide
    mov ax, [bx+6]
    dec ax
    cmp ax, 63
    ja .hide
    mov ax, [bx+4]
    add ax, 7
    shr ax, 3
    mul word [bx+6]
    mov cx, ax
    cmp byte [bx+11], 1
    je .mono
    mov al, [bx+11]
    cmp al, 8
    je .color
    cmp al, 16
    je .color
    cmp al, 24
    je .color
    cmp al, 32
    jne .hide
.color:
    xor ah, ah
    add ax, 7
    shr ax, 3
    mul word [bx+4]
    mul word [bx+6]
    add cx, ax
    jmp .data
.mono:
    shl cx, 1
.data:
    mov dx, 0x565a
.bytes:
    lodsb
    out dx, al
    loop .bytes
    dec dx
    mov ax, 0xc502
    out dx, ax
    jmp .done
.hide:
    mov dx, 0x5659
    mov ax, 0xc500
    out dx, ax
.done:
    pop ds
    popa
    popf
    mov ax, 1
    pop bp
    retf 4
move_cursor:
    retf 4
check_cursor:
    retf
; Footer lets the staging tool redirect NE ordinals without hardcoded offsets.
dw set_cursor, move_cursor, check_cursor
