; Select a standard VGA graphics mode from the ROM's hardware descriptor.
; Program actual ports, initialize BDA/font pointers and clear the framebuffer
; only when AL bit 7 permits it. IBM BIOS Interface, PDF 26..29; IBM 42G2193
; Type 1 Subsystem Parameters and register definitions, PDF 40 and 65 onward.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=26
set_graphics_mode:
    cmp al,0x20
    jae return_interrupt
    xor ah,ah
    shl ax,1
    mov bx,ax
    mov si,[cs:graphics_mode_pointers+bx]
    test si,si
    jz return_interrupt
    push si
    mov dx,DISPI_INDEX
    mov ax,4
    out dx,ax
    mov dx,DISPI_DATA
    xor ax,ax
    out dx,ax
    mov al,[cs:si+MODE_MISC]
    mov dx,MISC_OUTPUT
    out dx,al
    add si,MODE_SEQ
    mov dx,SEQUENCER
    mov cx,5
    call write_register_group
    mov dx,CRTC
    mov ax,0x0011
    out dx,ax
    mov cx,25
    call write_register_group
    mov dx,GRAPHICS
    mov cx,9
    call write_register_group
    test byte [MODESET_CONTROL],8
    jnz .palette_ready
    xor bx,bx
.attribute:
    mov al,bl
    mov ah,[cs:si+bx]
    call write_attribute
    inc bx
    cmp bx,21
    jb .attribute
    mov dx,DAC_MASK
    mov al,0xff
    out dx,al
    mov dx,DAC_WRITE
    xor al,al
    out dx,al
    mov dx,DAC_DATA
    mov al,[in_ax]
    and al,0x7f
    cmp al,0x13
    jne .ega_palette
    mov si,graphics_dac
    mov cx,768
    jmp .dac
.ega_palette:
    mov si,text_dac
    mov cx,192
.dac:
    mov al,[cs:si]
    out dx,al
    inc si
    loop .dac
    test byte [MODESET_CONTROL],2
    jz .palette_ready
    xor bx,bx
    mov cx,256
    call grayscale_colors
.palette_ready:
    pop si
    mov al,[cs:si]
    mov [MODE],al
    mov byte [0x66],0
    cmp al,6
    jne .cga_palette
    mov byte [0x66],15
.cga_palette:
    mov ax,[cs:si+MODE_WIDTH]
    shr ax,3
    mov [COLUMNS],ax
    mov ax,[cs:si+MODE_PAGE]
    mov [PAGE_BYTES],ax
    mov word [DISPLAY_START],0
    mov byte [ACTIVE_PAGE],0
    mov word [CRTC_BASE],CRTC
    mov al,[cs:si+MODE_FONT]
    xor ah,ah
    mov [CHARACTER_HEIGHT],ax
    mov bx,ax
    mov ax,[cs:si+MODE_HEIGHT]
    xor dx,dx
    div bx
    dec ax
    mov [ROWS_MINUS_ONE],al
    mov word [CURSOR_SHAPE],0x2000
    mov al,[in_ax]
    and al,0x80
    or al,0x60
    mov [VIDEO_CONTROL],al
    mov ax,0x40
    mov es,ax
    mov di,CURSORS
    mov cx,8
    xor ax,ax
    rep stosw
    xor ax,ax
    mov es,ax
    mov word [es:0x10e],cs
    mov ax,font_8
    cmp bx,8
    je .font
    mov ax,font_14
    cmp bx,14
    je .font
    mov ax,font_16
.font:
    mov [es:0x10c],ax
    test byte [in_ax],0x80
    jnz return_interrupt
    mov ax,0xa000
    mov cx,0x8000
    cmp byte [MODE],6
    ja .clear_segment
    mov ax,0xb800
    mov cx,0x2000
.clear_segment:
    mov es,ax
    xor di,di
    xor ax,ax
    rep stosw
    jmp return_interrupt

; Graphics pages occupy at most one 64-KiB plane aperture (chain-4 mode has
; only one page). CF reports an invalid page without modifying input registers.
; IBM BIOS Interface mode/page table, PDF 26.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=26
validate_graphics_page:
    cmp byte [MODE],6
    ja .vga
    test bh,bh
    jnz .cga_invalid
    clc
    ret
