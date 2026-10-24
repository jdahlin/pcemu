bits 32
section .text
global micro32
%macro MICRO_START32 1
    mov byte [micro32_pass],0
.case_%1:
    cmp byte [micro32_pass],0
    jne %%warm
    mov al,4
    out 0e7h,al
%%warm:
    mov al,%1
    out 0e6h,al
%if %1 = 36
    xor eax,eax
    mov edi,micro32_destination
    mov ecx,64
    rep stosd
%endif
    in eax,0e4h
    mov ecx,eax
    mov al,1
    out 0e7h,al
%endmacro
%macro MICRO_REPORT32 1
    out 0e5h,eax
    inc byte [micro32_pass]
    cmp byte [micro32_pass],2
    jb .case_%1
%endmacro
%macro MICRO_END32 0
    mov edi,eax
    mov al,2
    out 0e7h,al
    mov eax,edi
%endmacro
micro32:
    push ebx
    push esi
    push edi
    push ebp
    cld
    MICRO_START32 32
    xor eax,eax
.loop_alu:
%rep 16
    add eax,3
%endrep
    dec ecx
    jnz .loop_alu
    MICRO_END32
    MICRO_REPORT32 32
    MICRO_START32 33
    mov dword [micro32_cell],0
.loop_memory:
%rep 8
    mov eax,[micro32_cell]
    add eax,1
    mov [micro32_cell],eax
%endrep
    dec ecx
    jnz .loop_memory
    MICRO_END32
    MICRO_REPORT32 33
    MICRO_START32 34
    xor eax,eax
.loop_branch:
    test ecx,1
    jz .even
    add eax,3
    jmp .joined
.even:
    inc eax
.joined:
    dec ecx
    jnz .loop_branch
    MICRO_END32
    MICRO_REPORT32 34
    MICRO_START32 35
    xor eax,eax
.loop_calls:
%rep 4
    call micro32_increment
%endrep
    dec ecx
    jnz .loop_calls
    MICRO_END32
    MICRO_REPORT32 35
    MICRO_START32 36
    mov ebp,ecx
    shr ebp,6
    jnz .rep_ready
    inc ebp
.rep_ready:
.loop_rep:
    mov esi,micro32_source
    mov edi,micro32_destination
    mov ecx,64
    rep movsd
    dec ebp
    jnz .loop_rep
    mov al,2
    out 0e7h,al
    xor ebx,ebx
    mov esi,micro32_destination
    mov ecx,64
.checksum:
    lodsd
    add ebx,eax
    loop .checksum
    mov eax,ebx
    MICRO_REPORT32 36
    MICRO_START32 37
    xor eax,eax
.loop_counter:
    add eax,3
    loop .loop_counter
    MICRO_END32
    MICRO_REPORT32 37
    MICRO_START32 38
    xor eax,eax
.loop_exchange:
    xchg al,ah
    add ax,1
    xchg al,ah
    loop .loop_exchange
    MICRO_END32
    MICRO_REPORT32 38
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret
micro32_increment:
    add eax,1
    ret
section .data
micro32_pass: db 0
micro32_source: times 64 dd 05a5a5a5ah
section .bss align=4096
micro32_cell: resd 1
micro32_destination: resd 64
section .note.GNU-stack noalloc noexec nowrite progbits
