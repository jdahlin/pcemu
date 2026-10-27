; VESA BIOS Extensions 2.0 for the Bochs DISPI display interface. All firmware
; code executes in the guest; framebuffer memory remains owned by the device.
; VESA VBE Core Functions 3.0, September 16 1998 (2.0 interfaces retained),
; Function Reference, printed 27..58, PDF 35..66. Version 2.0 is advertised:
; VBE 3.0 refresh control, stereo, scheduled flips and pixel clocks are absent.
; https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=35
vbe_service:
    cmp al,0
    je vbe_controller_information
    cmp al,1
    je vbe_mode_information
    cmp al,2
    je vbe_set_mode
    cmp al,3
    je vbe_get_mode
    cmp al,4
    je vbe_state_service
    cmp al,5
    je vbe_window
    cmp al,6
    je vbe_scanline
    cmp al,7
    je vbe_display_start
    cmp al,8
    je vbe_dac_format
    cmp al,9
    je vbe_palette
    cmp al,0x0a
    je vbe_protected_mode_information
    jmp vbe_failure
vbe_success:
    mov word [in_ax],0x004f
    jmp return_interrupt
vbe_failure:
    mov word [in_ax],0x014f
    jmp return_interrupt
vbe_wrong_mode:
    mov word [in_ax],0x024f
    jmp return_interrupt

; Read DISPI word register AX -> AX, preserving DX. Register selection is a
; device side effect; BIOS calls do not promise to preserve its index latch.
; Bochs Display API 0.6, register access and B0C5 memory-size register.
; https://github.com/bochs-emu/VGABIOS/blob/master/vgabios/vbe_display_api.txt#L129
dispi_read:
    push dx
    mov dx,DISPI_INDEX
    out dx,ax
    mov dx,DISPI_DATA
    in ax,dx
    pop dx
    ret

; Write BX to DISPI register AX, preserving AX/DX and all other registers.
; Bochs Display API register protocol.
; https://github.com/bochs-emu/VGABIOS/blob/master/vgabios/vbe_display_api.txt#L129
dispi_write:
    push ax
    push dx
    mov dx,DISPI_INDEX
    out dx,ax
    mov ax,bx
    mov dx,DISPI_DATA
    out dx,ax
    pop dx
    pop ax
    ret

; Find the eight-byte descriptor for mode AX; return SI and clear CF on match.
; Published modes are explicit ROM data; unsupported requests never set hardware.
; VBE Function 00h/01h mode-list contract, PDF 35..38.
; https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=35
vbe_find_mode:
    mov si,vbe_modes
.next:
    cmp ax,[cs:si]
    je .found
    add si,8
    cmp si,vbe_modes_end
    jb .next
    stc
    ret
.found:
    clc
    ret

; 4F00h writes 256 bytes for legacy callers, or 512 when the caller requests
; VBE2. ROM pointers are far pointers; reserved fields are zero, and the list is
; terminated by FFFFh. No caller buffer beyond the selected length is touched.
; VBE Function 00h, printed 27..29, PDF 35..37.
; https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=35
vbe_controller_information:
    mov cx,256
    cmp di,0xff00
    ja vbe_failure
    cmp dword [es:di],'VBE2'
    jne .length
    cmp di,0xfe00
    ja vbe_failure
    mov cx,512
.length:
    push cx
    push di
    shr cx,1
    xor ax,ax
    rep stosw
    pop di
    pop cx
    mov dword [es:di],'VESA'
    mov word [es:di+4],0x0200
    mov word [es:di+6],vbe_oem_string
    mov [es:di+8],cs
    mov dword [es:di+10],1 ; switchable 6/8-bit DAC, VGA-compatible controller.
    mov word [es:di+14],vbe_mode_list
    mov [es:di+16],cs
    mov ax,10
    call dispi_read
    mov [es:di+18],ax
    cmp cx,512
    jne vbe_success
    mov word [es:di+20],0x0100
    mov word [es:di+22],vbe_vendor
    mov [es:di+24],cs
    mov word [es:di+26],vbe_product
    mov [es:di+28],cs
    mov word [es:di+30],vbe_revision
    mov [es:di+32],cs
    jmp vbe_success

; Discover the PCI framebuffer BAR instead of assuming a host-assigned base.
; The board has one 1234:1111 VGA device on bus 0. Preserve CF8 address selection.
; PCI configuration mechanism 1, PCI 2.2 register layout (BAR0 offset 10h).
; https://github.com/tianocore/edk2/blob/master/MdePkg/Include/IndustryStandard/Pci22.h
vbe_framebuffer_base:
    push ebx
    push ecx
    push edx
    mov dx,0xcf8
    in eax,dx
    push eax
    mov ecx,0x80000000
    xor ebx,ebx