.cga_invalid:
    stc
    ret
.vga:
    push ax
    push dx
    cmp bh,8
    jae .invalid
    xor ax,ax
    mov al,bh
    inc ax
    mul word [PAGE_BYTES]
    cmp dx,1
    ja .invalid
    jne .valid
    test ax,ax
    jnz .invalid
.valid:
    pop dx
    pop ax
    clc
    ret
.invalid:
    pop dx
    pop ax
    stc
    ret

; Resolve pixel CX,DX on page BH to ES:DI and CL=bit mask for planar access.
; Preserve AX/BX; reject coordinates outside the current physical mode.
; IBM 42G2193 graphics memory organization, PDF 49 onward.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=49
pixel_address:
    cmp byte [MODE],0xff
    je .bad
    call validate_graphics_page
    jc .bad
    push ax
    push dx
    mov di,dx
    mov ax,[COLUMNS]
    shl ax,3
    cmp cx,ax
    jae .invalid
    mov ax,[CHARACTER_HEIGHT]
    xor dx,dx
    mov dl,[ROWS_MINUS_ONE]
    inc dx
    mul dx
    cmp di,ax
    jae .invalid
    cmp byte [MODE],6
    jbe .cga
    mov ax,0xa000
    mov es,ax
    mov ax,di
    mov dx,[COLUMNS]
    cmp byte [MODE],0x13
    jne .planar
    shl dx,3
    mul dx
    add ax,cx
    mov di,ax
    jmp .valid
.planar:
    mul dx
    mov di,ax
    mov ax,cx
    shr ax,3
    add di,ax
    xor ax,ax
    mov al,bh
    mul word [PAGE_BYTES]
    add di,ax
    and cl,7
    mov al,0x80
    shr al,cl
    mov cl,al
    jmp .valid
.cga:
    mov ax,0xb800
    mov es,ax
    mov ax,di
    shr ax,1
    mov dx,80
    mul dx
    test di,1
    jz .cga_row
    add ax,0x2000
.cga_row:
    mov di,ax
    mov ax,cx
    mov dl,cl
    cmp byte [MODE],6
    je .cga_mono
    shr ax,2
    and dl,3
    mov cl,3
    sub cl,dl
    shl cl,1
    jmp .cga_column
.cga_mono:
    shr ax,3
    and dl,7
    mov cl,7
    sub cl,dl
.cga_column:
    add di,ax
.valid:
    pop dx
    pop ax
    clc
    ret
.invalid:
    pop dx
    pop ax
.bad:
    stc
    ret

; Write one planar/chain-4 pixel; bit 7 requests XOR except in 256-color mode.
; Preserve all caller registers, segment state and VGA register settings. The
; read before a planar write loads all four latches, preserving neighboring bits.
; IBM BIOS Interface 0Ch, PDF 33; H write mode 2 and Data Rotate, PDF 106..109.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=33
pixel_write:
    pushad
    push es
    mov bp,sp
    call pixel_address
    jc .done
    cmp byte [MODE],6
    jbe .cga
    cmp byte [MODE],0x13
    jne .planar
    mov [es:di],al
    jmp .done
.planar:
    mov si,ax
    mov dx,SEQUENCER
    in al,dx
    push ax
    mov al,2
    call read_indexed_register
    push ax
    mov ax,0x0f02
    cmp byte [MODE],0x0f
    je .mono
    cmp byte [MODE],0x11
    jne .map_mask
.mono:
    mov ah,1
.map_mask:
    out dx,ax
    mov dx,GRAPHICS
    in al,dx
    push ax
    mov al,3
    call read_indexed_register
    push ax
    mov al,5
    call read_indexed_register
    push ax
    mov al,8
    call read_indexed_register
    push ax
    mov ah,cl
    mov al,8
    out dx,ax
    mov ax,0x0205 ; write mode 2 expands CPU color bits into the four planes.
    out dx,ax
    mov ax,3
    test si,0x80
    jz .logic
    mov ah,0x18 ; exclusive-OR with the latched destination.
.logic:
    out dx,ax
    mov al,[es:di]
    mov ax,si
    mov [es:di],al
    pop ax
    out dx,ax
    pop ax
    out dx,ax
    pop ax
    out dx,ax
    pop ax
    out dx,al
    mov dx,SEQUENCER
    pop ax
    out dx,ax
    pop ax
    out dx,al
    jmp .done
