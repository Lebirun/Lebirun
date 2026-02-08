ENTRY(_start)

SECTIONS
{
    . = 0x00100000;
    _kernel_start = .;

    .multiboot.data : {
        KEEP(*(.multiboot.data))
    }

    .multiboot.text : {
        KEEP(*(.multiboot.text))
    }

    . += 0xC0000000;

    .text ALIGN(4K) : AT(ADDR(.text) - 0xC0000000) {
        *(.text .text.*)
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - 0xC0000000) {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - 0xC0000000) {
        *(.data .data.*)
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - 0xC0000000) {
        *(COMMON)
        *(.bss .bss.*)
        *(.bootstrap_stack)
        _memory_map_start = .;
    }

    _kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note)
        *(.note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}