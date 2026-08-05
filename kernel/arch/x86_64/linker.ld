OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

PHDRS
{
    boot_text PT_LOAD FLAGS(5);
    boot_bss PT_LOAD FLAGS(6);
    text PT_LOAD FLAGS(5);
    init_data PT_LOAD FLAGS(6);
    runtime_text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x00100000;
    _kernel_phys_start = .;

    .multiboot.data : {
        _kernel_multiboot_start = .;
        KEEP(*(.multiboot.data))
        KEEP(*(.multiboot.text))
        . = ALIGN(4K);
        _kernel_multiboot_end = .;
    } :boot_text

    .multiboot.bss ALIGN(4K) : {
        KEEP(*(.multiboot.bss))
    } :boot_bss

    . += KERNEL_VMA;

    _kernel_start = .;

    .init.early.text ALIGN(4K) : AT(ADDR(.init.early.text) - KERNEL_VMA) {
        _kernel_text_start = .;
        _kernel_early_init_start = .;
        KEEP(*(.init.early.text .init.early.text.*))
        . = ALIGN(4K);
        _kernel_early_init_end = .;
    } :text

    .init.text ALIGN(4K) : AT(ADDR(.init.text) - KERNEL_VMA) {
        _kernel_init_start = .;
        KEEP(*(.init.text .init.text.*))
        KEEP(*(.init.rodata .init.rodata.*))
        KEEP(kernel/launch_user.o(.text .text.* .rodata .rodata.*))
        KEEP(*(.rodata.kernel_boot .rodata.kernel_boot.*))
        . = ALIGN(4K);
    } :text

    .init.bss (NOLOAD) : AT(ADDR(.init.bss) - KERNEL_VMA) {
        KEEP(*(.init.bss .init.bss.*))
        . = ALIGN(4K);
        _kernel_init_end = .;
    } :init_data

    .init.optional.bss ALIGN(4K) (NOLOAD) :
        AT(ADDR(.init.optional.bss) - KERNEL_VMA) {
        _kernel_init_optional_start = .;
        KEEP(*(.init.optional.bss .init.optional.bss.*))
        . = ALIGN(4K);
        _kernel_init_optional_end = .;
    } :init_data

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) {
        *(.text .text.*)
        _kernel_text_end = .;
    } :runtime_text

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
        KEEP(*(.lke_info))
        KEEP(*(.rodata.font))
        *(.rodata .rodata.*)
        _kernel_rodata_end = .;
    } :rodata

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data .data.*)
    } :data

    .bss : AT(ADDR(.bss) - KERNEL_VMA) {
        *(COMMON)
        *(.bss)
        *(SORT_BY_ALIGNMENT(.bss.*))
    } :data

    .bootstrap_stack ALIGN(4K) : AT(ADDR(.bootstrap_stack) - KERNEL_VMA) {
        _kernel_boot_stack_start = .;
        KEEP(*(.bootstrap_stack))
        . = ALIGN(4K);
        _kernel_boot_stack_end = .;
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
        *(.init)
        *(.fini)
    }
}
