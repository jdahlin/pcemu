; Original DOS guest acceptance probe. Hardware effects go through DOS/BIOS,
; the installed XMS/EMS/mouse APIs, or the primary ATA register interface.
; No guest success is inferred from a driver banner alone.
bits 16
org 100h
start:
    cld
    push ds
    pop es
    mov byte [stage],1
    mov di,source
    mov cx,8192
    mov al,19
.fill: stosb
    add al,73
    loop .fill
    ; DOS file create/write/close/reopen/read/compare/rename/delete.
    mov dx,file1
    xor cx,cx
    mov ah,3ch
    int 21h
    jc fail
    mov bx,ax
    mov dx,source
    mov cx,8192
    mov ah,40h
    int 21h
    jc fail
    cmp ax,8192
    jne fail
    mov ah,3eh
    int 21h
    jc fail
    mov ax,3d00h
    mov dx,file1
    int 21h
    jc fail
    mov bx,ax
    mov dx,target
    mov cx,8192
    mov ah,3fh
    int 21h
    jc fail
    cmp ax,8192
    jne fail
    mov ah,3eh
    int 21h
    jc fail
    call compare_buffers
    mov dx,file1
    mov di,file2
    mov ah,56h
    int 21h
    jc fail
    mov dx,file2
    mov ah,41h
    int 21h
    jc fail
    mov dx,disk_ok
    call print
    ; Exercise the ATA device separately: DOS's block driver uses BIOS INT13.
    mov byte [stage],2
    mov dx,3f6h
    mov al,2
    out dx,al
    mov dx,1f6h
    mov al,0e0h
    out dx,al
    mov dx,1f7h
    mov al,0ech
    out dx,al
    call ata_ready
    mov dx,1f0h
    mov di,target
    mov cx,256
    rep insw
    test word [target+98],200h
    jz fail
    mov dx,1f2h
    mov al,1
    out dx,al
    inc dx
    xor al,al
    out dx,al
    inc dx
    out dx,al
    inc dx
    out dx,al
    mov dx,1f7h
    mov al,20h
    out dx,al
    call ata_ready
    mov dx,1f0h
    mov di,target
    mov cx,256
    rep insw
    mov dx,3f6h
    xor al,al
    out dx,al
    mov [dap+6],ds
    mov si,dap
    mov dl,80h
    mov ah,42h
    int 13h
    jc fail
    mov si,source
    mov di,target
    mov cx,256
    repe cmpsw
    jne fail
    mov dx,ata_ok
    call print
    mov byte [stage],3
    mov ax,0500h
    mov cx,1e61h
    int 16h
    test al,al
    jnz fail
    mov ah,1
    int 16h
    jz fail
    cmp ax,1e61h
    jne fail
    xor ah,ah
    int 16h
    cmp ax,1e61h
    jne fail
    mov dx,key_ok
    call print
    mov byte [stage],4
    xor ah,ah
    int 1ah
    mov bx,dx
    mov ecx,10000000
.tick:
    push ecx
    xor ah,ah
    int 1ah
    pop ecx
    cmp dx,bx
    jne .ticked
    a32 loop .tick
    jmp fail
.ticked:
    mov dx,timer_ok
    call print
%ifdef EXPECT_XMS
    mov byte [stage],5
    mov ax,4300h
    int 2fh
    cmp al,80h
    jne fail
    mov ax,4310h
    int 2fh
    mov [xms_entry],bx
    mov [xms_entry+2],es
    push ds
    pop es
    mov ah,9
    mov dx,64
    call far [xms_entry]
    cmp ax,1
    jne fail
    mov [xms_handle],dx
    mov [move_dst_handle],dx
    mov word [move_src_offset],source
    mov [move_src_offset+2],ds
    mov si,move_block
    mov ah,0bh
    call far [xms_entry]
    cmp ax,1
    jne fail
    mov ax,[xms_handle]
    mov [move_src_handle],ax
    mov dword [move_src_offset],0
    mov word [move_dst_handle],0
    mov word [move_dst_offset],target
    mov [move_dst_offset+2],ds
    mov si,move_block
    mov ah,0bh
    call far [xms_entry]
    cmp ax,1
    jne fail
    call compare_buffers
    mov dx,[xms_handle]
    mov ah,0ah
    call far [xms_entry]
    cmp ax,1
    jne fail
    mov dx,xms_ok
    call print
