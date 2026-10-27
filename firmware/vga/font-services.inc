; Character-generator service. Font RAM has eight blocks of 256 32-byte glyph
; slots in plane 2. ROM glyphs are independent data, not copied firmware code.
; IBM BIOS Interface, AH=11h, PDF pages 39..47; IBM 42G2193 Sequencer SR03/04.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=39
font_service:
    cmp al,0x30
    je font_information
    cmp al,3
    je .select_block
    cmp al,0x20
    je .pointer_1f
    cmp al,0x21
    je .user_graphics
    cmp al,0x22
    je .rom_graphics
    cmp al,0x23
    je .rom_graphics
    cmp al,0x24
    je .rom_graphics
    cmp al,0x14
    ja return_interrupt
    ; Valid loads: 00/01/02/04 and 10/11/12/14 only.
    mov ah,al
    and ah,0x0f
    cmp ah,4
    ja return_interrupt
    cmp ah,3
    je return_interrupt
    cmp bl,8
    jae return_interrupt
    push fs
    mov si,[ss:bp]
    mov dx,[in_dx]
    mov cx,[in_cx]
    mov ax,[in_es]
    mov fs,ax
    mov ax,[in_ax]
    and al,0x0f
    test al,al
    jz .parameters
    push cs
    pop fs
    mov si,font_14
    mov bh,14
    cmp al,1
    je .rom
    mov si,font_8
    mov bh,8
    cmp al,2
    je .rom
    mov si,font_16
    mov bh,16
.rom:
    xor dx,dx
    mov cx,256
.parameters:
    test bh,bh
    jz .bad_parameters
    cmp bh,32
    ja .bad_parameters
    cmp dx,256
    jae .bad_parameters
    mov ax,256
    sub ax,dx
    cmp cx,ax
    jbe .count
    mov cx,ax
.count:
    test cx,cx
    jz .bad_parameters
    push si ; bp-40 source; bp-38 is saved FS.
    push bx ; bp-42 block/height.
    push cx ; bp-44 glyph count.
    push dx ; bp-46 initial character.
    ; Preserve both index latches and every register changed for plane access.
    mov dx,SEQUENCER
    in al,dx
    xor ah,ah
    push ax
    mov al,2
    call read_indexed_register
    push ax
    mov al,4
    call read_indexed_register
    push ax
    mov dx,GRAPHICS
    in al,dx
    xor ah,ah
    push ax
    mov si,font_graphics_indexes
    mov cx,6
.save_graphics:
    mov al,[cs:si]
    inc si
    call read_indexed_register
    push ax
    loop .save_graphics
    mov dx,SEQUENCER
    mov ax,0x0402
    out dx,ax
    mov ax,0x0604
    out dx,ax
    mov dx,GRAPHICS
    mov ax,0x0000
    out dx,ax
    inc al
    out dx,ax
    mov al,3
    out dx,ax
    mov al,5
    out dx,ax
    mov ax,0x0406
    out dx,ax
    mov ax,0xff08
    out dx,ax
    mov bx,[ss:bp-42]
    xor ax,ax
    mov al,bl
    and al,3
    shl ax,14
    mov di,ax
    mov al,bl
    and ax,4
    shl ax,11
    add di,ax
    mov ax,[ss:bp-46]
    shl ax,5
    add di,ax
    mov ax,0xa000
    mov es,ax
    mov si,[ss:bp-40]
    mov dx,[ss:bp-44]
.glyph:
    xor cx,cx
    mov cl,bh
.row:
    mov al,[fs:si]
    inc si
    stosb
    loop .row
    mov cx,32
    xor ax,ax
    mov al,bh
    sub cx,ax
    xor ax,ax
    rep stosb
    dec dx
    jnz .glyph
    mov dx,GRAPHICS
    mov cx,6
.restore_graphics:
    pop ax
    out dx,ax
    loop .restore_graphics
    pop ax
    out dx,al
    mov dx,SEQUENCER
    pop ax
    out dx,ax
    pop ax
    out dx,ax
    pop ax
    out dx,al
    mov bx,[ss:bp-42]
    add sp,8
    test byte [in_ax],0x10
    jz .loaded
    mov al,bh
    call resize_text_font
.loaded:
    pop fs
    jmp return_interrupt
.bad_parameters:
    pop fs
    jmp return_interrupt
.select_block:
    mov dx,SEQUENCER
    mov ah,bl
    mov al,3
    out dx,ax
    jmp return_interrupt
.pointer_1f:
    xor ax,ax
    mov es,ax
    mov ax,[ss:bp]
    mov [es:0x7c],ax
    mov ax,[in_es]
    mov [es:0x7e],ax
    jmp return_interrupt
.user_graphics:
    xor ax,ax
    mov es,ax
    mov ax,[ss:bp]
    mov [es:0x10c],ax
    mov ax,[in_es]
    mov [es:0x10e],ax
    mov [CHARACTER_HEIGHT],cx
    jmp .graphics_rows
