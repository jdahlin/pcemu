; PCEmu standalone VGA option ROM, VGA and Bochs DISPI implementation.
; All services execute on the guest CPU using VGA ports and video memory; no
; host BIOS trap, SDL call, or mutable ROM state is used. Requires an 80386+ CPU.
; This is a documented subset, NOT a complete IBM VGA or VBE implementation.
;
; BIOS reference B: IBM PS/2 and PC BIOS Interface Technical Reference, April
; 1987, INT 10H (printed 2-10..2-39), BIOS video data (printed 3-7, 3-12).
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf
; Hardware reference H: IBM 42G2193, September 1992, section 2 VGA Function.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf
;
; IMPLEMENTATION NOTE: Only mode 03h/83h is accepted in this milestone. Unknown
; legacy functions leave registers unchanged; VBE calls return unsupported.
; FIXME: Graphics modes, 40-column/mono modes, font loading, state save/restore,
; video parameter tables, and additional palette functions are not implemented.
; B, INT 10H function summary, PDF page 25 (link below).
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=25
bits 16
cpu 386
org 0

; IBM expansion-ROM header: signature, length in 512-byte blocks, entry at 3.
; B, "Adapter ROM", printed 4-12, PDF page 173.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=173
dw 0xaa55
db 64
jmp initialize
times 0x18-($-$$) db 0
dw pci_data ; PCI expansion-ROM header: offset of the PCI data structure.
dw 0 ; No Plug and Play expansion header.
times 0x20-($-$$) db 0
; PCI 2.2 PCI_DATA_STRUCTURE (24 bytes); wire-format reference:
; https://github.com/tianocore/edk2/blob/master/MdePkg/Include/IndustryStandard/Pci22.h
; Fixed 1234:1111 VGA identity matches PCEmu's VGA/DISPI PCI device.
pci_data:
    db 'PCIR'
    dw 0x1234,0x1111,0,24 ; vendor, device, VPD pointer, structure length.
    db 0,0,0,3 ; revision 0; VGA display class 030000h, least-significant first.
    dw 64,0 ; image length in 512-byte blocks; firmware code revision.
    db 0,0x80 ; x86 executable image; final image in this ROM.
    dw 0 ; reserved.
identification db 'PCEmu standalone VGA BIOS VGA/VBE',0

; Absolute VGA ports, named according to H General/Sequencer/CRTC/Graphics/
; Attribute/DAC register descriptions, printed 2-39..2-89, PDF pages 65..115.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=65
%define SEQUENCER 0x3c4
%define GRAPHICS 0x3ce
%define CRTC 0x3d4
%define ATTRIBUTE 0x3c0
%define STATUS1 0x3da
%define MISC_OUTPUT 0x3c2
%define DAC_MASK 0x3c6
%define DAC_READ 0x3c7
%define DAC_WRITE 0x3c8
%define DAC_DATA 0x3c9
%define DISPI_INDEX 0x1ce
%define DISPI_DATA 0x1cf

; BDA offsets relative to segment 0040h. B, Video Control Data Area 1 and 2.
; Data Area 2 (rows/height/control): same document PDF page 146.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=144
%define MODE 0x49
%define COLUMNS 0x4a
%define PAGE_BYTES 0x4c
%define DISPLAY_START 0x4e
%define CURSORS 0x50
%define CURSOR_SHAPE 0x60
%define ACTIVE_PAGE 0x62
%define CRTC_BASE 0x63
%define ROWS_MINUS_ONE 0x84
%define CHARACTER_HEIGHT 0x85
%define VIDEO_CONTROL 0x87
%define VIDEO_SWITCHES 0x88
%define MODESET_CONTROL 0x89
%define DCC_INDEX 0x8a
%define VIDEO_SAVE_POINTER 0xa8

