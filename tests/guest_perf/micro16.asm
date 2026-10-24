; Original 386 real-mode microkernels; primarily 16-bit operands/addressing.
; ECX/EBP hold the outer count so tests can exceed 65535 iterations.
%macro MICRO_START16 1
    mov byte [micro16_pass],0
.case_%1:
    cmp byte [micro16_pass],0
    jne %%warm
    mov al,4
    out 0e7h,al
%%warm:
    mov al,%1
    out 0e6h,al
%if %1 = 20
    xor ax,ax
    mov di,micro16_destination
    mov cx,64
    rep stosw
%endif
    in eax,0e4h
    mov ecx,eax
    mov al,1
    out 0e7h,al
%endmacro
%macro MICRO_REPORT16 1
    out 0e5h,eax
    inc byte [micro16_pass]
    cmp byte [micro16_pass],2
    jb .case_%1
%endmacro
%macro MICRO_END16 0
    movzx edi,ax
    mov al,2
    out 0e7h,al
    mov eax,edi
%endmacro
micro16:
    pushad
    push es
    push ds
    MICRO_START16 21
    xor eax,eax
.loop_counter:
    add ax,3
    a32 loop .loop_counter
    MICRO_END16
    MICRO_REPORT16 21
    MICRO_START16 22
    xor eax,eax
.loop_exchange:
    xchg al,ah
    add ax,1
    xchg al,ah
    a32 loop .loop_exchange
    MICRO_END16
    MICRO_REPORT16 22
    pop es
    cld
    MICRO_START16 16
    xor ax,ax
.loop_alu:
%rep 16
    add ax,3
%endrep
    dec ecx
    jnz .loop_alu
    MICRO_END16
    MICRO_REPORT16 16
    MICRO_START16 17
    mov word [micro16_cell],0
.loop_memory:
%rep 8
    mov ax,[micro16_cell]
    add ax,1
    mov [micro16_cell],ax
%endrep
    dec ecx
    jnz .loop_memory
    MICRO_END16
    MICRO_REPORT16 17
    MICRO_START16 18
    xor ax,ax
.loop_branch:
    test cx,1
    jz .even
    add ax,3
    jmp .joined
.even:
    inc ax
.joined:
    dec ecx
    jnz .loop_branch
    MICRO_END16
    MICRO_REPORT16 18
    MICRO_START16 19
    xor ax,ax
.loop_calls:
%rep 4
    call micro16_increment
%endrep
    dec ecx
    jnz .loop_calls
    MICRO_END16
    MICRO_REPORT16 19
    MICRO_START16 20
    mov ebp,ecx
    shr ebp,6
    jnz .rep_ready
    inc ebp
.rep_ready:
.loop_rep:
    mov si,micro16_source
    mov di,micro16_destination
    mov cx,64
    rep movsw
    dec ebp
    jnz .loop_rep
    mov al,2
    out 0e7h,al
    xor eax,eax
    mov si,micro16_destination
    mov cx,64
.checksum:
    movzx edx,word [si]
    add eax,edx
    add si,2
    loop .checksum
    MICRO_REPORT16 20
    pop es
    popad
    ret
micro16_increment:
    add ax,1
    ret
micro16_pass: db 0
align 2
micro16_cell: dw 0
micro16_source: times 64 dw 05a5ah
micro16_destination: times 64 dw 0