.cga:
    mov si,ax
    mov dx,SEQUENCER
    in al,dx
    push ax
    mov al,2
    call read_indexed_register
    push ax
    mov al,4
    call read_indexed_register
    push ax
    mov ax,0x0302
    cmp byte [MODE],6
    jne .cga_mask
    mov ah,1
.cga_mask:
    out dx,ax
    mov ax,0x0204
    cmp byte [MODE],6
    jne .cga_memory
    mov ah,6
.cga_memory:
    out dx,ax
    mov dx,GRAPHICS
    in al,dx
    push ax
    mov al,0
    call read_indexed_register
    push ax
    mov al,1
    call read_indexed_register
    push ax
    mov al,3
    call read_indexed_register
    push ax
    mov al,5
    call read_indexed_register
    push ax
    mov al,8
    call read_indexed_register
    push ax
    mov ax,0
    out dx,ax
    mov al,1
    out dx,ax
    mov al,3
    out dx,ax
    mov ax,0x3005
    cmp byte [MODE],6
    jne .cga_mode
    xor ah,ah
.cga_mode:
    out dx,ax
    mov ax,0xff08
    out dx,ax
    mov bl,[es:di]
    mov ax,si
    mov bh,3
    cmp byte [MODE],6
    jne .cga_field
    mov bh,1
.cga_field:
    and al,bh
    shl al,cl
    mov ch,al
    shl bh,cl
    test si,0x80
    jnz .cga_xor
    not bh
    and bl,bh
    or bl,ch
    jmp .cga_store
.cga_xor:
    xor bl,ch
.cga_store:
    mov [es:di],bl
    mov cx,5
.cga_restore:
    pop ax
    out dx,ax
    loop .cga_restore
    pop ax
    out dx,al
    mov dx,SEQUENCER
    pop ax
    out dx,ax
    pop ax
    out dx,ax
    pop ax
    out dx,al

.done:
    pop es
    popad
    ret

; Read the raw palette index from a pixel, preserving register programming.
; IBM BIOS Interface 0Dh, PDF 34; H Read Map Select and memory read, PDF 107.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=34
pixel_read:
    pushad
    push es
    mov bp,sp
    call pixel_address
    jc .zero
    cmp byte [MODE],6
    jbe .cga
    cmp byte [MODE],0x13
    jne .planar
    mov al,[es:di]
    jmp .result
.planar:
    mov dx,GRAPHICS
    in al,dx
    push ax
    mov al,4
    call read_indexed_register
    push ax
    mov al,5
    call read_indexed_register
    push ax
    and ah,0xf7 ; read mode 0, independently of caller compare/read mode.
    out dx,ax
    xor si,si
    xor bx,bx
.next_plane:
    mov al,4
    mov ah,bl
    out dx,ax
    mov al,[es:di]
    test al,cl
    jz .next
    bts si,bx
.next:
    inc bx
    cmp bx,4
    jb .next_plane
    pop ax
    out dx,ax
    pop ax
    out dx,ax
    pop ax
    out dx,al
    mov ax,si
    cmp byte [MODE],0x0f
    je .mono
    cmp byte [MODE],0x11
    jne .result
.mono:
    and al,1
    jmp .result
.cga:
    mov al,[es:di]
    shr al,cl
    cmp byte [MODE],6
    je .cga_mono
    and al,3
    jmp .result
.cga_mono:
    and al,1
    jmp .result

.zero:
    xor ax,ax
.result:
    mov [ss:bp+30],al ; low byte of saved EAX in PUSHAD + saved ES frame.
    pop es
    popad
    ret

; INT 10h pixel entry points only publish AL for reads; all other registers
; retain the interrupt caller's values. IBM BIOS Interface 0Ch/0Dh, PDF 33..34.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=33
write_dot:
    call pixel_write
    jmp return_interrupt
read_dot:
    call pixel_read
    mov [in_ax],al
    jmp return_interrupt

