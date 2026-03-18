OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x00400000;

    .init : {
        KEEP(*(.init))
    } :text

    .text : {
        KEEP(*(.text._start))
        KEEP(*(.text._start_c))
        KEEP(*(.text.main))
        *(.text.entry)
        *(.text .text.*)
    } :text

    .fini : {
        KEEP(*(.fini))
    } :text

    .rodata : {
        *(.rodata .rodata.*)
    } :text

    .eh_frame : {
        *(.eh_frame)
    } :text

    . = ALIGN(4096);

    .preinit_array : {
        __preinit_array_start = .;
        *(.preinit_array)
        __preinit_array_end = .;
    } :data

    .init_array : {
        __init_array_start = .;
        *(.init_array)
        __init_array_end = .;
    } :data

    .fini_array : {
        __fini_array_start = .;
        *(.fini_array)
        __fini_array_end = .;
    } :data

    .tdata : {
        *(.tdata .tdata.*)
    } :data

    .tbss : {
        *(.tbss .tbss.*)
    } :data

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } :data

    _end = .;
}