.rom_graphics:
    xor ax,ax
    mov es,ax
    mov word [es:0x10e],cs
    mov ax,[in_ax]
    mov cx,14
    mov si,font_14
    cmp al,0x22
    je .graphics_pointer
    mov cx,8
    mov si,font_8
    cmp al,0x23
    je .graphics_pointer
    mov cx,16
    mov si,font_16
.graphics_pointer:
    mov [es:0x10c],si
    mov [CHARACTER_HEIGHT],cx
.graphics_rows:
    mov al,dl
    cmp bl,0
    je .rows
    mov al,14
    cmp bl,1
    je .rows
    mov al,25
    cmp bl,2
    je .rows
    mov al,43
.rows:
    test al,al
    jz return_interrupt
    dec al
    mov [ROWS_MINUS_ONE],al
    jmp return_interrupt

; Read an indexed register as AH=value, AL=index, leaving DX at index port.
; IBM 42G2193, indexed-register protocols, PDF page 65 onward.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=65
read_indexed_register:
    push bx
    mov bl,al
    out dx,al
    inc dx
    in al,dx
    dec dx
    mov ah,al
    mov al,bl
    pop bx
    ret
font_graphics_indexes db 0,1,3,5,6,8

; Recalculate text rows, page allocation and CRTC character/vertical/cursor
; fields after an AL=10h..14h font load. AL is glyph height, no memory clears.
; IBM BIOS Interface, printed 2-28, PDF page 43. Page allocation is rounded up
; to 256 bytes so hardware page bases are aligned, matching common VGA BIOSes.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=43
resize_text_font:
    xor ah,ah
    mov [CHARACTER_HEIGHT],ax
    mov bx,ax
    mov ax,400
    cmp byte [MODE],7
    je .mono
    test byte [MODESET_CONTROL],0x80
    jz .not_200
    mov ax,200
    jmp .scanlines
.not_200:
    test byte [MODESET_CONTROL],0x10
    jnz .scanlines
.mono:
    mov ax,350
.scanlines:
    xor dx,dx
    div bx
    mov cx,ax
    mov ax,0x4000
    xor dx,dx
    div word [COLUMNS]
    cmp ax,256
    jbe .max_rows
    mov ax,256
.max_rows:
    cmp cx,ax
    jbe .rows_fit
    mov cx,ax
.rows_fit:
    mov ax,cx
    dec ax
    mov [ROWS_MINUS_ONE],al
    inc ax
    push ax
    mul bx
    dec ax
    mov cx,ax
    mov dx,[CRTC_BASE]
    mov al,0x12
    mov ah,cl
    out dx,ax
    mov al,7
    call read_indexed_register
    and ah,0xbd ; retain all overflow bits except Vertical Display End 8/9.
    test ch,1
    jz .bit9
    or ah,2
.bit9:
    test ch,2
    jz .overflow
    or ah,0x40
.overflow:
    out dx,ax
    mov al,9
    call read_indexed_register
    and ah,0xe0
    mov bl,[CHARACTER_HEIGHT]
    dec bl
    or ah,bl
    out dx,ax
    mov ah,bl
    mov al,0x0b
    out dx,ax
    mov cl,bl
    dec ah
    mov ch,ah
    mov al,0x0a
    out dx,ax
    mov [CURSOR_SHAPE],cx
    pop ax
    mul word [COLUMNS]
    shl ax,1
    add ax,255
    and ax,0xff00
    mov [PAGE_BYTES],ax
    mov ax,0x0500
    int 0x10
    ret

; Return ROM and interrupt-vector font pointers. CX is current glyph height;
; DL is current row count minus one. Alternate 9-dot tables are empty patch
; lists (FFh terminator), which means no glyph overrides are necessary.
; IBM BIOS Interface, printed 2-26 and 2-32, PDF pages 41 and 47.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=41
font_information:
    cmp bh,7
    ja return_interrupt
    cmp bh,0
    je .vector_1f
    cmp bh,1
    je .vector_43
    mov ax,cs
    mov [in_es],ax
    mov ax,font_14
    cmp bh,2
    je .pointer
    mov ax,font_8
    cmp bh,3
    je .pointer
    add ax,1024
    cmp bh,4
    je .pointer
    mov ax,font_16
    cmp bh,6
    je .pointer
    mov ax,font_alternate
    jmp .pointer
.vector_1f:
    mov si,0x7c
    jmp .vector
.vector_43:
    mov si,0x10c
.vector:
    xor ax,ax
    mov es,ax
    mov ax,[es:si+2]
    mov [in_es],ax
    mov ax,[es:si]
.pointer:
    mov [ss:bp],ax
    mov ax,[CHARACTER_HEIGHT]
    mov [in_cx],ax
    mov al,[ROWS_MINUS_ONE]
    mov [in_dx],al
    jmp return_interrupt
font_alternate db 0xff
