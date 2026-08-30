OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    phdr PT_PHDR PHDRS FLAGS(4);
    text PT_LOAD FILEHDR PHDRS FLAGS(5);
    data PT_LOAD FLAGS(6);
    tls PT_TLS FLAGS(4);
}

SECTIONS
{
    . = 0x00400000 + SIZEOF_HEADERS;

    .init : {
        KEEP(*(.init))
    } :text

    .text : {
        KEEP(*(.text._start))
        KEEP(*(.text._start_c))
        *(.text.__init_libc)
        *(.text.libc_start_init)
        *(.text.libc_start_main_stage2)
        *(.text.__libc_start_main)
        *(.text.__init_tp)
        *(.text.__copy_tls)
        *(.text.static_init_tls)
        *(.text.__set_thread_area)
        *(.rodata.__init_libc.str1.1)
        *(.text.hot.user)
        *(.text.read)
        *(.text.write)
        *(.text.getuid)
        *(.text.strcmp)
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
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } :data

    .ctors : {
        __CTOR_LIST__ = .;
        KEEP(*crtbegin.o(.ctors))
        KEEP(*crtbegin?.o(.ctors))
        KEEP(*(EXCLUDE_FILE(*crtend.o *crtend?.o) .ctors))
        KEEP(*(SORT(.ctors.*)))
        KEEP(*(.ctors))
        PROVIDE(__CTOR_END__ = .);
    } :data

    .dtors : {
        __DTOR_LIST__ = .;
        KEEP(*crtbegin.o(.dtors))
        KEEP(*crtbegin?.o(.dtors))
        KEEP(*(EXCLUDE_FILE(*crtend.o *crtend?.o) .dtors))
        KEEP(*(SORT(.dtors.*)))
        KEEP(*(.dtors))
        PROVIDE(__DTOR_END__ = .);
    } :data

    .fini_array : {
        __fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        __fini_array_end = .;
    } :data

    .tdata : {
        *(.tdata .tdata.*)
    } :data :tls

    .tbss : {
        *(.tbss .tbss.*)
    } :data :tls

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        __bss_start = .;
        *(.bss.__sysinfo)
        *(.bss.__progname_full)
        *(.bss.__progname)
        *(.bss.__hwcap)
        *(.bss.__libc)
        *(.bss.__environ)
        *(.bss.main_tls)
        *(.bss.builtin_tls)
        *(.bss.__thread_list_lock)
        *(.bss.hot.user)
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } :data

    _end = .;
}
