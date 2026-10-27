; AH=12h controls mode-set palette loading, grayscale and cursor emulation,
; selects 200/350/400 text scan lines, and controls display/address enabling.
; IBM BIOS Interface, printed 2-32..36, PDF 47..51. BDA flag encoding follows
; the established VGA BIOS convention (MODESET_CONTROL at 40:89).
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=47
alternate_select:
    cmp bl,0x10
    je .information
    cmp bl,0x30
    je .scanlines
    cmp bl,0x31
    je .palette
    cmp bl,0x32
    je .addressing
    cmp bl,0x33
    je .gray
    cmp bl,0x34
    je .cursor
    cmp bl,0x36
    je .screen
    jmp return_interrupt
.information:
    mov word [in_bx],3 ; color VGA, 256 KiB plane memory.
    cmp word [CRTC_BASE],0x3b4
    jne .info_color
    mov byte [in_bx+1],1
.info_color:
    mov word [in_cx],9
    jmp return_interrupt
.scanlines:
    cmp al,2
    ja return_interrupt
    and byte [MODESET_CONTROL],0x6f
    and byte [VIDEO_SWITCHES],0xf0
    or byte [VIDEO_SWITCHES],9
    cmp al,1
    je .success
    cmp al,2
    je .four_hundred
    or byte [MODESET_CONTROL],0x80
    and byte [VIDEO_SWITCHES],0xfe
    jmp .success
.four_hundred:
    or byte [MODESET_CONTROL],0x10
    jmp .success
.palette:
    cmp al,1
    ja return_interrupt
    and byte [MODESET_CONTROL],0xf7
    shl al,3
    or [MODESET_CONTROL],al
    jmp .success
.gray:
    cmp al,1
    ja return_interrupt
    xor al,1
    shl al,1
    and byte [MODESET_CONTROL],0xfd
    or [MODESET_CONTROL],al
    jmp .success
.cursor:
    cmp al,1
    ja return_interrupt
    xor al,1
    and byte [MODESET_CONTROL],0xfe
    or [MODESET_CONTROL],al
    jmp .success
.addressing:
    cmp al,1
    ja return_interrupt
    mov bl,al
    mov dx,0x3cc
    in al,dx
    and al,0xfd
    xor bl,1
    shl bl,1
    or al,bl
    mov dx,MISC_OUTPUT
    out dx,al
    jmp .success
.screen:
    cmp al,1
    ja return_interrupt
    mov bl,al
    mov dx,SEQUENCER
    mov al,1
    call read_indexed_register
    and ah,0xdf
    shl bl,5
    or ah,bl
    out dx,ax
.success:
    mov byte [in_ax],0x12
    jmp return_interrupt

; DCC describes our single attached display. Accept the two actual display
; combinations (color VGA or analog monochrome) and reject nonexistent pairs.
; IBM BIOS Interface, printed 2-39, PDF 54.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=54
display_information:
    cmp al,1
    je .set
    test al,al
    jnz return_interrupt
    xor bx,bx
    mov bl,[DCC_INDEX]
    and bl,0x7f
    cmp bl,16
    jae return_interrupt
    shl bx,1
    mov bx,[cs:dcc_entries+bx]
    test byte [DCC_INDEX],0x80
    jz .output
    xchg bl,bh
.output:
    mov [in_bx],bx
.success:
    mov byte [in_ax],0x1a
    jmp return_interrupt
.set:
    xor si,si
.next:
    mov ax,[cs:dcc_entries+si]
    cmp ax,bx
    je .found
    add si,2
    cmp si,32
    jb .next
    xchg bl,bh
    xor si,si
.reverse:
    mov ax,[cs:dcc_entries+si]
    cmp ax,bx
    je .reversed
    add si,2
    cmp si,32
    jb .reverse
    jmp return_interrupt
.reversed:
    shr si,1
    mov ax,si
    or al,0x80
    mov [DCC_INDEX],al
    jmp .success
.found:
    shr si,1
    mov ax,si
    mov [DCC_INDEX],al
    jmp .success

; Return the standard 64-byte functionality/state block and a ROM pointer to
; immutable capabilities. Only documented fields are populated; reserved bytes
; are zero. IBM BIOS Interface, printed 2-40..43, PDF 55..58.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=55
functionality_information:
    cmp byte [MODE],0xff
    je return_interrupt
    test bx,bx
    jnz return_interrupt
    cmp di,0xffc0
    ja return_interrupt
    push di
    mov cx,32
    xor ax,ax
    rep stosw
    pop di
    mov word [es:di],static_functionality
    mov [es:di+2],cs
    push di
    add di,4
    mov si,MODE
    mov cx,30
    rep movsb
    mov si,ROWS_MINUS_ONE
    mov cx,3
    rep movsb
    pop di
    inc byte [es:di+0x22]
    mov ax,0x1a00
    int 0x10
    mov [es:di+0x25],bx
    mov word [es:di+0x27],16
    mov dx,0
    mov ax,0x8000
    cmp byte [MODE],7
    je .page_count
    cmp byte [MODE],4
    jb .page_count
    cmp byte [MODE],6
    jbe .cga
    mov word [es:di+0x27],2
    cmp byte [MODE],0x0f
    je .vga_pages
    cmp byte [MODE],0x11
    je .vga_pages
    mov word [es:di+0x27],256
    cmp byte [MODE],0x13
    je .one_page
    mov word [es:di+0x27],16
