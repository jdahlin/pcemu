; Attribute Controller reads must reset its alternating address/data latch.
; H Attribute Controller, printed 2-86 onward, PDF 112. AL=index -> AH=value.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=112
read_attribute:
    push bx
    mov bl,al
    mov dx,STATUS1
    in al,dx
    mov dx,ATTRIBUTE
    mov al,bl
    out dx,al
    inc dx
    in al,dx
    mov ah,al
    mov dx,STATUS1
    in al,dx
    mov dx,ATTRIBUTE
    mov al,0x20
    out dx,al
    mov al,bl
    pop bx
    ret

; Write AH to Attribute Controller index AL and re-enable display output.
; H Attribute Controller address/data protocol, PDF 112.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=112
write_attribute:
    push bx
    mov bx,ax
    mov dx,STATUS1
    in al,dx
    mov dx,ATTRIBUTE
    mov al,bl
    out dx,al
    mov al,bh
    out dx,al
    mov al,0x20
    out dx,al
    pop bx
    ret

; AH=10h implements EGA attribute palette, VGA RGB DAC, color paging, PEL mask
; and grayscale conversion. Buffer operations are bounded by 256 DAC entries.
; IBM BIOS Interface, printed 2-20..24, PDF 35..39.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=35
palette_service:
    cmp al,0
    je .write_palette
    cmp al,1
    je .write_border
    cmp al,2
    je .palette_block
    cmp al,3
    je .blink
    cmp al,7
    je .read_palette
    cmp al,8
    je .read_border
    cmp al,9
    je .palette_block
    cmp al,0x10
    je .write_dac
    cmp al,0x12
    je .dac_block
    cmp al,0x13
    je .color_page
    cmp al,0x15
    je .read_dac
    cmp al,0x17
    je .dac_block
    cmp al,0x18
    je .set_mask
    cmp al,0x19
    je .get_mask
    cmp al,0x1a
    je .get_page
    cmp al,0x1b
    je .gray
    jmp return_interrupt
.write_palette:
    cmp bl,15
    ja return_interrupt
    mov al,bl
    mov ah,bh
    call write_attribute
    jmp return_interrupt
.write_border:
    mov al,0x11
    mov ah,bh
    call write_attribute
    jmp return_interrupt
.read_palette:
    cmp bl,15
    ja return_interrupt
    mov al,bl
    call read_attribute
    mov [in_bx+1],ah
    jmp return_interrupt
.read_border:
    mov al,0x11
    call read_attribute
    mov [in_bx+1],ah
    jmp return_interrupt
.palette_block:
    mov di,[in_dx]
    xor si,si
.palette_next:
    mov ax,si
    cmp al,16
    jb .palette_index
    inc al ; overscan index is 11h; mode control at 10h is not a palette entry.
.palette_index:
    cmp byte [in_ax],2
    jne .palette_read
    mov ah,[es:di]
    call write_attribute
    jmp .palette_advance
.palette_read:
    call read_attribute
    mov [es:di],ah
.palette_advance:
    inc di
    inc si
    cmp si,17
    jb .palette_next
    jmp return_interrupt
.blink:
    cmp bl,1
    ja return_interrupt
    mov al,0x10
    call read_attribute
    and ah,0xf7
    test bl,1
    jz .blink_write
    or ah,8
.blink_write:
    call write_attribute
    jmp return_interrupt
.write_dac:
    cmp bx,255
    ja return_interrupt
    mov dx,DAC_WRITE
    mov al,bl
    out dx,al
    inc dx
    mov al,[in_dx+1]
    out dx,al
    mov al,ch
    out dx,al
    mov al,cl
    out dx,al
    test byte [MODESET_CONTROL],2
    jz return_interrupt
    mov cx,1
    call grayscale_colors
    jmp return_interrupt
.read_dac:
    cmp bx,255
    ja return_interrupt
    mov dx,DAC_READ
    mov al,bl
    out dx,al
    mov dx,DAC_DATA
    in al,dx
    mov [in_dx+1],al
    in al,dx
    mov [in_cx+1],al
    in al,dx
    mov [in_cx],al
    jmp return_interrupt
.dac_block:
    cmp bx,256
    jae return_interrupt
    mov ax,256
    sub ax,bx
    cmp cx,ax
    jbe .dac_count
    mov cx,ax
.dac_count:
    push bx
    push cx
    mov si,[in_dx]
    mov dx,DAC_WRITE
    cmp byte [in_ax],0x12
    je .dac_index
    mov dx,DAC_READ
.dac_index:
    mov al,bl
    out dx,al
    mov dx,DAC_DATA
    imul cx,cx,3
    test cx,cx
    jz .dac_finished
.dac_next:
    cmp byte [in_ax],0x12
    jne .dac_read_byte
    mov al,[es:si]
    out dx,al
    jmp .dac_advance
.dac_read_byte:
    in al,dx
    mov [es:si],al
.dac_advance:
    inc si
    loop .dac_next
.dac_finished:
    pop cx
    pop bx
    cmp byte [in_ax],0x12
    jne return_interrupt
    test byte [MODESET_CONTROL],2
    jz return_interrupt
    call grayscale_colors
    jmp return_interrupt
.color_page:
    cmp bl,1
    ja return_interrupt
    je .set_page
    cmp bh,1
    ja return_interrupt
    mov al,0x10
    call read_attribute
    and ah,0x7f
    test bh,1
    jz .paging_write
    or ah,0x80
.paging_write:
    call write_attribute
    jmp return_interrupt
.set_page:
    mov al,0x10
    call read_attribute
    test ah,0x80
    jnz .page16
    mov ah,bh
    and ah,3
    shl ah,2
    jmp .page_value
.page16:
    mov ah,bh
    and ah,15
.page_value:
    mov al,0x14
    call write_attribute
    jmp return_interrupt
.get_page:
    mov al,0x10
    call read_attribute
    mov cl,ah
    mov al,0x14
    call read_attribute
    and ah,15
    mov al,1
    test cl,0x80
    jnz .page_output
    shr ah,2
    xor al,al
.page_output:
    mov [in_bx],ax
    jmp return_interrupt
.set_mask:
    mov dx,DAC_MASK
    mov al,bl
    out dx,al
    jmp return_interrupt
.get_mask:
    mov dx,DAC_MASK
    in al,dx
    mov [in_bx],al
    jmp return_interrupt
.gray:
    call grayscale_colors
    jmp return_interrupt

; Replace CX entries starting at BX with the specified 30R+59G+11B weighted
; grayscale. Round to nearest integer; DAC reads supply its current precision.
; IBM BIOS Interface, printed 2-24, PDF 39.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=39
grayscale_colors:
    cmp bx,256
    jae .done
    mov ax,256
    sub ax,bx
    cmp cx,ax
    jbe .count
    mov cx,ax
.count:
    test cx,cx
    jz .done
.next:
    mov dx,DAC_READ
    mov al,bl
    out dx,al
    mov dx,DAC_DATA
    in al,dx
    xor ah,ah
    imul si,ax,30
    in al,dx
    imul ax,ax,59
    add si,ax
    in al,dx
    xor ah,ah
    imul ax,ax,11
    add ax,si
    add ax,50
    xor dx,dx
    mov si,100
    div si
    mov ah,al
    mov dx,DAC_WRITE
    mov al,bl
    out dx,al
    inc dx
    mov al,ah
    out dx,al
    out dx,al
    out dx,al
    inc bx
    loop .next
.done:
    ret