; Install INT 10h and font pointers, then initialize mode 3. Preserve the
; caller's registers and FLAGS, including its direction/interrupt settings.
; B INT 10H entry/ROM tables, PDF pages 25 and 41..43.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=25
initialize:
    pushf
    pusha
    push ds
    push es
    cld
    cli
    xor ax,ax
    mov es,ax
    mov word [es:0x40],video_interrupt
    mov [es:0x42],cs
    mov word [es:0x7c],font_8+128*8
    mov [es:0x7e],cs
    mov word [es:0x10c],font_16
    mov [es:0x10e],cs
    mov word [es:0x74],video_parameters ; INT 1Dh mode parameter table.
    mov [es:0x76],cs
    mov ax,0x40
    mov ds,ax
    mov byte [MODESET_CONTROL],0x51 ; 400 lines, cursor emulation, VGA enabled.
    mov byte [VIDEO_SWITCHES],0xf9
    mov byte [DCC_INDEX],0
    mov word [VIDEO_SAVE_POINTER],video_save_table
    mov [VIDEO_SAVE_POINTER+2],cs
    mov ax,3
    int 0x10
    pop es
    pop ds
    popa
    popf
    retf

; Save a private register frame. Outputs are written into saved slots, so
; internal arithmetic cannot leak scratch values to the guest. IRET restores
; incoming FLAGS (including DF); string operations always run forward here.
; B INT 10H per-function register contracts, PDF pages 30..54.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=30
%define in_ax ss:bp-8
%define in_bx ss:bp-20
%define in_cx ss:bp-12
%define in_dx ss:bp-16
%define in_si ss:bp-32
%define in_di ss:bp-36
%define in_es ss:bp-4
video_interrupt:
    push bp
    mov bp,sp
    push ds
    push es
    pushad
    cld
    mov ax,0x40
    mov ds,ax
    mov ax,[in_ax]
    cmp ah,0x00
    je set_mode
    cmp ah,0x01
    je set_cursor_shape
    cmp ah,0x02
    je set_cursor_position
    cmp ah,0x03
    je read_cursor_position
    cmp ah,0x05
    je select_page
    cmp ah,0x06
    je scroll_window
    cmp ah,0x07
    je scroll_window
    cmp ah,0x08
    je read_character
    cmp ah,0x09
    je write_character
    cmp ah,0x0a
    je write_character
    cmp ah,0x0e
    je teletype
    cmp ah,0x0f
    je read_mode
    cmp ah,0x10
    je palette_service
    cmp ah,0x11
    je font_service
    cmp ah,0x12
    je alternate_select
    cmp ah,0x13
    je write_string
    cmp ah,0x1b
    je functionality_information
    cmp ah,0x1a
    je display_information
    cmp ah,0x4f
    jne return_interrupt
    mov word [in_ax],0x014f ; VBE standard status: function unsupported.
return_interrupt:
    popad
    pop es
    pop ds
    pop bp
    iret

; Program the indexed register group at DX from CX bytes in CS:SI.
; H indexed-register interfaces; helper policy: starts at index zero.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=65
write_register_group:
    xor bx,bx
.next:
    mov al,bl
    mov ah,[cs:si]
    out dx,ax
    inc si
    inc bx
    loop .next
    ret

; Initialize 80x25 color text, including plane-2 font, DAC and BIOS data.
; AL bit 7 preserves display memory; B 00H, PDF pages 27..29. H registers
; determine odd/even text access, 400 scan lines and 16-line characters.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=29
set_mode:
    and al,0x7f
    cmp al,3
    jbe .accepted
    cmp al,7
    jne return_interrupt
.accepted:
    mov dx,DISPI_INDEX
    mov ax,4 ; DISPI Enable index: disable extended framebuffer before VGA setup.
    out dx,ax
    mov dx,DISPI_DATA
    xor ax,ax
    out dx,ax
    mov dx,MISC_OUTPUT
    mov al,0x67 ; color CRTC, RAM enabled, 28 MHz clock, 400-line sync polarity.
    out dx,al
    mov dx,SEQUENCER
    mov si,text_sequencer
    mov cx,5
    call write_register_group
    mov dx,CRTC
    mov ax,0x0e11 ; clear CRTC protection while loading the timing registers.
    out dx,ax
    mov si,text_crtc
    mov cx,25
    call write_register_group
    mov dx,GRAPHICS
    mov si,text_graphics
    mov cx,9
    call write_register_group
    ; 40-column modes divide the dot clock and use half-width horizontal timing.
    mov al,[in_ax]
    and al,0x7f
    cmp al,2
    jae .horizontal_ready
    mov dx,SEQUENCER
    mov ax,0x0801
    out dx,ax
    mov dx,CRTC
    mov si,text40_crtc
    mov cx,6
    call write_register_group