%endif
%ifdef EXPECT_EMS
    mov byte [stage],6
    mov ah,40h
    int 67h
    test ah,ah
    jnz fail
    mov ah,41h
    int 67h
    test ah,ah
    jnz fail
    mov [ems_frame],bx
    mov ah,43h
    mov bx,4
    int 67h
    test ah,ah
    jnz fail
    mov [ems_handle],dx
    xor bp,bp
.ems_write:
    call map_page
    xor di,di
    mov ax,bp
    add ax,1234h
    mov cx,8192
    rep stosw
    inc bp
    cmp bp,4
    jb .ems_write
    xor bp,bp
.ems_read:
    call map_page
    xor di,di
    mov ax,bp
    add ax,1234h
    mov cx,8192
    repe scasw
    jne fail
    inc bp
    cmp bp,4
    jb .ems_read
    push ds
    pop es
    mov dx,[ems_handle]
    mov ah,45h
    int 67h
    test ah,ah
    jnz fail
    mov dx,ems_ok
    call print
%endif
%ifdef EXPECT_MOUSE
    mov byte [stage],7
    xor ax,ax
    int 33h
    cmp ax,0ffffh
    jne fail
    mov ax,4
    mov cx,160
    mov dx,80
    int 33h
    mov ax,3
    int 33h
    cmp cx,160
    jne fail
    cmp dx,80
    jne fail
    mov ax,0bh
    int 33h
    ; Fence host motion behind a visible prompt and a blocking DOS read. The
    ; harness injects PS/2 motion then a key; require nonzero driver mickeys.
    mov bx,2
    mov cx,mouse_wait_end-mouse_wait
    mov dx,mouse_wait
    mov ah,40h
    int 21h
    mov ah,8
    int 21h
    mov ax,0bh
    int 33h
    or cx,dx
    jz fail
    mov dx,mouse_ok
    call print
%endif
    mov dx,all_ok
    call print
    mov ax,4c00h
    int 21h
ata_ready:
    mov dx,1f7h
    mov ecx,1000000
.poll:
    in al,dx
    test al,80h
    jnz .again
    test al,21h
    jnz fail
    test al,8
    jnz .done
.again:
    a32 loop .poll
    jmp fail
.done: ret
compare_buffers:
    push ds
    pop es
    mov si,source
    mov di,target
    mov cx,4096
    repe cmpsw
    jne fail
    ret
%ifdef EXPECT_EMS
map_page:
    mov dx,[ems_handle]
    mov bx,bp
    mov ax,4400h
    int 67h
    test ah,ah
    jnz fail
    mov es,[ems_frame]
    ret
%endif
print:
    mov ah,9
    int 21h
    ret
fail:
    push ds
    pop es
    mov al,[stage]
    add al,'0'
    mov [fail_number],al
    mov dx,failed
    call print
    mov ax,4c01h
    int 21h
stage db 0
file1 db 'PROBE.DAT',0
file2 db 'RENAMED.DAT',0
disk_ok db 'PASS DOS-FILE-8192',13,10,'$'
ata_ok db 'PASS ATA-PIO-IDENTIFY-READ',13,10,'$'
key_ok db 'PASS BIOS-KEYBOARD-RING',13,10,'$'
timer_ok db 'PASS PIT-IRQ-TICKS',13,10,'$'
xms_ok db 'PASS XMS-MOVE-8192',13,10,'$'
ems_ok db 'PASS EMS-4-PAGES-65536',13,10,'$'
mouse_ok db 'PASS PS2-MOUSE-IRQ-MOTION',13,10,'$'
mouse_wait db 'PCEMU MOUSE WAIT',13,10
mouse_wait_end:
all_ok db 'PASS PROBE-COMPLETE',13,10,'$'
failed db 'FAIL STAGE '
fail_number db '?',13,10,'$'
xms_entry dd 0
xms_handle dw 0
ems_handle dw 0
ems_frame dw 0
align 4
move_block dd 8192
move_src_handle dw 0
move_src_offset dd 0
move_dst_handle dw 0
move_dst_offset dd 0
align 4
dap db 16,0
    dw 1
    dw source
    dw 0
    dq 0
source times 8192 db 0
target times 8192 db 0