.scan:
    mov eax,ecx
    mov dx,0xcf8
    out dx,eax
    mov dx,0xcfc
    in eax,dx
    cmp eax,0x11111234
    je .found
    add ecx,0x800
    cmp ecx,0x80010000
    jb .scan
    jmp .done
.found:
    mov eax,ecx
    or eax,0x10
    mov dx,0xcf8
    out dx,eax
    mov dx,0xcfc
    in eax,dx
    and eax,0xfffffff0
    mov ebx,eax
.done:
    pop eax
    mov dx,0xcf8
    out dx,eax
    mov eax,ebx
    pop edx
    pop ecx
    pop ebx
    ret

; 4F01h constructs a 256-byte ModeInfoBlock for the requested published mode.
; Both banked and linear memory models are available, with 64-KiB banks and
; exact packed stride. Direct-color component masks describe RGB555/RGB565/BGR.
; VBE Function 01h, printed 30..39, PDF 38..47.
; https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=38
vbe_mode_information:
    cmp di,0xff00
    ja vbe_failure
    mov ax,cx
    and ax,0x3fff
    call vbe_find_mode
    jc vbe_failure
    push di
    mov cx,128
    xor ax,ax
    rep stosw
    pop di
    mov word [es:di],0x009b ; supported, color, graphics, LFB, extended info.
    mov byte [es:di+2],7 ; window A present, readable and writable.
    mov word [es:di+4],64
    mov word [es:di+6],64
    mov word [es:di+8],0xa000
    mov word [es:di+12],vbe_window_far
    mov [es:di+14],cs
    mov ax,[cs:si+2]
    mov [es:di+18],ax
    mov ax,[cs:si+4]
    mov [es:di+20],ax
    mov word [es:di+22],0x1008 ; 8x16 nominal character cells.
    mov byte [es:di+24],1
    mov al,[cs:si+6]
    mov [es:di+25],al
    mov byte [es:di+27],4
    cmp al,8
    je .packed
    mov byte [es:di+27],6
.packed:
    mov byte [es:di+26],1
    movzx eax,byte [cs:si+6]
    add eax,7
    shr eax,3
    movzx ecx,word [cs:si+2]
    mul ecx
    mov [es:di+16],ax
    movzx ecx,word [cs:si+4]
    mul ecx
    mov ecx,eax
    mov ax,10
    call dispi_read
    movzx eax,ax
    shl eax,16
    xor edx,edx
    div ecx
    dec eax
    cmp eax,255
    jbe .pages
    mov eax,255
.pages:
    mov [es:di+29],al
    mov al,[cs:si+6]
    cmp al,8
    je .base
    mov word [es:di+31],0x1008 ; red width 8, position 16.
    mov word [es:di+33],0x0808 ; green width 8, position 8.
    mov word [es:di+35],0x0008 ; blue width 8, position 0.
    cmp al,24
    jae .truecolor
    mov word [es:di+31],0x0a05
    mov word [es:di+33],0x0505
    mov word [es:di+35],0x0005
    cmp al,15
    je .base
    mov word [es:di+31],0x0b05
    mov word [es:di+33],0x0506
    jmp .base
.truecolor:
    cmp al,32
    jne .base
    mov word [es:di+37],0x1808 ; reserved byte, not an alpha channel.
.base:
    call vbe_framebuffer_base
    test eax,eax
    jz vbe_failure
    mov [es:di+40],eax
    jmp vbe_success

; 4F02h validates flags and mode before touching DISPI. BIOS legacy mode calls
; remain available through this entry. Setting a VBE mode resets bank, stride,
; panning and DAC precision; bit 15 preserves framebuffer bytes, bit 14 selects
; the linear framebuffer. VBE Function 02h, printed 40..42, PDF 48..50.
; https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=48
vbe_set_mode:
    test bx,0x3e00
    jnz vbe_failure
    mov ax,bx
    and ax,0x01ff
    cmp ax,0x100
    jb .legacy
    call vbe_find_mode
    jc vbe_failure
    mov ax,4
    xor bx,bx
    call dispi_write
    mov ax,1
    mov bx,[cs:si+2]
    call dispi_write
    mov ax,2
    mov bx,[cs:si+4]
    call dispi_write
    mov ax,3
    movzx bx,byte [cs:si+6]
    call dispi_write
    mov ax,5
    xor bx,bx
    call dispi_write
    mov bx,1
    test word [in_bx],0x4000
    jz .no_lfb
    or bx,0x40
