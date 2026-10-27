; VGA/VBE save-state format is private to this ROM, as permitted by the BIOS
; interfaces. The fixed 1024-byte buffer has independent hardware, BIOS, DAC
; and DISPI sections; selectors restore only requested sections. Video memory
; is deliberately excluded. Bad signatures/masks are rejected before mutation.
; IBM BIOS Interface AH=1Ch, printed 2-44; VBE Function 04h, PDF 52.
; https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=52
%define STATE_BYTES 1024
%define STATE_HARDWARE 16
%define STATE_BIOS 96
%define STATE_DAC 160
%define STATE_DISPI 960
vga_state_service:
    test cx,0xfff8
    jnz .failure
    cmp al,2
    ja .failure
    mov dl,al
    jmp state_service
.failure:
    jmp return_interrupt
vbe_state_service:
    test cx,0xfff0
    jnz vbe_failure
    cmp dl,2
    ja vbe_failure
state_service:
    test dl,dl
    jnz .buffer
    mov word [in_bx],0
    test cx,cx
    jz state_success
    mov word [in_bx],STATE_BYTES/64
    jmp state_success
.buffer:
    test cx,cx
    jz state_success
    cmp bx,65536-STATE_BYTES
    ja state_failure
    cmp dl,2
    je .restore
    mov di,bx
    mov cx,STATE_BYTES/2
    xor ax,ax
    rep stosw
    mov bx,[in_bx]
    mov dword [es:bx],'PVGA'
    mov word [es:bx+4],1
    mov ax,[in_cx]
    mov [es:bx+6],ax
    test ax,1
    jz .save_bios
    call state_save_hardware
.save_bios:
    test word [in_cx],2
    jz .save_dac
    call state_save_bios
.save_dac:
    test word [in_cx],4
    jz .save_dispi
    call state_save_dac
.save_dispi:
    test word [in_cx],8
    jz state_success
    call state_save_dispi
    jmp state_success
.restore:
    cmp dword [es:bx],'PVGA'
    jne state_failure
    cmp word [es:bx+4],1
    jne state_failure
    mov ax,[es:bx+6]
    and ax,[in_cx]
    cmp ax,[in_cx]
    jne state_failure
    test ax,1
    jz .restore_bios
    call state_restore_hardware
.restore_bios:
    test word [in_cx],2
    jz .restore_dispi
    call state_restore_bios
.restore_dispi:
    test word [in_cx],8
    jz .restore_dac
    call state_restore_dispi
.restore_dac:
    test word [in_cx],4
    jz .restore_index
    call state_restore_dac
.restore_index:
    test word [in_cx],8
    jz state_success
    mov bx,[in_bx]
    mov ax,[es:bx+STATE_DISPI+22]
    mov dx,DISPI_INDEX
    out dx,ax
state_success:
    cmp byte [in_ax+1],0x4f
    je vbe_success
    mov byte [in_ax],0x1c
    jmp return_interrupt
state_failure:
    cmp byte [in_ax+1],0x4f
    je vbe_failure
    jmp return_interrupt

; Capture all defined indexed VGA register groups, miscellaneous output and
; index latches, then restore the latches changed by reading the groups.
; IBM 42G2193 VGA register definitions, PDF 65..129.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=65
state_save_hardware:
    mov di,[in_bx]
    add di,STATE_HARDWARE
    mov dx,0x3cc
    in al,dx
    stosb
    mov dx,SEQUENCER
    in al,dx
    stosb
    mov dx,GRAPHICS
    in al,dx
    stosb
    mov dx,CRTC
    test byte [es:di-3],1
    jnz .crtc
    mov dx,0x3b4
.crtc:
    in al,dx
    stosb
    mov dx,ATTRIBUTE
    in al,dx
    stosb
    mov dx,SEQUENCER
    mov cx,5
    call state_read_group
    mov dx,GRAPHICS
    mov cx,9
    call state_read_group
    mov dx,CRTC
    mov bx,[in_bx]
    test byte [es:bx+STATE_HARDWARE],1
    jnz .crtc_group
    mov dx,0x3b4
.crtc_group:
    mov cx,25
    call state_read_group
    xor bx,bx
.attributes:
    mov ax,bx
    call read_attribute
    mov al,ah
    stosb
    inc bx
    cmp bx,21
    jb .attributes
    call state_restore_indexes
    ret