.horizontal_ready:
    test byte [MODESET_CONTROL],8
    jnz .palette_ready
    mov dx,STATUS1
    in al,dx
    mov dx,ATTRIBUTE
    xor bx,bx
.attr:
    mov al,bl
    out dx,al
    mov al,[cs:text_attribute+bx]
    out dx,al
    inc bx
    cmp bx,21
    jb .attr
    mov al,0x20 ; Palette Address Source: enable display.
    out dx,al
    mov dx,DAC_MASK
    mov al,0xff
    out dx,al
    mov dx,DAC_WRITE
    xor al,al
    out dx,al
    inc dx
    xor si,si
    mov cx,64*3
.dac:
    mov al,[cs:text_dac+si]
    out dx,al
    inc si
    loop .dac
.palette_ready:
    call load_text_font
    mov al,[in_ax]
    and al,0x7f
    mov [MODE],al
    mov word [COLUMNS],80
    mov word [PAGE_BYTES],4096
    cmp al,2
    jae .text_geometry
    mov word [COLUMNS],40
    mov word [PAGE_BYTES],2048
.text_geometry:
    mov word [DISPLAY_START],0
    mov byte [ACTIVE_PAGE],0
    mov word [CRTC_BASE],CRTC
    cmp byte [MODE],7
    jne .color_crtc
    mov word [CRTC_BASE],0x3b4
    mov dx,MISC_OUTPUT
    mov al,0x66
    out dx,al
    mov dx,GRAPHICS
    mov ax,0x0a06 ; B0000 monochrome text aperture.
    out dx,ax
.color_crtc:
    mov word [CURSOR_SHAPE],0x0d0e
    mov byte [ROWS_MINUS_ONE],24
    mov word [CHARACTER_HEIGHT],16
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
    mov al,16
    test byte [MODESET_CONTROL],0x80
    jz .not_200
    mov al,8
    jmp .font_geometry
.not_200:
    test byte [MODESET_CONTROL],0x10
    jnz .font_geometry
    mov al,14
.font_geometry:
    ; Use the character-generator service so scan-line and font state agree.
    mov bl,0
    mov ah,0x11
    cmp al,8
    mov al,0x12
    je .font_call
    mov al,0x11
    test byte [MODESET_CONTROL],0x10
    jz .font_call
    mov al,0x14
.font_call:
    cmp byte [MODE],7
    jne .font_selected
    mov al,0x11
.font_selected:
    int 0x10
    test byte [in_ax],0x80
    jnz return_interrupt
    call text_memory_segment
    xor di,di
    mov ax,0x0720
    mov cx,0x4000 ; clear all eight 4-KiB text pages, including their padding.
    rep stosw
    jmp return_interrupt

; Copy the ROM 8x16 font to plane 2 with a 32-byte slot per character, then
; restore text addressing. H Character Generator RAM/Sequencer Memory Mode.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=78
load_text_font:
    mov dx,SEQUENCER
    mov ax,0x0402 ; Map Mask: write plane 2 only.
    out dx,ax
    mov ax,0x0604 ; extended memory, sequential addressing, no chain-4.
    out dx,ax
    mov dx,GRAPHICS
    mov ax,0x0204 ; Read Map Select: plane 2.
    out dx,ax
    mov ax,0x0005 ; write mode 0; disable host odd/even addressing.
    out dx,ax
    mov ax,0x0406 ; A0000 64-KiB window, graphics odd/even disabled.
    out dx,ax
    mov ax,0xa000
    mov es,ax
    xor di,di
    mov si,font_16
    mov bx,256
.character:
    mov cx,16