.vga_pages:
    xor ax,ax
    mov dx,1
    jmp .page_count
.cga:
    mov word [es:di+0x27],4
    cmp byte [MODE],6
    jne .one_page
    mov word [es:di+0x27],2
.one_page:
    mov ax,1
    jmp .pages
.page_count:
    div word [PAGE_BYTES]
    cmp ax,8
    jbe .pages
    mov ax,8
.pages:
    mov [es:di+0x29],al
    cmp byte [MODE],7
    je .scan_350
    cmp byte [MODE],4
    jb .text_scanlines
    cmp byte [MODE],0x11
    je .scan_480
    cmp byte [MODE],0x12
    je .scan_480
    cmp byte [MODE],0x0f
    je .scan_350
    cmp byte [MODE],0x10
    je .scan_350
    xor al,al
    jmp .scanlines
.scan_480:
    mov al,3
    jmp .scanlines
.scan_350:
    mov al,1
    jmp .scanlines
.text_scanlines:
    mov al,2
    test byte [MODESET_CONTROL],0x10
    jnz .scanlines
    mov al,1
    test byte [MODESET_CONTROL],0x80
    jz .scanlines
    xor al,al
.scanlines:
    mov [es:di+0x2a],al
    mov dx,SEQUENCER
    mov al,3
    call read_indexed_register
    mov al,ah
    and al,3
    mov cl,ah
    and cl,0x10
    shr cl,2
    or al,cl
    mov [es:di+0x2b],al
    mov al,ah
    shr al,2
    and al,3
    and ah,0x20
    shr ah,3
    or al,ah
    mov [es:di+0x2c],al
    mov al,[MODESET_CONTROL]
    and al,0x0a
    test byte [MODESET_CONTROL],1
    jz .no_cursor
    or al,0x10
.no_cursor:
    push ax
    mov al,0x10
    call read_attribute
    test ah,8
    pop ax
    jz .no_blink
    or al,0x20
.no_blink:
    mov [es:di+0x2d],al
    mov byte [es:di+0x31],3
    mov byte [in_ax],0x1b
    jmp return_interrupt

; Static functionality flags reflect the implemented service families. Mode
; flags are expanded with graphics support; no reserved feature bits are set.
; IBM BIOS Interface static functionality table, PDF 57..58.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=57
static_functionality:
    db 0xff,0xe0,0x0f,0,0,0,0,7,8,2,0xfe,0x0e,0,0,0,0
video_save_table:
    dw video_parameters,0xc000
    dd 0,0,0
    dw secondary_save_table,0xc000
    dd 0,0
; Both required pointers in the primary/secondary save-pointer tables are
; valid ROM far pointers. Optional dynamic/font/palette overrides default zero.
; IBM BIOS Interface, printed 3-12..15, PDF 149..152.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=149
secondary_save_table:
    dw 26
    dw dcc_table,0xc000
    dd 0,0,0,0,0
dcc_table:
    db 16,1,0x0c,0
dcc_entries:
    dw 0x0000,0x0001,0x0002,0x0201,0x0004,0x0401,0x0005,0x0205
    dw 0x0006,0x0106,0x0506,0x0008,0x0108,0x0007,0x0207,0x0206

; Legacy INT 1Dh exposes four sixteen-byte 6845 parameter sets (40/80-column
; text, 320/640-pixel CGA graphics). The VGA's separate 64-byte records are
; reached through the save-pointer table, not this legacy interrupt vector.
; IBM BIOS Interface INT 1Dh/ROM parameter conventions; CGA compatibility data.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=149
cga_parameters:
    db 0x38,0x28,0x2d,0x0a,0x1f,6,0x19,0x1c,2,7,6,7,0,0,0,0
    db 0x71,0x50,0x5a,0x0a,0x1f,6,0x19,0x1c,2,7,6,7,0,0,0,0
    db 0x38,0x28,0x2d,0x0a,0x7f,6,0x64,0x70,2,1,6,7,0,0,0,0
    db 0x71,0x50,0x5a,0x0a,0x7f,6,0x64,0x70,2,1,6,7,0,0,0,0
