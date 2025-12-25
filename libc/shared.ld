OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)

SECTIONS
{
    . = 0;

    .text : ALIGN(4096) {
        *(.text .text.*)
    }

    .rodata : ALIGN(4096) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(4096) {
        *(.data .data.*)
    }

    .dynamic : {
        *(.dynamic)
    }

    .hash : {
        *(.hash)
    }

    .dynsym : {
        *(.dynsym)
    }

    .dynstr : {
        *(.dynstr)
    }

    .rel.dyn : {
        *(.rel.dyn)
        *(.rel*)
    }

    .symtab : {
        *(.symtab)
    }

    .strtab : {
        *(.strtab)
    }

    .shstrtab : {
        *(.shstrtab)
    }

    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    _end = .;
}
