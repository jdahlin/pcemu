; RAM-only boot-sector integration probe. Runs unchanged under PCEmu and QEMU;
; it checks the installed vector, real INT 10h calls and VGA text memory.
bits 16
org 0x7c00
; The first sector loads a small RAM-only validation payload. No DOS or guest
; installation is needed, and all media are temporary and read-only.
bootstrap:
    cli
    xor ax,ax
    mov ss,ax
    mov sp,0x7c00
    mov ds,ax
    mov es,ax
    mov bx,stage2
    mov ax,0x0200+stage2_sectors
    mov cx,2
    xor dh,dh
    int 0x13
    jc .failed
    jmp stage2
.failed:
    mov al,'L'
    out 0xe9,al
    mov al,0x11
    out 0xf4,al
    hlt
    jmp $
times 510-($-$$) db 0
dw 0xaa55
stage2:
probe_start:
    cli
    xor ax,ax
    mov ss,ax
    mov sp,0x7c00
    mov ds,ax
    cld
    mov bp,1
    mov ax,[0x42]
    cmp ax,0xc000
    jb fail
    cmp ax,0xc400
    jae fail
    mov ax,3
    int 0x10
    mov ax,0x0200
    xor bx,bx
    mov dx,0x0205
    int 0x10
    mov ax,0x0951
    mov bl,0x1e
    mov cx,3
    int 0x10
    mov bp,2
    mov ax,0x0800
    int 0x10
    cmp ax,0x1e51
    jne fail
    mov bp,3
    mov ax,0x0300
    int 0x10
    cmp dx,0x0205
    jne fail
    mov bp,4
    mov ax,0xb800
    mov es,ax
    cmp word [es:2*160+5*2],0x1e51
    jne fail
    cmp word [es:2*160+7*2],0x1e51
    jne fail
    mov bp,5
    mov ax,0x12
    int 0x10
    mov ax,0x0c0b
    xor bx,bx
    mov cx,639
    mov dx,479
    int 0x10
    mov ax,0x0d00
    int 0x10
    cmp al,11
    jne fail
    mov bp,6
    mov ax,0x13
    int 0x10
    mov ax,0x0cfe
    mov cx,319
    mov dx,199
    int 0x10
    mov ax,0x0d00
    int 0x10
    cmp al,0xfe
    jne fail
    mov bp,10
    mov ax,4
    int 0x10
    mov ax,0xb800
    mov es,ax
    mov byte [es:0],0xe4
    mov ax,0x0d00
    xor bx,bx
    xor cx,cx
    xor dx,dx
    int 0x10
    cmp al,3
    jne fail
    mov ax,0x0c01
    int 0x10
    cmp byte [es:0],0x64
    jne fail
    mov ax,6
    int 0x10
    mov byte [es:0],0x80
    mov ax,0x0d00
    int 0x10
    cmp al,1
    jne fail
    mov ax,0x0c81
    int 0x10
    cmp byte [es:0],0
    jne fail
    mov bp,7
    mov ax,0x1000
    mov es,ax
    xor di,di
    mov dword [es:di],'VBE2'
    mov ax,0x4f00
    int 0x10
    cmp ax,0x004f
    jne fail
    cmp dword [es:di],'VESA'
    jne fail
    cmp word [es:di+4],0x0200
    jne fail
    mov ax,0x4f01
    mov cx,0x142
    int 0x10
    cmp ax,0x004f
    jne fail
    cmp word [es:di+18],640
    jne fail
    cmp word [es:di+20],480
    jne fail
    cmp dword [es:di+40],0
    je fail
    mov ax,0x4f02
    mov bx,0x4142
    int 0x10
    cmp ax,0x004f
    jne fail
    mov ax,0x4f06
    xor bx,bx
    mov cx,800
    int 0x10
    cmp ax,0x004f
    jne fail
    cmp bx,3200
    jne fail
    mov ax,0x4f07
    xor bx,bx
    mov cx,8
    mov dx,16
    int 0x10
    cmp ax,0x004f
    jne fail
    mov ax,0x4f08
    mov bx,0x0800
    int 0x10
    cmp ax,0x004f
    jne fail
    mov dx,0x3c8
    mov al,50
    out dx,al
    mov dx,0x3c7
    in al,dx
    test al,al
    jnz fail
    mov al,50
    out dx,al
    in al,dx
    cmp al,3
    jne fail
    mov bp,8
    mov ax,0x4f04
    mov bx,0x0800
    mov cx,15
    mov dx,1
    int 0x10
    cmp ax,0x004f
    jne fail
    mov ax,3
    int 0x10
    mov ax,0x4f04
    mov bx,0x0800
    mov cx,15
    mov dx,2
    int 0x10
    cmp ax,0x004f
    jne fail
    mov dx,0x3c7
    in al,dx
    cmp al,3
    jne fail
    mov ax,0x4f03
    int 0x10
    cmp ax,0x004f
    jne fail
    cmp bx,0x4142
    jne fail
    mov bp,9
    mov ax,0x4f0a
    xor bx,bx
    int 0x10
    cmp ax,0x004f
    jne fail
    push ds
    mov ax,es
    mov ds,ax
    mov si,di
    mov ax,0x2000
    mov es,ax
    xor di,di
    cld
    rep movsb
    pop ds
    lgdt [gdtr]
    mov eax,cr0
    or eax,1
    mov cr0,eax
    jmp dword 8:protected_entry
