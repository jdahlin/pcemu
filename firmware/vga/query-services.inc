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
    mov word [in_bx],8
    cmp byte [DCC_INDEX],1
    jne .done
    mov word [in_bx],7
.done:
    mov byte [in_ax],0x1a
    jmp return_interrupt
.set:
    test bh,bh
    jnz return_interrupt
    cmp bl,8
    je .color
    cmp bl,7
    jne return_interrupt
    mov byte [DCC_INDEX],1
    jmp .done
.color:
    mov byte [DCC_INDEX],0
    jmp .done

; Return the standard 64-byte functionality/state block and a ROM pointer to
; immutable capabilities. Only documented fields are populated; reserved bytes
; are zero. IBM BIOS Interface, printed 2-40..43, PDF 55..58.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=55
functionality_information:
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
    mov byte [es:di+0x25],8
    cmp byte [DCC_INDEX],1
    jne .display
    mov byte [es:di+0x25],7
.display:
    mov word [es:di+0x27],16
    mov ax,0x8000
    xor dx,dx
    div word [PAGE_BYTES]
    cmp ax,8
    jbe .pages
    mov ax,8
.pages:
    mov [es:di+0x29],al
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
    db 0x8f,0,0,0,0,0,0,7,8,2,0xfe,0x0c,0,0,0,0
video_save_table:
    dw video_parameters
    dw 0xc000
    times 6 dd 0
; VGA 64-byte parameter record, in the standard columns/rows/height/page-size,
; sequencer/misc/CRTC/attribute/graphics layout. The full mode table replaces
; this initial record when the graphics mode set is added.
video_parameters:
    db 80,24,16
    dw 4096
    db 0,3,0,2,0x67
    db 0x5f,0x4f,0x50,0x82,0x55,0x81,0xbf,0x1f,0,0x4f,0x0d,0x0e,0,0,0,0
    db 0x9c,0x8e,0x8f,0x28,0x1f,0x96,0xb9,0xa3,0xff
    db 0,1,2,3,4,5,20,7,56,57,58,59,60,61,62,63,0x0c,0,0x0f,8
    db 0,0,0,0,0,0x10,0x0e,0,0xff
