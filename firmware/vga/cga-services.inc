; CGA RGBI values map to the VGA's EGA-compatible DAC entries. BL is the
; sixteen-color RGBI index; AH is the six-bit Attribute palette value.
; IBM 42G2193 color mapping, printed 2-92..93, PDF 118..119.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=118
cga_rgbi_color:
    mov ah,bl
    and ah,7
    test bl,8
    jz .normal
    add ah,0x38
    ret
.normal:
    cmp ah,6
    jne .done
    mov ah,20
.done:
    ret

; AH=0Bh sets the text border/CGA background and selects the CGA palette.
; In 640x200 monochrome mode the color is the foreground. In 320x200 modes,
; bit 4 selects bright foreground colors and BH=1 selects the alternate palette.
; IBM BIOS Interface, printed 2-18, PDF 33.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=33
cga_palette_service:
    cmp bh,1
    ja return_interrupt
    je .select
    call cga_rgbi_color
    mov al,0x11
    call write_attribute
    cmp byte [MODE],4
    jb return_interrupt
    cmp byte [MODE],6
    ja return_interrupt
    and byte [0x66],0x20
    and bl,0x1f
    or [0x66],bl
    cmp byte [MODE],6
    je .mono
    mov al,0
    call write_attribute
    jmp .colors
.mono:
    mov al,1
    call write_attribute
    jmp return_interrupt
.select:
    cmp bl,1
    ja return_interrupt
    and byte [0x66],0xdf
    shl bl,5
    or [0x66],bl
.colors:
    cmp byte [MODE],4
    jb return_interrupt
    cmp byte [MODE],5
    ja return_interrupt
    mov si,1
.next:
    mov bx,si
    shl bl,1
    test byte [0x66],0x20
    jz .mode5
    inc bl
.mode5:
    cmp byte [MODE],5
    jne .intensity
    mov bx,si
    cmp bl,1
    jne .mode5_other
    mov bl,3
    jmp .intensity
.mode5_other:
    cmp bl,2
    mov bl,4
    je .intensity
    mov bl,7
.intensity:
    test byte [0x66],0x10
    jz .write
    or bl,8
.write:
    call cga_rgbi_color
    mov dx,ax
    mov ax,si
    mov ah,dh
    call write_attribute
    inc si
    cmp si,4
    jb .next
    jmp return_interrupt