.no_lfb:
    test word [in_bx],0x8000
    jz .clear
    or bx,0x80
.clear:
    mov ax,4
    call dispi_write
    mov byte [MODE],0xff
    mov word [CRTC_BASE],CRTC
    mov ax,[cs:si+2]
    shr ax,3
    mov [COLUMNS],ax
    mov word [CHARACTER_HEIGHT],16
    mov ax,[cs:si+4]
    shr ax,4
    dec ax
    mov [ROWS_MINUS_ONE],al
    mov word [PAGE_BYTES],0xffff
    mov byte [ACTIVE_PAGE],0
    mov word [DISPLAY_START],0
    test byte [MODESET_CONTROL],8
    jnz vbe_success
    mov dx,DAC_WRITE
    xor al,al
    out dx,al
    mov dx,DAC_DATA
    mov si,graphics_dac
    mov cx,768
.palette:
    mov al,[cs:si]
    out dx,al
    inc si
    loop .palette
    jmp vbe_success
.legacy:
    cmp ax,3
    jbe .legacy_ok
    cmp ax,7
    jbe .legacy_ok
    cmp ax,0x0d
    jb vbe_failure
    cmp ax,0x13
    ja vbe_failure
.legacy_ok:
    test word [in_bx],0x8000
    jz .legacy_call
    or al,0x80
.legacy_call:
    xor ah,ah
    int 0x10
    jmp vbe_success

; 4F03h reconstructs the current mode from actual device dimensions/format,
; retaining the LFB flag. There is no hidden mutable ROM or host-side mode state.
; VBE Function 03h, printed 43, PDF 51.
; https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=51
vbe_get_mode:
    mov ax,4
    call dispi_read
    test ax,1
    jz .legacy
    mov di,ax
    mov ax,1
    call dispi_read
    mov cx,ax
    mov ax,2
    call dispi_read
    mov dx,ax
    mov ax,3
    call dispi_read
    mov bx,ax
    mov si,vbe_modes
.next:
    cmp cx,[cs:si+2]
    jne .advance
    cmp dx,[cs:si+4]
    jne .advance
    cmp bl,[cs:si+6]
    je .found
.advance:
    add si,8
    cmp si,vbe_modes_end
    jb .next
    jmp vbe_failure
.found:
    mov ax,[cs:si]
    test di,0x40
    jz .result
    or ax,0x4000
.result:
    mov [in_bx],ax
    jmp vbe_success
.legacy:
    mov al,[MODE]
    xor ah,ah
    jmp .result

; 4F05h controls window A with bank numbers in 64-KiB units. Reject window B,
; invalid banks and calls outside extended framebuffer mode before writing.
; VBE Function 05h, printed 45..47, PDF 53..55.
; https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=53
vbe_window:
    test bl,bl
    jnz vbe_failure
    cmp bh,1
    ja vbe_failure
    mov ax,4
    call dispi_read
    test ax,1
    jz vbe_wrong_mode
    cmp byte [in_bx+1],1
    je .get
    mov ax,10
    call dispi_read
    cmp dx,ax
    jae vbe_failure
    mov bx,dx
    mov ax,5
    call dispi_write
    jmp vbe_success
.get:
    mov ax,5
    call dispi_read
    mov [in_dx],ax
    jmp vbe_success

; Direct far-call window entry has the same register ABI as INT 10h/4F05h.
; VBE ModeInfoBlock.WinFuncPtr, printed 39, PDF 47.
; https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=47
vbe_window_far:
    mov ax,0x4f05
    int 0x10
    retf

; Compute EDI=bytes/pixel and ESI=available framebuffer bytes; require DISPI
; enabled. The caller's interrupt frame preserves all temporary registers.
; VBE packed/direct-color memory layout; Bochs B0C5 size register.
; https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=38
vbe_memory_geometry:
    mov ax,4
    call dispi_read
    test ax,1
    jz .invalid
    mov ax,3
    call dispi_read
    movzx edi,ax
    add edi,7
    shr edi,3
    test edi,edi
    jz .invalid
    mov ax,10
    call dispi_read
    movzx esi,ax
    shl esi,16
    clc
    ret
.invalid:
    stc
    ret