; Transfer one indexed register group to ES:DI. The descriptor's count is
; independent of the BIOS BDA. H indexed interfaces, PDF 65 onward.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=65
state_read_group:
    xor bx,bx
.next:
    mov al,bl
    call read_indexed_register
    mov al,ah
    stosb
    inc bx
    loop .next
    ret

; Restore each register group from ES:SI, unlocking CRTC protected registers
; before writing their saved values. H Sequencer Reset/CRTC protection, PDF 72/97.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=72
state_restore_hardware:
    mov si,[in_bx]
    add si,STATE_HARDWARE
    mov al,[es:si]
    mov dx,MISC_OUTPUT
    out dx,al
    add si,5
    mov dx,SEQUENCER
    mov cx,5
    call state_write_group
    mov dx,GRAPHICS
    mov cx,9
    call state_write_group
    mov bx,[in_bx]
    mov dx,CRTC
    test byte [es:bx+STATE_HARDWARE],1
    jnz .crtc
    mov dx,0x3b4
.crtc:
    mov ax,0x0011
    out dx,ax
    mov cx,25
    call state_write_group
    xor bx,bx
.attributes:
    mov al,bl
    mov ah,[es:si]
    inc si
    call write_attribute
    inc bx
    cmp bx,21
    jb .attributes
    call state_restore_indexes
    ret

; Write one indexed VGA group from the saved buffer; preserve segment mapping.
; H indexed-register programming, PDF 65 onward.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=65
state_write_group:
    xor bx,bx
.next:
    mov al,bl
    mov ah,[es:si]
    inc si
    out dx,ax
    inc bx
    loop .next
    ret

; Restore selector latches without changing register contents. Attribute writes
; first reset the flip-flop and restore the saved Palette Address Source bit.
; H Attribute Address Register/CRTC/SR/GR indices, PDF 65 onward.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=65
state_restore_indexes:
    mov bx,[in_bx]
    mov dx,SEQUENCER
    mov al,[es:bx+STATE_HARDWARE+1]
    out dx,al
    mov dx,GRAPHICS
    mov al,[es:bx+STATE_HARDWARE+2]
    out dx,al
    mov dx,CRTC
    test byte [es:bx+STATE_HARDWARE],1
    jnz .crtc
    mov dx,0x3b4
.crtc:
    mov al,[es:bx+STATE_HARDWARE+3]
    out dx,al
    mov dx,STATUS1
    in al,dx
    mov dx,ATTRIBUTE
    mov al,[es:bx+STATE_HARDWARE+4]
    out dx,al
    ret

; Save BDA video fields and font/parameter pointers. No keyboard/timer/disk
; fields are included. IBM BIOS Interface video data, PDF 144/146.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=144
state_save_bios:
    push fs
    mov di,[in_bx]
    add di,STATE_BIOS
    mov si,MODE
    mov cx,30
    rep movsb
    mov si,ROWS_MINUS_ONE
    mov cx,7
    rep movsb
    xor ax,ax
    mov fs,ax
    mov eax,[fs:0x74]
    stosd
    mov eax,[fs:0x7c]
    stosd
    mov eax,[fs:0x10c]
    stosd
    mov eax,[VIDEO_SAVE_POINTER]
    stosd
    pop fs
    ret

; Restore only the video BDA and associated pointers from the saved image.
; IBM BIOS Interface video data and ROM tables, PDF 144..146.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/PS2_and_PC_BIOS_Interface_Technical_Reference_Apr87.pdf#page=144
state_restore_bios:
    push fs
    mov si,[in_bx]
    add si,STATE_BIOS
    push ds
    push es
    mov ax,es
    mov ds,ax
    mov ax,0x40
    mov es,ax
    mov di,MODE
    mov cx,30
    rep movsb
    mov di,ROWS_MINUS_ONE
    mov cx,7
    rep movsb
    pop es
    pop ds
    xor ax,ax
    mov fs,ax
    mov eax,[es:si]
    mov [fs:0x74],eax
    mov eax,[es:si+4]
    mov [fs:0x7c],eax
    mov eax,[es:si+8]
    mov [fs:0x10c],eax
    mov eax,[es:si+12]
    mov [VIDEO_SAVE_POINTER],eax
    pop fs
    ret

; Save DAC components, mask, address/state and precision. Reading the DAC
; changes its access state, so restore the saved address/direction afterward.
; H Video DAC to System Interface, printed 2-101..102, PDF 127..128.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=127
state_save_dac:
    mov bx,[in_bx]
    mov dx,DAC_MASK
    in al,dx
    mov [es:bx+STATE_DAC+768],al
    mov dx,DAC_WRITE
    in al,dx
    mov [es:bx+STATE_DAC+769],al
    mov dx,DAC_READ
    in al,dx
    mov [es:bx+STATE_DAC+770],al
    mov ax,4
    call dispi_read
    and al,0x20
    mov [es:bx+STATE_DAC+771],al
    mov di,bx
    add di,STATE_DAC
    mov dx,DAC_READ
    xor al,al
    out dx,al
    mov dx,DAC_DATA
    mov cx,768
.next:
    in al,dx
    stosb
    loop .next
    call state_restore_dac_index
    ret

; Restore DAC precision before RGB writes, then mask and access direction.
; H DAC protocol, PDF 127; Bochs 8-bit DAC enable bit, Display API B0C3.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=127
state_restore_dac:
    mov si,[in_bx]
    mov ax,4
    call dispi_read
    and ax,0xffdf
    mov bx,ax
    or bl,[es:si+STATE_DAC+771]
    mov ax,4
    call dispi_write
    add si,STATE_DAC
    mov dx,DAC_WRITE
    xor al,al
    out dx,al
    mov dx,DAC_DATA
    mov cx,768
.next:
    mov al,[es:si]
    inc si
    out dx,al
    loop .next
    mov bx,[in_bx]
    mov al,[es:bx+STATE_DAC+768]
    mov dx,DAC_MASK
    out dx,al
    call state_restore_dac_index
    ret

; Restore the DAC's saved component-stream address and read/write direction.
; H DAC State and Address registers, PDF 127..128.
; https://bitsavers.trailing-edge.com/pdf/ibm/pc/ps2/42G2193_PS2_Hardware_Interface_Technical_Reference_Video_Subsystems_Sep92.pdf#page=127
state_restore_dac_index:
    mov bx,[in_bx]
    mov dx,DAC_WRITE
    cmp byte [es:bx+STATE_DAC+770],3
    jne .address
    mov dx,DAC_READ
.address:
    mov al,[es:bx+STATE_DAC+769]
    out dx,al
    ret

; Save defined DISPI registers and its index latch. Virtual height/memory size
; are read-only but recorded for a deterministic format and diagnostics.
; Bochs Display API, register definitions B0C0..B0C5.
; https://github.com/bochs-emu/VGABIOS/blob/master/vgabios/vbe_display_api.txt
state_save_dispi:
    mov di,[in_bx]
    add di,STATE_DISPI
    mov dx,DISPI_INDEX
    in ax,dx
    mov [es:di+22],ax
    xor cx,cx
.next:
    mov ax,cx
    call dispi_read
    stosw
    inc cx
    cmp cx,11
    jb .next
    mov bx,[in_bx]
    mov ax,[es:bx+STATE_DISPI+22]
    mov dx,DISPI_INDEX
    out dx,ax
    ret

; Restore geometry while disabled, then enable with no-clear, restore bank/pan
; and finally the exact enable flags without a disabled-to-enabled transition.
; Framebuffer bytes are never erased by save/restore. Bochs Display API enable.
; https://github.com/bochs-emu/VGABIOS/blob/master/vgabios/vbe_display_api.txt#L164
state_restore_dispi:
    mov si,[in_bx]
    add si,STATE_DISPI
    xor bx,bx
    mov ax,4
    call dispi_write
    mov ax,0
    mov bx,[es:si]
    call dispi_write
    mov cx,1
.geometry:
    mov ax,cx
    mov bx,cx
    shl bx,1
    mov bx,[es:si+bx]
    call dispi_write
    inc cx
    cmp cx,4
    jb .geometry
    mov ax,4
    mov bx,[es:si+8]
    or bx,0x80
    call dispi_write
    mov ax,6
    mov bx,[es:si+12]
    call dispi_write
    mov ax,5
    mov bx,[es:si+10]
    call dispi_write
    mov ax,8
    mov bx,[es:si+16]
    call dispi_write
    mov ax,9
    mov bx,[es:si+18]
    call dispi_write
    mov ax,4
    mov bx,[es:si+8]
    call dispi_write
    ret
