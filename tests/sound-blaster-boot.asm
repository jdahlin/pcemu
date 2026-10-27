; RAM-only DSP/8237/PIC integration test; runs unchanged with QEMU SB16.
bits 16
org 0x7c00
    cli
    xor ax,ax
    mov ss,ax
    mov sp,0x7c00
    mov ds,ax
    mov es,ax
    mov bx,stage2
    mov ax,0x0201
    mov cx,2
    xor dh,dh
    int 0x13
    jc fail_boot
    jmp stage2
fail_boot:
    mov al,'L'
    out 0xe9,al
    hlt
    jmp $
times 510-($-$$) db 0
dw 0xaa55
stage2:
    cli
    xor ax,ax
    mov ds,ax
    mov es,ax
    mov word [0x0f*4],irq7
    mov word [0x0f*4+2],0
    mov al,0x7f
    out 0x21,al
    mov bp,1
    mov dx,0x226
    mov al,1
    out dx,al
    ; At least 3 us on hardware; longer in both virtual clock domains.
    mov cx,100
.delay:
    loop .delay
    xor al,al
    out dx,al
    call read_dsp
    cmp al,0xaa
    jne fail
    mov al,0xe1
    call write_dsp
    call read_dsp
    cmp al,2
    jb fail
    call read_dsp
    mov bp,2
    ; Eight-byte automatic DMA buffer, four-byte DSP interrupt blocks.
    mov al,5
    out 0x0a,al
    xor al,al
    out 0x0c,al
    mov ax,samples
    out 2,al
    mov al,ah
    out 2,al
    xor al,al
    out 0x83,al
    out 0x0c,al
    mov al,7
    out 3,al
    xor al,al
    out 3,al
    mov al,0x59
    out 0x0b,al
    mov al,1
    out 0x0a,al
    mov al,0x40
    call write_dsp
    mov al,156
    call write_dsp
    mov al,0x48
    call write_dsp
    mov al,3
    call write_dsp
    xor al,al
    call write_dsp
    mov al,0xd1
    call write_dsp
    mov al,0x1c
    call write_dsp
    mov bp,3
    sti
    mov ecx,1000000000
.wait:
    cmp byte [irq_count],3
    jae .done
%ifdef WAIT_WITH_HLT
    hlt
%endif
    a32 loop .wait
    jmp fail
.done:
    mov bp,4
    cli
    mov al,0xd0
    call write_dsp
    xor al,al
    out 0x0c,al
    in al,2
    mov bl,al
    in al,2
    mov bh,al
    cmp bx,samples
    jb fail
    cmp bx,samples+8
    jae fail
    mov si,pass_text
    mov al,0x10
    jmp finish
fail:
    cli
    mov ax,bp
    add al,'0'
    out 0xe9,al
    mov al,[irq_count]
    add al,'0'
    out 0xe9,al
    mov si,fail_text
    mov al,0x11
finish:
    mov bl,al
.print:
    lodsb
    test al,al
    jz .exit
    out 0xe9,al
    mov ah,0x0e
    push bx
    xor bx,bx
    int 0x10
    pop bx
    jmp .print
.exit:
    mov al,bl
    out 0xf4,al
    hlt
    jmp $
irq7:
    push ax
    push dx
    mov dx,0x22e
    in al,dx
    inc byte [cs:irq_count]
    mov al,0x20
    out 0x20,al
    pop dx
    pop ax
    iret
write_dsp:
    push ax
    mov dx,0x22c
    mov ecx,10000000
.wait:
    in al,dx
    test al,0x80
    jz .ready
    a32 loop .wait
    jmp fail
.ready:
    pop ax
    out dx,al
    ret
read_dsp:
    mov dx,0x22e
    mov ecx,10000000
.wait:
    in al,dx
    test al,0x80
    jnz .ready
    a32 loop .wait
    jmp fail
.ready:
    mov dx,0x22a
    in al,dx
    ret
irq_count db 0
samples db 0,64,128,192,255,192,128,64
pass_text db 'PCEMU-SB-PASS',0
fail_text db 'PCEMU-SB-FAIL',0
times 1024-($-$$) db 0