bits 32
protected_entry:
    mov ax,16
    mov ds,ax
    mov es,ax
    mov ss,ax
    mov esp,0x70000
    mov ebp,9
    movzx eax,word [0x20000]
    add eax,0x20000
    xor ebx,ebx
    mov edx,2
    call eax
    cmp ax,0x004f
    jne protected_fail
    mov dx,0x1ce
    mov ax,5
    out dx,ax
    mov dx,0x1cf
    in ax,dx
    cmp ax,2
    jne protected_fail
    movzx eax,word [0x20002]
    add eax,0x20000
    xor ebx,ebx
    mov ecx,(32*3200+16*4)/4
    xor edx,edx
    call eax
    cmp ax,0x004f
    jne protected_fail
    mov dx,0x1ce
    mov ax,8
    out dx,ax
    mov dx,0x1cf
    in ax,dx
    cmp ax,16
    jne protected_fail
    movzx eax,word [0x20004]
    add eax,0x20000
    xor ebx,ebx
    mov ecx,1
    mov edx,200
    mov edi,pm_palette_data
    call eax
    cmp ax,0x004f
    jne protected_fail
    mov dx,0x3c7
    mov al,200
    out dx,al
    mov dx,0x3c9
    in al,dx
    cmp al,50
    jne protected_fail
    jmp word 0x18:protected_exit
protected_fail:
    mov byte [probe_failed],1
    jmp word 0x18:protected_exit
bits 16
protected_exit:
    mov ax,0x20
    mov ds,ax
    mov es,ax
    mov ss,ax
    mov eax,cr0
    and eax,0xfffffffe
    mov cr0,eax
    jmp 0:real_exit
real_exit:
    xor ax,ax
    mov ds,ax
    mov es,ax
    mov ss,ax
    mov sp,0x7c00
    cmp byte [probe_failed],0
    jne fail
    call debug_trap_contract
    call raster_calibration
    call copper_calibration
    mov ax,3
    int 0x10
    mov si,pass_text
    call print
    mov al,0x10
    out 0xf4,al ; QEMU isa-debug-exit: exit status 33.
    hlt
    jmp $
fail:
    mov ax,bp
    add al,'0'
    out 0xe9,al
    mov si,fail_text
    call print
    mov al,0x11
    out 0xf4,al
    hlt
    jmp $
; Packed DOS executables use TF/INT 1 to decrypt their next instruction.
; A trap is delivered after MOV AX and must save its following IP.
debug_trap_contract:
    mov bp,8
    mov word [4],debug_trap_handler
    mov word [6],0
    pushf
    pop ax
    or ax,0x100
    push ax
    popf
    mov ax,0x1234
.after_step:
    cmp ax,0x1234
    jne fail
    cmp word [debug_return_ip],.after_step
    jne fail
    pushf
    pop ax
    test ax,0x100
    jnz fail
    ret
debug_trap_handler:
    push bp
    mov bp,sp
    push ax
    mov ax,[ss:bp+2]
    mov [debug_return_ip],ax
    and word [ss:bp+6],0xfeff
    pop ax
    pop bp
    iret
debug_return_ip dw 0
; The shipped demo reissues mode 3 before reading the counting element.
; A control word must not reload that element from the initial count.
copper_calibration:
    mov bp,9
    mov al,0x36
    out 0x43,al
    mov al,0xff
    out 0x40,al
    out 0x40,al
    call retrace_edge
    mov al,0x36
    out 0x43,al
    mov al,0xff
    out 0x40,al
    out 0x40,al
    call retrace_edge
    mov al,0x36
    out 0x43,al
    in al,0x40
    mov ah,al
    in al,0x40
    xchg al,ah
    neg ax
    shr ax,1
    cmp ax,16000
    jb fail
    cmp ax,18000
    ja fail
    ret
; Like Second Reality's copper synchronizer, measure successive VGA retraces
; using a latched PIT countdown. Mode 13h is about 70 Hz: roughly 17,025 PIT
; ticks per frame. Read-count parity would give a near-zero interval and fail.
; CLI keeps BIOS IRQ0 bookkeeping out of this bounded RAM-only probe.
raster_calibration:
    mov bp,9
    mov ax,0x13
    int 0x10
    mov al,0x34
    out 0x43,al
    xor al,al
    out 0x40,al
    out 0x40,al
    mov di,3
    call retrace_edge
    call latch_timer
    mov si,ax
.next:
    call retrace_edge
    call latch_timer
    mov bx,si
    sub bx,ax
    mov si,ax
    cmp bx,16000
    jb fail
    cmp bx,18000
    ja fail
    dec di
    jnz .next
    ret
; Poll a full low-to-high edge with a watchdog independent of the PIT.
retrace_edge:
    mov dx,0x3da
    mov ecx,10000000
.low:
    in al,dx
    test al,8
    jz .wait_high
    a32 loop .low
    jmp fail
.wait_high:
    mov ecx,10000000
.high:
    in al,dx
    test al,8
    jnz .done
    a32 loop .high
    jmp fail
.done:
    ret
; Counter-latch command freezes both bytes despite intervening I/O time.
latch_timer:
    xor al,al
    out 0x43,al
    in al,0x40
    mov ah,al
    in al,0x40
    xchg al,ah
    ret
; Print through the tested BIOS and the emulator-independent debug console.
print:
    lodsb
    test al,al
    jz .done
    out 0xe9,al
    mov ah,0x0e
    xor bx,bx
    int 0x10
    jmp print
.done:
    ret
pass_text db 'PCEMU-VGA-PASS',0
fail_text db 'PCEMU-VGA-FAIL',0
align 8
gdt:
    dq 0
    dq 0x00cf9a000000ffff
    dq 0x00cf92000000ffff
    dq 0x00009a000000ffff
    dq 0x000092000000ffff
gdtr:
    dw $-gdt-1
    dd gdt
probe_failed db 0
pm_palette_data db 10,30,50,0
probe_end:
stage2_sectors equ (probe_end-stage2+511)/512
times ((-($-$$)) & 511) db 0