; Render a character from the active graphics font using BIOS pixel semantics.
; Planar background is zero; mode 13h uses BH as the background color. BL bit 7
; selects XOR only in non-256-color modes. IBM BIOS Interface 09h, PDF 32.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=32
draw_graphics_glyph:
    pushad
    push es
    push fs
    mov bp,sp
    sub sp,8
    xor ax,ax
    mov es,ax
    mov si,[es:0x10c]
    mov ax,[es:0x10e]
    mov fs,ax
    xor ax,ax
    mov al,[ss:bp+32]
    mul word [CHARACTER_HEIGHT]
    add si,ax
    mov dx,[ss:bp+24]
    movzx cx,dl
    shl cx,3
    mov [ss:bp-2],cx
    movzx dx,dh
    imul dx,[CHARACTER_HEIGHT]
    mov word [ss:bp-4],0
.row:
    mov al,[fs:si]
    inc si
    mov [ss:bp-6],al
    mov cx,[ss:bp-2]
    mov di,8
.column:
    mov bx,[ss:bp+20]
    mov al,bl
    test byte [ss:bp-6],0x80
    jnz .color
    cmp byte [MODE],0x13
    jne .background_zero
    mov al,bh
    jmp .color
.background_zero:
    and al,0x80
.color:
    cmp byte [MODE],0x13
    jne .page
    xor bh,bh
.page:
    call pixel_write
    shl byte [ss:bp-6],1
    inc cx
    dec di
    jnz .column
    inc dx
    inc word [ss:bp-4]
    mov ax,[ss:bp-4]
    cmp ax,[CHARACTER_HEIGHT]
    jb .row
    add sp,8
    pop fs
    pop es
    popad
    ret

; Repeat graphics glyphs on one character row, without advancing the cursor.
; IBM BIOS Interface 09h/0Ah, PDF 32..33: cross-row graphics repeats undefined.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=32
write_graphics_character:
    mov bx,[in_bx]
    cmp byte [MODE],0x13
    jne .page
    xor bh,bh
.page:
    call validate_graphics_page
    jc return_interrupt
    call cursor_address
    mov cx,[in_cx]
    movzx ax,dl
    mov si,[COLUMNS]
    sub si,ax
    cmp cx,si
    jbe .count
    mov cx,si
.count:
    test cx,cx
    jz return_interrupt
.next:
    mov ax,[in_ax]
    mov bx,[in_bx]
    call draw_graphics_glyph
    inc dl
    loop .next
    jmp return_interrupt

; Recognize a glyph by comparing a cached pixel mask with each ROM/user font
; glyph. Return character zero when no font glyph matches. Attribute return is
; specified only in text modes. IBM BIOS Interface 08h, PDF 32.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=32
read_graphics_character:
    call validate_graphics_page
    jc return_interrupt
    call cursor_address
    call recognize_graphics_glyph
    mov [in_ax],al
    jmp return_interrupt

; Cache one character cell's foreground mask, then search the active font.
; This helper is independent of the interrupt register frame and preserves all
; inputs except AL. IBM BIOS Interface 08h character-generator behavior, PDF 32.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=32
recognize_graphics_glyph:
    pushad
    push fs
    push es
    mov bp,sp
    sub sp,40
    movzx cx,dl
    shl cx,3
    mov [ss:bp-34],cx
    movzx dx,dh
    imul dx,[CHARACTER_HEIGHT]
    xor di,di
.row:
    mov cx,[ss:bp-34]
    xor si,si
    xor bl,bl
.pixel:
    call pixel_read
    shl si,1
    test al,al
    jz .zero
    or si,1
.zero:
    inc cx
    inc bl
    cmp bl,8
    jb .pixel
    mov ax,si
    mov [ss:bp+di-32],al
    inc dx
    inc di
    cmp di,[CHARACTER_HEIGHT]
    jb .row
    xor ax,ax
    mov es,ax
    mov ax,[es:0x10e]
    mov fs,ax
    mov ax,[es:0x10c]
    mov [ss:bp-36],ax
    xor bx,bx
.candidate:
    mov ax,bx
    mul word [CHARACTER_HEIGHT]
    add ax,[ss:bp-36]
    mov si,ax
    xor di,di
.compare:
    mov al,[fs:si]
    cmp al,[ss:bp+di-32]
    jne .mismatch
    inc si
    inc di
    cmp di,[CHARACTER_HEIGHT]
    jb .compare
    mov ax,bx
    jmp .result