; 4F06h implements pixel/byte stride set, stride query and maximum query.
; Arithmetic uses full 32-bit framebuffer sizes, checks minimum physical width
; and height, and never reports more scan lines than fit in memory.
; VBE Function 06h, printed 48..49, PDF 56..57.
; https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=56
vbe_scanline:
    cmp bl,3
    ja vbe_failure
    call vbe_memory_geometry
    jc vbe_wrong_mode
    cmp byte [in_bx],3
    je .maximum
    cmp byte [in_bx],1
    je .current
    movzx ecx,word [in_cx]
    cmp byte [in_bx],2
    jne .pixels
    mov eax,ecx
    add eax,edi
    dec eax
    xor edx,edx
    div edi
    mov ecx,eax
.pixels:
    mov ax,1
    call dispi_read
    movzx eax,ax
    cmp ecx,eax
    jb vbe_failure
    mov eax,ecx
    mul edi
    test edx,edx
    jnz vbe_failure
    cmp eax,65535
    ja vbe_failure
    test eax,eax
    jz vbe_failure
    mov ebx,eax
    mov eax,esi
    xor edx,edx
    div ebx
    mov ebx,eax
    mov ax,2
    call dispi_read
    movzx eax,ax
    cmp ebx,eax
    jb vbe_failure
    push cx
    xor bx,bx
    mov ax,8
    call dispi_write
    inc ax
    call dispi_write
    pop bx
    mov ax,6
    call dispi_write
.current:
    mov ax,6
    call dispi_read
    movzx ecx,ax
    jmp .result
.maximum:
    mov ax,2
    call dispi_read
    movzx ecx,ax
    imul ecx,edi
    mov eax,esi
    xor edx,edx
    div ecx
    mov ecx,eax
    mov eax,65535
    xor edx,edx
    div edi
    cmp ecx,eax
    jbe .result
    mov ecx,eax
.result:
    mov [in_cx],cx
    mov eax,ecx
    mul edi
    mov [in_bx],ax
    mov ecx,eax
    mov eax,esi
    xor edx,edx
    div ecx
    cmp eax,65535
    jbe .height
    mov eax,65535
.height:
    mov [in_dx],ax
    jmp vbe_success

; Wait for vertical retrace with a finite budget so a broken device cannot
; hang the BIOS. CF means timeout; no mode/pan/palette writes occur on failure.
; VBE Function 07h/09h retrace variants, printed 50 and 54, PDF 58 and 62.
; https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=58
vbe_wait_retrace:
    push cx
    push dx
    mov cx,0xffff
    mov dx,STATUS1
.wait:
    in al,dx
    test al,8
    jnz .ready
    loop .wait
    pop dx
    pop cx
    stc
    ret
.ready:
    pop dx
    pop cx
    clc
    ret

; 4F07h validates the complete visible rectangle before programming either
; coordinate. This prevents partial writes on invalid panning requests.
; VBE Function 07h, printed 50..52, PDF 58..60 (VBE 2.0 subfunctions only).
; https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=58
vbe_display_start:
    cmp bl,1
    je .get
    test bl,0x7f
    jnz vbe_failure
    call vbe_memory_geometry
    jc vbe_wrong_mode
    mov ax,6
    call dispi_read
    movzx ecx,ax
    mov ax,1
    call dispi_read
    movzx eax,ax
    movzx ebx,word [in_cx]
    add eax,ebx
    cmp eax,ecx
    ja vbe_failure
    mov eax,ecx
    mul edi
    mov ecx,eax
    mov eax,esi
    xor edx,edx
    div ecx
    mov ecx,eax
    mov ax,2
    call dispi_read
    movzx eax,ax
    movzx ebx,word [in_dx]
    add eax,ebx
    cmp eax,ecx
    ja vbe_failure
    test byte [in_bx],0x80
    jz .set
    call vbe_wait_retrace
    jc vbe_failure
.set:
    mov ax,8
    mov bx,[in_cx]
    call dispi_write
    inc ax
    mov bx,[in_dx]
    call dispi_write
    jmp vbe_success
.get:
    mov ax,4
    call dispi_read
    test ax,1
    jz vbe_wrong_mode
    mov ax,8
    call dispi_read
    mov [in_cx],ax
    inc ax ; index is not the returned value: explicitly select Y below.
    mov ax,9
    call dispi_read
    mov [in_dx],ax
    mov byte [in_bx+1],0
    jmp vbe_success

