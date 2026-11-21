ENTRY(harness_entry)
SECTIONS
{
    . = ALIGN(GFX_InitCfg, 64);
    .text : { *(.text.entry) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { harness_bss_start = .; *(.bss*) *(COMMON) harness_bss_end = .; }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