.mismatch:
    inc bx
    cmp bx,256
    jb .candidate
    xor ax,ax
.result:
    mov [ss:bp+32],al
    add sp,40
    pop es
    pop fs
    popad
    ret

; Graphics teletype uses the same font/pixel rules as AH=09h, with cursor
; wrapping and scrolling. IBM BIOS Interface 0Eh, PDF 34.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=34
graphics_teletype:
    mov bh,[ACTIVE_PAGE]
    call cursor_address
    mov ax,[in_ax]
    cmp al,7
    je return_interrupt
    cmp al,8
    je .backspace
    cmp al,13
    je .carriage
    cmp al,10
    je .newline
    call draw_graphics_glyph
    inc dl
    cmp dl,[COLUMNS]
    jb .position
    xor dl,dl
.newline:
    inc dh
    jmp .position
.backspace:
    test dl,dl
    jz .position
    dec dl
    jmp .position
.carriage:
    xor dl,dl
.position:
    cmp dh,[ROWS_MINUS_ONE]
    jbe .save
    push dx
    push bx
    mov ax,0x0601
    xor bx,bx
    xor cx,cx
    mov dh,[ROWS_MINUS_ONE]
    mov dl,[COLUMNS]
    dec dl
    int 0x10
    pop bx
    pop dx
    mov dh,[ROWS_MINUS_ONE]
.save:
    xor ax,ax
    mov al,[ACTIVE_PAGE]
    shl ax,1
    mov si,ax
    mov [CURSORS+si],dx
    jmp return_interrupt

; Scroll character-aligned graphics rectangles one physical scan line at a
; time. Planar copies use VGA write mode 1 to copy four latched planes together;
; fills use set/reset. Chain-4 copies transfer packed bytes. Register state is
; restored and screen pixels outside the inclusive rectangle remain unchanged.
; IBM BIOS Interface 06h/07h, PDF 31; H VGA write mode 1, PDF 109.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=31
scroll_graphics_window:
    cmp byte [MODE],0xff
    je return_interrupt
    cmp ch,[ROWS_MINUS_ONE]
    ja return_interrupt
    cmp cl,[COLUMNS]
    jae return_interrupt
    cmp dh,[ROWS_MINUS_ONE]
    jbe .rows
    mov dh,[ROWS_MINUS_ONE]
.rows:
    cmp dl,[COLUMNS]
    jb .columns
    mov dl,[COLUMNS]
    dec dl
.columns:
    cmp ch,dh
    ja return_interrupt
    cmp cl,dl
    ja return_interrupt
    sub sp,24
    mov [ss:bp-52],bx
    movzx bx,cl
    mov [ss:bp-38],bx
    movzx bx,dl
    mov [ss:bp-40],bx
    push ax
    movzx ax,ch
    mul word [CHARACTER_HEIGHT]
    mov [ss:bp-42],ax
    ; DX was changed by MUL; obtain bottom from the saved interrupt input.
    movzx ax,byte [in_dx+1]
    cmp al,[ROWS_MINUS_ONE]
    jbe .bottom
    mov al,[ROWS_MINUS_ONE]
.bottom:
    inc ax
    mul word [CHARACTER_HEIGHT]
    dec ax
    mov [ss:bp-44],ax
    pop ax
    xor ah,ah
    mul word [CHARACTER_HEIGHT]
    mov [ss:bp-46],ax
    mov ax,[ss:bp-42]
    cmp byte [in_ax+1],7
    jne .first
    mov ax,[ss:bp-44]
.first:
    mov [ss:bp-48],ax
    mov word [ss:bp-54],1
    cmp byte [MODE],6
    jae .vga_factor
    mov word [ss:bp-54],2
    jmp .factor
.vga_factor:
    cmp byte [MODE],0x13
    jne .factor
    mov word [ss:bp-54],8
.factor:
    mov dx,SEQUENCER
    in al,dx
    push ax
    mov al,2
    call read_indexed_register
    push ax
    mov ax,0x0f02
    cmp byte [MODE],6
    ja .scroll_mask
    mov ah,3
    jne .scroll_mask
    mov ah,1
.scroll_mask:
    out dx,ax
    mov dx,GRAPHICS
    in al,dx
    push ax
    mov si,scroll_graphics_indexes
    mov cx,5
