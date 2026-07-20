.set MB2_MAGIC,    0xE85250D6
.set MB2_ARCH,     0
.set MB2_LENGTH,   (mb2_header_end - mb2_header)
.set MB2_CHECKSUM, -(MB2_MAGIC + MB2_ARCH + MB2_LENGTH)

.set KERNEL_VMA_LOW, 0x80000000

.section .multiboot.data, "a"
.align 8
mb2_header:
    .long MB2_MAGIC
    .long MB2_ARCH
    .long MB2_LENGTH
    .long MB2_CHECKSUM

    .align 8
mb2_fb_tag:
    .short 5
    .short 0
    .long 20
    .long 1024
    .long 768
    .long 32

    .align 8
    .short 0
    .short 0
    .long 8
mb2_header_end:

.section .bootstrap_stack, "aw", @nobits
stack_bottom:
.skip 8192
stack_top:

.section .multiboot.bss, "aw", @nobits
.align 4096
.global boot_pml4
boot_pml4:
.skip 4096
.global boot_pdpt_low
boot_pdpt_low:
.skip 4096
.global boot_pdpt_high
.global boot_pdpt
boot_pdpt_high:
boot_pdpt:
.skip 4096
.global boot_pd_0
.global boot_pd_high
.global boot_pd_low
boot_pd_0:
boot_pd_high:
boot_pd_low:
.skip 4096
.global boot_pd_1
boot_pd_1:
.skip 4096

.global multiboot_magic
.global multiboot_ptr

.section .bss
.align 8
multiboot_magic: .skip 4
multiboot_ptr: .skip 8

.section .multiboot.text, "ax"
.code32
.global _start
_start:
    cli

    movl %eax, %esi
    movl %ebx, %edi

    movl $(boot_pml4), %eax
    movl $1024, %ecx
    xorl %edx, %edx
clear_pml4:
    movl %edx, (%eax)
    addl $4, %eax
    loop clear_pml4

    movl $(boot_pdpt_low), %eax
    movl $1024, %ecx
    xorl %edx, %edx
clear_pdpt_low:
    movl %edx, (%eax)
    addl $4, %eax
    loop clear_pdpt_low

    movl $(boot_pdpt_high), %eax
    movl $1024, %ecx
    xorl %edx, %edx
clear_pdpt_high:
    movl %edx, (%eax)
    addl $4, %eax
    loop clear_pdpt_high

    movl $(boot_pd_0), %eax
    movl $512, %ecx
    movl $0x00000083, %edx
fill_pd_0:
    movl %edx, (%eax)
    movl $0, 4(%eax)
    addl $0x00200000, %edx
    addl $8, %eax
    loop fill_pd_0

    movl $(boot_pd_1), %eax
    movl $512, %ecx
    movl $0x40000083, %edx
fill_pd_1:
    movl %edx, (%eax)
    movl $0, 4(%eax)
    addl $0x00200000, %edx
    addl $8, %eax
    loop fill_pd_1

    movl $(boot_pdpt_low), %eax
    movl $(boot_pd_0), %edx
    orl $0x003, %edx
    movl %edx, 0*8(%eax)
    movl $0,   0*8+4(%eax)

    movl $(boot_pd_1), %edx
    orl $0x003, %edx
    movl %edx, 1*8(%eax)
    movl $0,   1*8+4(%eax)

    movl $(boot_pdpt_high), %eax
    movl $(boot_pd_0), %edx
    orl $0x003, %edx
    movl %edx, 510*8(%eax)
    movl $0,   510*8+4(%eax)

    movl $(boot_pd_1), %edx
    orl $0x003, %edx
    movl %edx, 511*8(%eax)
    movl $0,   511*8+4(%eax)

    movl $(boot_pml4), %eax
    movl $(boot_pdpt_low), %edx
    orl $0x003, %edx
    movl %edx, 0*8(%eax)
    movl $0,   0*8+4(%eax)

    movl $(boot_pdpt_high), %edx
    orl $0x003, %edx
    movl %edx, 511*8(%eax)
    movl $0,   511*8+4(%eax)

    movl $(boot_pml4), %eax
    movl %eax, %cr3

    movl %cr4, %eax
    orl $(1 << 5), %eax
    orl $(1 << 9), %eax
    orl $(1 << 10), %eax
    movl %eax, %cr4

    movl $0xC0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    orl $(1 << 11), %eax
    wrmsr

    movl %cr0, %eax
    orl $0x80010001, %eax
    movl %eax, %cr0

    lgdt (early_gdt_ptr)
    ljmp $0x08, $(long_mode_entry)

.code64
long_mode_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    movabsq $higher_half, %rax
    jmp *%rax

.section .text
.code64
higher_half:
    movabsq $stack_top, %rsp

    movl %esi, multiboot_magic(%rip)
    movl %edi, multiboot_ptr(%rip)

    xorl %ebp, %ebp
    call kernel_main

    cli
hang:
    hlt
    jmp hang

.section .multiboot.data
.align 16
early_gdt:
    .quad 0x0000000000000000
    .quad 0x00AF9A000000FFFF
    .quad 0x00CF92000000FFFF
early_gdt_end:

.align 4
early_gdt_ptr:
    .word early_gdt_end - early_gdt - 1
early_gdt_ptr_val:
    .long early_gdt
