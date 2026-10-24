ENTRY(_start)
SECTIONS {
    . = 0x100000;
    .text : { *(.entry) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
    ASSERT(. < 0x180000, "Benchmark payload overlaps stack reserve")
}