.save_registers:
    mov al,[cs:si]
    inc si
    call read_indexed_register
    push ax
    loop .save_registers
    mov dx,GRAPHICS
    mov ax,3
    out dx,ax
    mov ax,0xff08
    out dx,ax
    mov ax,0xa000
    cmp byte [MODE],6
    ja .scroll_segment
    mov ax,0xb800
.scroll_segment:
    mov es,ax
.row:
    mov ax,[ss:bp-48]
    cmp byte [in_ax+1],7
    je .source_up
    add ax,[ss:bp-46]
    jmp .source
.source_up:
    sub ax,[ss:bp-46]
.source:
    mov word [ss:bp-50],1
    cmp ax,[ss:bp-42]
    jb .blank
    cmp ax,[ss:bp-44]
    ja .blank
    cmp word [ss:bp-46],0
    jne .offsets
.blank:
    mov word [ss:bp-50],0
.offsets:
    call graphics_line_offset
    mov si,ax
    mov ax,[ss:bp-48]
    call graphics_line_offset
    mov di,ax
    mov cx,[ss:bp-40]
    sub cx,[ss:bp-38]
    inc cx
    imul cx,[ss:bp-54]
    mov dx,GRAPHICS
    cmp byte [MODE],6
    jbe .cga
    cmp byte [MODE],0x13
    je .packed
    mov ax,0x0005
    cmp word [ss:bp-50],0
    je .fill
    mov ah,1
    out dx,ax
.copy:
    mov al,[es:si]
    inc si
    stosb
    loop .copy
    jmp .advance
.fill:
    out dx,ax
    mov ah,[ss:bp-51] ; BH fill color.
    mov al,0
    out dx,ax
    mov ax,0x0f01
    out dx,ax
    xor al,al
    rep stosb
    mov ax,0x0001
    out dx,ax
    jmp .advance
.packed:
    mov ax,0x4005
    out dx,ax
    mov ax,1
    out dx,ax
    cmp word [ss:bp-50],0
    jne .copy
    mov al,[ss:bp-51]
    rep stosb
    jmp .advance
.cga:
    mov ax,0x3005
    cmp byte [MODE],6
    jne .cga_copy_mode
    xor ah,ah
.cga_copy_mode:
    out dx,ax
    mov ax,1
    out dx,ax
    cmp word [ss:bp-50],0
    jne .copy
    mov al,[ss:bp-51]
    cmp byte [MODE],6
    je .cga_fill_mono
    and al,3
    mov bl,0x55
    mul bl
    jmp .cga_fill
.cga_fill_mono:
    and al,1
    neg al
.cga_fill:
    rep stosb
.advance:
    mov ax,[ss:bp-48]
    cmp byte [in_ax+1],7
    je .down
    inc ax
    mov [ss:bp-48],ax
    cmp ax,[ss:bp-44]
    jbe .row
    jmp .done
.down:
    dec ax
    mov [ss:bp-48],ax
    cmp ax,[ss:bp-42]
    jge .row
.done:
    mov dx,GRAPHICS
    mov cx,5
.restore:
    pop ax
    out dx,ax
    loop .restore
    pop ax
    out dx,al
    mov dx,SEQUENCER
    pop ax
    out dx,ax
    pop ax
    out dx,al
    add sp,24
    jmp return_interrupt

; Compute one graphics scan-line/window offset in AX. These helpers are local
; to the window operation; left position and byte factor are stack parameters.
; IBM 42G2193 graphics memory addressing, PDF 49.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=49
graphics_line_offset:
    cmp byte [MODE],6
    ja .vga
    push ax
    shr ax,1
    mul word [COLUMNS]
    imul ax,[ss:bp-54]
    pop dx
    and dx,1
    shl dx,13
    add ax,dx
    jmp .left
.vga:
    mul word [COLUMNS]
    imul ax,[ss:bp-54]
.left:
    push ax
    mov ax,[ss:bp-38]
    imul ax,[ss:bp-54]
    mov bx,ax
    xor ax,ax
    mov al,[ACTIVE_PAGE]
    mul word [PAGE_BYTES]
    add ax,bx
    pop bx
    add ax,bx
    ret
scroll_graphics_indexes db 0,1,3,5,8
