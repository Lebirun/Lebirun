OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

PHDRS
{
    boot_text PT_LOAD FLAGS(5);
    boot_bss PT_LOAD FLAGS(6);
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x00100000;
    _kernel_phys_start = .;

    .multiboot.data : {
        KEEP(*(.multiboot.data))
        KEEP(*(.multiboot.text))
    } :boot_text

    .multiboot.bss ALIGN(4K) : {
        *(.multiboot.bss)
    } :boot_bss

    . += KERNEL_VMA;

    _kernel_start = .;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) {
        _kernel_text_start = .;
        *(.text .text.*)
        _kernel_text_end = .;
    } :text

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata .rodata.*)
        _kernel_rodata_end = .;
    } :rodata

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data .data.*)
    } :data

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
        *(COMMON)
        *(.bss .bss.*)
        *(.bootstrap_stack)
        _memory_map_start = .;
    } :data

    _kernel_end = .;
    _kernel_phys_end = _kernel_end - KERNEL_VMA;

    /DISCARD/ : {
        *(.comment)
        *(.note)
        *(.note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}
