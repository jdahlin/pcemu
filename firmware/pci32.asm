; Original BIOS32 service directory entry and PCI BIOS protected-mode services.
; Code is position-independent within the F0000 firmware segment.
bits 32
org 410h
bios32:
    cmp eax, 49435024h ; '$PCI'
    jne .missing
    mov ebx, 0f0000h
    mov ecx, 10000h
    mov edx, 440h
    xor eax,eax
    retf
.missing:
    mov al,80h
    retf
 times 440h-($-$$+410h) db 90h
pci:
    pushad
    cmp ah,0b1h
    jne unsupported
    cmp al,1
    je install
    cmp al,2
    je find_device
    cmp al,3
    je find_class
    cmp al,8
    jb unsupported
    cmp al,13
    ja unsupported
    mov esi,eax
    and esi,0ffh
    cmp di,255
    ja bad_register
    movzx eax,bx
    shl eax,8
    or eax,80000000h
    movzx ebx,di
    and ebx,0fch
    or eax,ebx
    mov dx,0cf8h
    out dx,eax
    movzx ebx,di
    and ebx,3
    mov dx,0cfch
    add dx,bx
    cmp esi,8
    je read_byte
    cmp esi,9
    je read_word
    cmp esi,10
    je read_dword
    mov eax,ecx
    cmp esi,11
    je write_byte
    cmp esi,12
    je write_word
    test di,3
    jnz bad_register
    out dx,eax
    jmp success
write_byte:
    out dx,al
    jmp success
write_word:
    test di,1
    jnz bad_register
    out dx,ax
    jmp success
read_byte:
    in al,dx
    mov [esp+24],al
    jmp success
read_word:
    test di,1
    jnz bad_register
    in ax,dx
    mov [esp+24],ax
    jmp success
read_dword:
    test di,3
    jnz bad_register
    in eax,dx
    mov [esp+24],eax
    jmp success
install:
    mov word [esp+28],1
    mov word [esp+16],210h
    mov byte [esp+24],0
    mov dword [esp+20],20494350h
    mov dword [esp],0f0440h
    jmp success
find_device:
    cmp cx,1111h
    jne not_found
    cmp dx,1234h
    jne not_found
    jmp find_index
find_class:
    cmp ecx,030000h
    jne not_found
find_index:
    test si,si
    jnz not_found
    mov word [esp+16],10h
success:
    mov byte [esp+29],0
    popad
    clc
    retf
unsupported:
    mov byte [esp+29],81h
    jmp failure
not_found:
    mov byte [esp+29],86h
    jmp failure
bad_register:
    mov byte [esp+29],87h
failure:
    popad
    stc
    retf