; 4F08h switches between 6- and 8-bit DAC precision, retaining mode/pan/bank
; and display bytes. Mode setting always resets precision to six bits.
; VBE Function 08h, printed 53, PDF 61.
; https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=61
vbe_dac_format:
    cmp bl,1
    ja vbe_failure
    je .get
    cmp bh,6
    je .set
    cmp bh,8
    jne vbe_failure
.set:
    mov ax,4
    call dispi_read
    mov bx,ax
    and bx,0xffdf
    cmp byte [in_bx+1],8
    jne .write
    or bx,0x20
.write:
    mov ax,4
    call dispi_write
.get:
    mov ax,4
    call dispi_read
    mov byte [in_bx+1],6
    test ax,0x20
    jz vbe_success
    mov byte [in_bx+1],8
    jmp vbe_success

; 4F09h transfers BGRA palette entries, accepting the primary DAC only. Validate
; the whole buffer/range first, and set reserved bytes to zero on readback.
; VBE Function 09h, printed 54..55, PDF 62..63.
; https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=62
vbe_palette:
    mov al,bl
    and al,0x7f
    cmp al,1
    ja vbe_wrong_mode
    cmp bl,0x81
    je vbe_failure
    movzx eax,dx
    movzx ebx,cx
    add eax,ebx
    cmp eax,256
    ja vbe_failure
    shl ebx,2
    movzx eax,di
    add eax,ebx
    cmp eax,65536
    ja vbe_failure
    test cx,cx
    jz vbe_success
    test byte [in_bx],0x80
    jz .transfer
    call vbe_wait_retrace
    jc vbe_failure
.transfer:
    mov ax,[in_dx]
    mov dx,DAC_WRITE
    cmp byte [in_bx],1
    jne .index
    mov dx,DAC_READ
.index:
    out dx,al
    mov dx,DAC_DATA
.next:
    cmp byte [in_bx],1
    je .read
    mov al,[es:di+2]
    out dx,al
    mov al,[es:di+1]
    out dx,al
    mov al,[es:di]
    out dx,al
    jmp .advance
.read:
    in al,dx
    mov [es:di+2],al
    in al,dx
    mov [es:di+1],al
    in al,dx
    mov [es:di],al
    mov byte [es:di+3],0
.advance:
    add di,4
    loop .next
    jmp vbe_success

; 4F0Ah returns relocatable 32-bit protected-mode code plus required ports.
; The copied code contains no absolute references to ROM data or BIOS RAM.
; VBE Function 0Ah, printed 56..58, PDF 64..66.
; https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=64
vbe_protected_mode_information:
    test bl,bl
    jnz vbe_failure
    mov ax,cs
    mov [in_es],ax
    mov word [in_di],vbe_pm_table
    mov word [in_cx],vbe_pm_end-vbe_pm_table
    jmp vbe_success

vbe_oem_string db 'PCEmu VGA / Bochs DISPI',0
vbe_vendor db 'PCEmu',0
vbe_product db 'Standalone VGA BIOS',0
vbe_revision db '1.0',0

; Relocatable VBE 2.0 protected-mode routines. The table contains relative
; offsets, and the code uses only registers, stack and caller ES data. All
; entries use near RET and preserve ESI/EBP/DS/SS as required by Function 0Ah.
; VBE Function 0Ah and protected-mode calling rules, PDF 64..66.
; https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=64
vbe_pm_table:
    dw vbe_pm_bank-vbe_pm_table
    dw vbe_pm_display_start-vbe_pm_table
    dw vbe_pm_palette-vbe_pm_table
    dw vbe_pm_ports-vbe_pm_table
bits 32
vbe_pm_bank:
    pushad
    mov ebp,esp
    cmp word [ss:ebp+16],0
    jne vbe_pm_failure
    mov ax,4
    call vbe_pm_read
    test ax,1
    jz vbe_pm_failure
    mov ax,10
    call vbe_pm_read
    cmp word [ss:ebp+20],ax
    jae vbe_pm_failure
    mov bx,[ss:ebp+20]
    mov ax,5
    call vbe_pm_write
    jmp vbe_pm_success

