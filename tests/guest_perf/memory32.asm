; Original dependent-load sweep; not an lmbench port or published lmbench score.
; 32-bit supervisor identity paging. Data and page tables live only in RAM.
bits 32
org 100000h
    push ebx
    push esi
    push edi
    push ebp
    cld
    ; Identity map 64 MiB with ordinary 4 KiB PDE/PTEs.
    mov edi,200000h
    mov ecx,1024
    xor eax,eax
    rep stosd
    mov edi,200000h
    mov eax,201007h
    mov ecx,16
.pde:
    stosd
    add eax,1000h
    loop .pde
    mov edi,201000h
    mov eax,7
    mov ecx,16384
.pte:
    stosd
    add eax,1000h
    loop .pte
    mov dword [paged],0
.mode:
    mov dword [stride],4
.stride:
    mov dword [size_index],0
.size:
    mov edi,order
    mov ecx,[size_index]
    mov ecx,[sizes+ecx*4]
    mov [nodes],ecx
    xor eax,eax
.fill:
    stosd
    inc eax
    loop .fill
    mov ebx,[nodes]
    dec ebx
    mov esi,12345678h
.shuffle:
    mov eax,esi
    shl eax,13
    xor esi,eax
    mov eax,esi
    shr eax,17
    xor esi,eax
    mov eax,esi
    shl eax,5
    xor esi,eax
    mov eax,esi
    xor edx,edx
    lea ecx,[ebx+1]
    div ecx
    mov eax,[order+ebx*4]
    mov ecx,[order+edx*4]
    mov [order+edx*4],eax
    mov [order+ebx*4],ecx
    dec ebx
    jnz .shuffle
    xor ebx,ebx
.link:
    mov eax,[order+ebx*4]
    imul eax,[stride]
    add eax,400000h
    lea ecx,[ebx+1]
    mov edx,[nodes]
    dec edx
    and ecx,edx
    mov edx,[order+ecx*4]
    imul edx,[stride]
    add edx,400000h
    mov [eax],edx
    inc ebx
    cmp ebx,[nodes]
    jb .link
    mov eax,200000h
    mov cr3,eax
    mov eax,cr0
    and eax,7fffffffh
    cmp dword [paged],0
    je .flat
    or eax,80000000h
.flat:
    mov cr0,eax
    ; Warm the exact load loop before measurement, then restore its initial node.
    mov ecx,256
    call chase_start
    mov ecx,LOOPS
    mov al,1
    out 0e7h,al
    call chase_start
    mov ebp,eax
    mov al,2
    out 0e7h,al
    ; One line per interval: mode, stride, node count, final pointer (hex).
    mov eax,[paged]
    call hex
    mov eax,[stride]
    call hex
    mov eax,[nodes]
    call hex
    mov eax,ebp
    call hex
    mov al,10
    out 0e9h,al
    inc dword [size_index]
    cmp dword [size_index],5
    jb .size
    cmp dword [stride],4
    jne .next_mode
    mov dword [stride],4096
    jmp .stride
.next_mode:
    inc dword [paged]
    cmp dword [paged],2
    jb .mode
    mov eax,cr0
    and eax,7fffffffh
    mov cr0,eax
    pop ebp
    pop edi
    pop esi
    pop ebx
    mov al,3
    out 0e7h,al
    ret
chase_start:
    mov eax,[order]
    imul eax,[stride]
    add eax,400000h
chase:
%rep 32
    mov eax,[eax]
%endrep
    dec ecx
    jnz chase
%rep 7
    mov eax,[eax]
%endrep
    ret
hex:
    push ecx
    push edx
    mov edx,eax
    mov ecx,8
.hex:
    rol edx,4
    mov eax,edx
    and eax,15
    add al,'0'
    cmp al,'9'
    jbe .emit
    add al,7
.emit:
    out 0e9h,al
    loop .hex
    mov al,' '
    out 0e9h,al
    pop edx
    pop ecx
    ret
align 4
sizes: dd 16,256,1024,2048,4096
paged: dd 0
stride: dd 0
size_index: dd 0
nodes: dd 0
order: times 4096 dd 0
