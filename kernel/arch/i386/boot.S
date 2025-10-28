.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set FLAGS,    ALIGN | MEMINFO
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot.data, "aw"
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

.section .bootstrap_stack, "aw", @nobits
stack_bottom:
    .skip 16384
stack_top:

.section .bss, "aw", @nobits
.align 4096
.global boot_page_directory
boot_page_directory:
    .skip 4096
.global boot_page_table1
boot_page_table1:
    .skip 4096

.section .multiboot.text, "a"
.global _start
.type _start, @function
_start:
    movl $(boot_page_table1 - 0xC0000000), %edi

    movl $0x00000000, %esi
    movl $1024, %ecx

1:
    movl %esi, %edx
    orl $0x3, %edx
    movl %edx, (%edi)

    addl $4096, %esi
    addl $4, %edi
    loop 1b

    movl $((0x000B8000) | 0x3), %eax
    movl %eax, boot_page_table1 - 0xC0000000 + 1023*4

    movl $(boot_page_table1 - 0xC0000000), %eax
    orl $0x3, %eax

    movl %eax, boot_page_directory - 0xC0000000
    movl %eax, boot_page_directory - 0xC0000000 + 768*4

    movl $(boot_page_directory - 0xC0000000), %eax
    movl %eax, %cr3

    /* Make recursive mapping: point PDE[1023] to the page directory itself so
     * that 0xFFFFF000 maps to the page directory and 0xFFC00000 maps to page tables.
     */
    movl $(boot_page_directory - 0xC0000000), %eax
    orl $0x3, %eax
    movl %eax, boot_page_directory - 0xC0000000 + 1023*4

    movl %cr0, %eax
    orl $0x80000000, %eax
    movl %eax, %cr0

    jmp 4f

.section .text
4:
    movl %cr3, %eax
    movl %eax, %cr3

    movl $stack_top, %esp

    call kernel_main

    cli
hang:
    hlt
    jmp hang