.row:
    mov al,[cs:si]
    inc si
    stosb
    loop .row
    mov cx,16
    xor ax,ax
    rep stosb
    dec bx
    jnz .character
    mov dx,SEQUENCER
    mov ax,0x0302 ; text character/attribute planes 0 and 1.
    out dx,ax
    mov ax,0x0204 ; extended memory with odd/even addressing.
    out dx,ax
    mov dx,GRAPHICS
    mov ax,0x0004
    out dx,ax
    mov ax,0x1005 ; host odd/even addressing.
    out dx,ax
    mov ax,0x0e06 ; B8000 text window, chain odd/even enabled.
    out dx,ax
    ret

; Store cursor shape and update the CRTC start/end scan-line registers.
; B 01H, PDF pages 29..30 (PS/2 VGA, excluding Model 30; shape values
; are hardware scan lines, without Model 30 double-scan cursor emulation).
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=30
set_cursor_shape:
    mov [CURSOR_SHAPE],cx
    test byte [MODESET_CONTROL],1
    jz .hardware_shape
    cmp byte [CHARACTER_HEIGHT],8
    jbe .hardware_shape
    cmp ch,8
    jae .hardware_shape
    cmp cl,8
    jae .hardware_shape
    cmp ch,cl
    ja .hardware_shape
    mov bl,cl
    mov al,ch
    inc al
    mul byte [CHARACTER_HEIGHT]
    shr ax,3
    dec ax
    mov ch,al
    mov al,bl
    inc al
    mul byte [CHARACTER_HEIGHT]
    shr ax,3
    dec ax
    mov cl,al
.hardware_shape:
    mov dx,[CRTC_BASE]
    mov al,0x0a
    mov ah,ch
    out dx,ax
    mov al,0x0b
    mov ah,cl
    out dx,ax
    jmp return_interrupt

; Resolve BH's cursor into DI = byte offset in B8000, DX = row/column.
; B video page/cursor data layout, PDF page 144. Caller validates BH < 8.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=144
cursor_address:
    xor ax,ax
    mov al,bh
    mov si,ax
    shl si,1
    mov dx,[CURSORS+si]
    mul word [PAGE_BYTES]
    mov di,ax
    mov dx,[CURSORS+si]
    xor ax,ax
    mov al,dh
    mul word [COLUMNS]
    mov dx,[CURSORS+si]
    xor dh,dh
    add ax,dx
    shl ax,1
    add di,ax
    mov dx,[CURSORS+si]
    ret

; Write cursor location into the CRTC only for the currently displayed page.
; H Cursor Location High/Low, printed 2-68..2-69.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=94
update_cursor:
    cmp bh,[ACTIVE_PAGE]
    jne .done
    call cursor_address
    mov cx,di
    shr cx,1
    mov dx,[CRTC_BASE]
    mov al,0x0e
    mov ah,ch
    out dx,ax
    mov al,0x0f
    mov ah,cl
    out dx,ax
.done:
    ret

; Set one page's cursor; do not move the active page when updating another.
; B 02H, PDF page 30.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=30
set_cursor_position:
    call validate_text_page
    jc return_interrupt
    xor ax,ax
    mov al,bh
    shl ax,1
    mov si,ax
    cmp dh,[ROWS_MINUS_ONE]
    ja return_interrupt
    xor ax,ax
    mov al,dl
    cmp ax,[COLUMNS]
    jae return_interrupt
    mov [CURSORS+si],dx
    call update_cursor
    jmp return_interrupt

; Return requested page cursor and shared cursor shape.
; B 03H, PDF page 30.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=30
read_cursor_position:
    call validate_text_page
    jc return_interrupt
    call cursor_address
    mov [in_dx],dx
    mov cx,[CURSOR_SHAPE]
    mov [in_cx],cx
    jmp return_interrupt

; Select one of eight text pages; CRTC start uses words, BDA start uses bytes.
; B 05H, PDF page 31; H Start Address High/Low, PDF page 93.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=31
select_page:
    mov bh,al
    call validate_text_page
    jc return_interrupt
    mov [ACTIVE_PAGE],al
    xor ah,ah
    mul word [PAGE_BYTES]
    mov [DISPLAY_START],ax
    shr ax,1
    mov cx,ax
    mov dx,[CRTC_BASE]
    mov al,0x0c
    mov ah,ch
    out dx,ax
    mov al,0x0d
    mov ah,cl
    out dx,ax
    call update_cursor
    jmp return_interrupt

