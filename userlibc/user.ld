OUTPUT_FORMAT(binary)
ENTRY(_start)

SECTIONS
{
    . = 0x00400000;

    .text : {
        *(.text.entry) 
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
}
