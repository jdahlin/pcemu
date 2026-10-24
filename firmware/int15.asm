; Original firmware wrapper: virtual-8086 monitors must observe PIC mask I/O
; when the BIOS enables/disables a mouse interrupt. Status belongs to IRET flags.
bits 16
org 20a0h
    int 0feh
    pushf
    push ax
    cmp byte [cs:032ah],1
    je enable
    cmp byte [cs:032ah],2
    je disable
    jmp done
enable:
    in al,21h
    and al,0fbh
    out 21h,al
    in al,0a1h
    and al,0efh
    out 0a1h,al
    jmp done
disable:
    in al,0a1h
    or al,10h
    out 0a1h,al
done:
    pop ax
    popf
    iret