; Read character and attribute using the REQUESTED page's cursor.
; B 08H, PDF page 32.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=32
read_character:
    call validate_text_page
    jc return_interrupt
    call cursor_address
    call text_memory_segment
    mov ax,[es:di]
    mov [in_ax],ax
    jmp return_interrupt

; Repeat character with/without attribute; leave cursor unchanged. Clamp writes
; to the page allocation to prevent overflowing the 32-KiB text aperture.
; B 09H/0AH, PDF pages 32..33; clamp is defensive implementation policy.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=32
write_character:
    call validate_text_page
    jc return_interrupt
    call cursor_address
    call text_memory_segment
    mov ax,[in_ax]
    mov cx,[in_cx]
    mov dx,di
    xor ax,ax
    mov al,bh
    mul word [PAGE_BYTES]
    mov dx,di
    sub dx,ax
    neg dx
    add dx,[PAGE_BYTES]
    mov ax,[in_ax]
    shr dx,1
    cmp cx,dx
    jbe .count_ok
    mov cx,dx
.count_ok:
    jcxz .done
    cmp ah,9
    mov ah,bl
    jne .character_only
    rep stosw
    jmp .done
.character_only:
    stosb
    inc di
    loop .character_only
.done:
    jmp return_interrupt

; Scroll the active page's inclusive rectangle. Invalid rectangles are ignored;
; bottom/right are clipped. Copy rows in overlap-safe order, then blank exposed
; rows using BH. B 06H/07H, PDF page 31. No guest cursor moves.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=31
scroll_window:
    cmp ch,[ROWS_MINUS_ONE]
    ja return_interrupt
    cmp cl,[COLUMNS]
    jae return_interrupt
    cmp dh,[ROWS_MINUS_ONE]
    jbe .row_ok
    mov dh,[ROWS_MINUS_ONE]
.row_ok:
    cmp dl,[COLUMNS]
    jb .col_ok
    mov dl,[COLUMNS]
    dec dl
.col_ok:
    cmp ch,dh
    ja return_interrupt
    cmp cl,dl
    ja return_interrupt
    ; Stack locals: rectangle, operation/lines, fill attribute, destination row.
    sub sp,10
    mov [ss:bp-38],cx
    mov [ss:bp-40],dx
    mov [ss:bp-42],ax
    mov [ss:bp-44],bx
    xor ax,ax
    mov al,ch
    cmp byte [in_ax+1],7
    jne .first_row
    mov al,dh
.first_row:
    mov [ss:bp-46],ax
    call text_memory_segment
.row:
    mov ax,[ss:bp-46]
    mov bx,[ss:bp-42]
    xor bh,bh
    cmp byte [in_ax+1],7
    je .down
    add ax,bx
    jmp .source
.down:
    sub ax,bx
.source:
    ; AX source row; zero lines or source outside window means blank row.
    mov si,ax
    xor dx,dx
    mov dl,[ss:bp-37] ; CH, top
    cmp ax,dx
    jb .blank
    mov dl,[ss:bp-39] ; DH, bottom
    cmp ax,dx
    ja .blank
    test bx,bx
    jz .blank
    mov bx,1
    jmp .offsets
.blank:
    xor bx,bx
.offsets:
    mov ax,si
    mov dx,[COLUMNS]
    shl dx,1
    mul dx
    mov si,ax
    mov ax,[ss:bp-46]
    mov dx,[COLUMNS]
    shl dx,1
    mul dx
    mov di,ax
    xor ax,ax
    mov al,[ACTIVE_PAGE]
    mul word [PAGE_BYTES]
    add di,ax
    add si,ax
    xor ax,ax
    mov al,[ss:bp-38] ; CL, left
    shl ax,1
    add di,ax
    add si,ax
    xor cx,cx
    mov cl,[ss:bp-40] ; DL, right
    sub cl,[ss:bp-38]
    inc cx
    mov ah,[ss:bp-43] ; BH, blank attribute
    mov al,0x20
.cell:
    test bx,bx
    jz .store
    mov ax,[es:si]