; Protected-mode display start is DX:CX in units of four bytes, not pixels.
; Convert to a validated pixel X/Y pair at the current stride and pixel format.
; VBE Function 07h protected-mode ABI, printed 50, PDF 58.
; https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=58
vbe_pm_display_start:
    mov eax,ebx
    and eax,0xff7f
    jnz .bad_entry
    pushad
    mov ebp,esp
    mov ax,4
    call vbe_pm_read
    test ax,1
    jz vbe_pm_failure
    movzx esi,word [ss:ebp+20]
    shl esi,16
    movzx ecx,word [ss:ebp+24]
    or esi,ecx
    mov ax,10
    call vbe_pm_read
    movzx eax,ax
    shl eax,14
    cmp esi,eax
    jae vbe_pm_failure
    shl esi,2
    mov ax,3
    call vbe_pm_read
    movzx edi,ax
    add edi,7
    shr edi,3
    test edi,edi
    jz vbe_pm_failure
    mov ax,6
    call vbe_pm_read
    movzx ecx,ax
    imul ecx,edi
    test ecx,ecx
    jz vbe_pm_failure
    mov eax,esi
    xor edx,edx
    div ecx
    mov esi,eax
    mov eax,edx
    xor edx,edx
    div edi
    test edx,edx
    jnz vbe_pm_failure
    mov edi,eax
    mov ax,1
    call vbe_pm_read
    movzx eax,ax
    add eax,edi
    push eax
    mov ax,6
    call vbe_pm_read
    movzx ebx,ax
    pop eax
    cmp eax,ebx
    ja vbe_pm_failure
    mov ax,10
    call vbe_pm_read
    movzx eax,ax
    shl eax,16
    xor edx,edx
    div ecx
    mov ecx,eax
    mov ax,2
    call vbe_pm_read
    movzx eax,ax
    add eax,esi
    cmp eax,ecx
    ja vbe_pm_failure
    test byte [ss:ebp+16],0x80
    jz .set
    call vbe_pm_wait
    jc vbe_pm_failure
.set:
    mov bx,di
    mov ax,8
    call vbe_pm_write
    mov bx,si
    mov ax,9
    call vbe_pm_write
    jmp vbe_pm_success
.bad_entry:
    mov ax,0x014f
    ret

; Protected-mode primary palette writes consume the same BGRA layout as the
; real-mode entry. Caller ES maps the palette buffer, and EDI is a 32-bit offset.
; VBE Function 09h protected-mode ABI, printed 54..55, PDF 62..63.
; https://read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/vbe3.pdf#page=62
vbe_pm_palette:
    pushad
    mov ebp,esp
    movzx eax,bx
    and eax,0xff7f
    jnz vbe_pm_failure
    movzx eax,dx
    movzx ecx,cx
    add eax,ecx
    cmp eax,256
    ja vbe_pm_failure
    test byte [ss:ebp+16],0x80
    jz .index
    call vbe_pm_wait
    jc vbe_pm_failure
.index:
    mov eax,[ss:ebp+20]
    mov dx,DAC_WRITE
    out dx,al
    mov dx,DAC_DATA
    test ecx,ecx
    jz vbe_pm_success
.next:
    mov al,[es:edi+2]
    out dx,al
    mov al,[es:edi+1]
    out dx,al
    mov al,[es:edi]
    out dx,al
    add edi,4
    dec ecx
    jnz .next
    jmp vbe_pm_success

; Pure port-access helpers shared only by the 32-bit copied code.
; Bochs Display API register protocol; no pointer into the original ROM is used.
; https://github.com/bochs-emu/VGABIOS/blob/master/vgabios/vbe_display_api.txt#L129
vbe_pm_read:
    push edx
    mov dx,DISPI_INDEX
    out dx,ax
    mov dx,DISPI_DATA
    in ax,dx
    pop edx
    ret
vbe_pm_write:
    push eax
    push edx
    mov dx,DISPI_INDEX
    out dx,ax
    mov ax,bx
    mov dx,DISPI_DATA
    out dx,ax
    pop edx
    pop eax
    ret
vbe_pm_wait:
    push ecx
    push edx
    mov ecx,0xffff
    mov dx,STATUS1
.wait:
    in al,dx
    test al,8
    jnz .ready
    dec ecx
    jnz .wait
    pop edx
    pop ecx
    stc
    ret
.ready:
    pop edx
    pop ecx
    clc
    ret
vbe_pm_success:
    mov word [ss:ebp+28],0x004f
    popad
    ret
vbe_pm_failure:
    mov word [ss:ebp+28],0x014f
    popad
    ret
bits 16
vbe_pm_ports:
    dw DISPI_INDEX,DISPI_DATA,DAC_MASK,DAC_READ,DAC_WRITE,DAC_DATA,STATUS1
    dw 0xffff,0xffff
vbe_pm_end:
