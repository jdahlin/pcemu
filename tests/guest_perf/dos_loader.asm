; Original COM loader. Run only on the private minimal-DOS benchmark disk:
; no HIMEM/EMM386/TSRs are loaded, and extended memory 1-2 MiB is private.
bits 16
org 100h
start:
    pushf
    pop word [saved_flags]
    mov [saved_ss],ss
    mov [saved_sp],sp
    mov ax,cs
    mov [real_jump+2],ax
    mov [saved_ds],ds
    mov [saved_es],es
    mov [saved_fs],fs
    mov [saved_gs],gs
    xor ebp,ebp
    mov bp,cs
    shl ebp,4
    mov eax,ebp
    add eax,gdt
    mov [gdtr+2],eax
    mov eax,ebp
    mov [code16+2],ax
    shr eax,16
    mov [code16+4],al
    mov [code16+7],ah
    mov eax,ebp
    add eax,protected32
    mov [protected_jump],eax
    mov ebx,ebp
    add ebx,payload
    in al,21h
    mov [saved_pic],al
    in al,0a1h
    mov [saved_pic+1],al
    cli
    mov al,0ffh
    out 21h,al
    out 0a1h,al
    in al,92h
    and al,0feh
    or al,2
    out 92h,al
    sgdt [saved_gdtr]
    lgdt [gdtr]
    mov eax,cr0
    mov [saved_cr0],eax
    or eax,1
    mov cr0,eax
    jmp dword far [protected_jump]
bits 32
protected32:
    mov ax,10h
    mov ds,ax
    mov es,ax
    mov fs,ax
    mov gs,ax
    mov ss,ax
    mov esp,1f0000h
    cld
    xor eax,eax
    mov edi,100000h
    mov ecx,0f0000h/4
    rep stosd
    mov esi,ebx
    mov edi,100000h
    mov ecx,payload_end-payload
    rep movsb
    mov eax,100000h
    call eax
    jmp word 18h:protected16
bits 16
protected16:
    ; Clear the 32-bit data/stack segment caches before returning to real mode.
    mov ax,20h
    mov ds,ax
    mov es,ax
    mov fs,ax
    mov gs,ax
    mov ss,ax
    mov eax,[cs:saved_cr0]
    mov cr0,eax
    jmp far [cs:real_jump]
real16:
    lgdt [cs:saved_gdtr]
    mov fs,[cs:saved_fs]
    mov gs,[cs:saved_gs]
    mov ss,[cs:saved_ss]
    mov sp,[cs:saved_sp]
    mov ds,[cs:saved_ds]
    mov es,[cs:saved_es]
    mov al,[cs:saved_pic]
    out 21h,al
    mov al,[cs:saved_pic+1]
    out 0a1h,al
    push word [cs:saved_flags]
    popf
    mov ax,4c00h
    int 21h
align 8
gdt:
    dq 0
    dq 00cf9a000000ffffh
    dq 00cf92000000ffffh
code16: dq 00009a000000ffffh
    dq 000092000000ffffh
gdt_end:
gdtr: dw gdt_end-gdt-1
    dd 0
protected_jump: dd 0
    dw 8
real_jump: dw real16,0
saved_flags: dw 0
saved_ss: dw 0
saved_sp: dw 0
saved_ds: dw 0
saved_es: dw 0
saved_fs: dw 0
saved_gs: dw 0
saved_gdtr: times 6 db 0
saved_pic: dw 0
saved_cr0: dd 0
align 16
payload:
incbin PAYLOAD
payload_end:
