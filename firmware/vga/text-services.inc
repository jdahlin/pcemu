; Text memory follows the active color/monochrome aperture. The segment is an
; output; callers retain their page-relative byte offsets. IBM 42G2193, memory
; organization, PDF page 38.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=38
text_memory_segment:
    mov ax,0xb800
    cmp byte [MODE],7
    jne .color
    mov ax,0xb000
.color:
    mov es,ax
    ret

; AH=13h writes a counted string to BH's page, optionally with per-character
; attributes and optionally updating its cursor. Controls follow teletype rules.
; IBM BIOS Interface, printed 2-36..37, PDF 51..52.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=51
write_string:
    cmp al,3
    ja return_interrupt
    call validate_text_page
    jc return_interrupt
    cmp dh,[ROWS_MINUS_ONE]
    ja return_interrupt
    cmp dl,[COLUMNS]
    jae return_interrupt
    test cx,cx
    jz .empty
    push fs
    mov ax,[in_es]
    mov fs,ax
    mov si,[ss:bp]
    xor ax,ax
    mov al,bh
    shl ax,1
    mov di,ax
    push word [CURSORS+di]
    mov ax,0x0200
    int 0x10
.next:
    mov al,[fs:si]
    inc si
    test byte [in_ax],2
    jz .attribute
    mov bl,[fs:si]
    inc si
.attribute:
    cmp al,7
    je .advance
    cmp al,8
    je .backspace
    cmp al,13
    je .carriage
    cmp al,10
    je .newline
    push cx
    mov ah,9
    mov cx,1
    int 0x10
    pop cx
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
    jbe .set_cursor
    ; The window service scrolls the BDA-selected page. Temporarily selecting
    ; BH here does not alter the CRTC start or the visible display page.
    push ax
    push bx
    push cx
    push dx
    xor ax,ax
    mov al,[ACTIVE_PAGE]
    push ax
    mov [ACTIVE_PAGE],bh
    mov bh,bl
    mov ax,0x0601
    xor cx,cx
    mov dh,[ROWS_MINUS_ONE]
    mov dl,[COLUMNS]
    dec dl
    int 0x10
    pop ax
    mov [ACTIVE_PAGE],al
    pop dx
    pop cx
    pop bx
    pop ax
    mov dh,[ROWS_MINUS_ONE]
.set_cursor:
    mov ax,0x0200
    int 0x10
.advance:
    dec cx
    jnz .next
    pop dx
    test byte [in_ax],1
    jnz .done
    mov ax,0x0200
    int 0x10
.done:
    pop fs
.empty:
    jmp return_interrupt

; Text pages must fit the 32-KiB color/monochrome aperture. Font resizing can
; reduce the number of usable pages below eight. Return CF for an invalid page.
; IBM BIOS Interface mode/page table, PDF 26; defensive bounds are board policy.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=26
validate_text_page:
    push ax
    push dx
    cmp bh,8
    jae .invalid
    xor ax,ax
    mov al,bh
    inc ax
    mul word [PAGE_BYTES]
    test dx,dx
    jnz .invalid
    cmp ax,0x8000
    ja .invalid
    pop dx
    pop ax
    clc
    ret
.invalid:
    pop dx
    pop ax
    stc
    ret