.store:
    stosw
    add si,2
    loop .cell
    mov ax,[ss:bp-46]
    cmp byte [in_ax+1],7
    je .next_down
    inc ax
    mov [ss:bp-46],ax
    cmp al,[ss:bp-39] ; DH, bottom
    jbe .row
    jmp .done
.next_down:
    dec ax
    mov [ss:bp-46],ax
    cmp ax,0
    jl .done
    cmp al,[ss:bp-37] ; CH, top
    jae .row
.done:
    add sp,10
    jmp return_interrupt

; Emit a character on the active page. CR/LF/BS/BEL are controls. Scroll on
; bottom overflow, using the last row's first attribute for the new line.
; B 0EH, PDF page 34. Bell is silent (speaker synthesis is outside this ROM).
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=34
teletype:
    mov bh,[ACTIVE_PAGE]
    call cursor_address
    call text_memory_segment
    mov al,[in_ax]
    cmp al,7
    je return_interrupt
    cmp al,8
    je .backspace
    cmp al,13
    je .carriage
    cmp al,10
    je .newline
    mov [es:di],al
    inc dl
    cmp dl,[COLUMNS]
    jb .position
    xor dl,dl
.newline:
    inc dh
    jmp .position
.carriage:
    xor dl,dl
    jmp .position
.backspace:
    test dl,dl
    jz .position
    dec dl
.position:
    cmp dh,[ROWS_MINUS_ONE]
    jbe .save
    ; COMPATIBILITY: use the last row's first attribute, matching Bochs VGABIOS.
    ; The IBM 0EH description does not specify the blank-line attribute.
    push dx
    push bx
    xor ax,ax
    mov al,[ACTIVE_PAGE]
    mul word [PAGE_BYTES]
    mov si,ax
    xor ax,ax
    mov al,[ROWS_MINUS_ONE]
    mul word [COLUMNS]
    shl ax,1
    add si,ax
    mov bh,[es:si+1]
    mov ax,0x0601
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
    mov al,bh
    shl ax,1
    mov si,ax
    mov [CURSORS+si],dx
    call update_cursor
    jmp return_interrupt

; Return current mode, columns, active page. B 0FH, PDF page 34.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=34
read_mode:
    mov al,[MODE]
    mov ah,[VIDEO_CONTROL]
    and ah,0x80
    or al,ah
    mov ah,[COLUMNS]
    mov [in_ax],ax
    mov al,[ACTIVE_PAGE]
    mov [in_bx+1],al
    jmp return_interrupt

%include "text-services.inc"
%include "font-services.inc"
%include "palette-services.inc"
%include "query-services.inc"

; Mode-3 programming values encode 720x400 raster, 80x25 characters, 9x16
; cells, 4-KiB pages. H CRTC/Sequencer/Graphics/Attribute register definitions.
; IMPLEMENTATION NOTE: timing values describe the intended raster; our VGA
; currently models register state and pixels rather than physical sync timing.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=65
text_sequencer db 3,0,3,0,2
text40_crtc db 0x2d,0x27,0x28,0x90,0x2b,0xa0
text_crtc db 0x5f,0x4f,0x50,0x82,0x55,0x81,0xbf,0x1f,0,0x4f,0x0d,0x0e,0,0,0,0
          db 0x9c,0x8e,0x8f,0x28,0x1f,0x96,0xb9,0xa3,0xff
text_graphics db 0,0,0,0,0,0x10,0x0e,0,0xff
text_attribute db 0,1,2,3,4,5,0x14,7,0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f
               db 0x0c,0,0x0f,8,0
; EGA-compatible 64-entry DAC: low RGB bits contribute 42, upper bits 21.
; H Color Register Mapping, DAC color definition, printed 2-92 onward.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=118
text_dac:
%assign color 0
%rep 64
    db (((color >> 2) & 1)*42 + ((color >> 5) & 1)*21)
    db (((color >> 1) & 1)*42 + ((color >> 4) & 1)*21)
    db ((color & 1)*42 + ((color >> 3) & 1)*21)
%assign color color+1
%endrep

%include "pcemu-vga-fonts.inc"
times 32767-($-$$) db 0
db 0 ; build script sets the checksum byte; ROM is immutable at runtime.
