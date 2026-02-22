.code16
.section .text

.global ap_tramp_start
.global ap_tramp_end
.global ap_boot_cr3
.global ap_boot_stack
.global ap_boot_flag
.global ap_pae_flag
.global ap_boot_entry

ap_tramp_start:
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    lgdt (ap_tramp_gdt_ptr - ap_tramp_start + 0x8000)

    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0

    ljmp $0x08, $(ap_tramp_pm - ap_tramp_start + 0x8000)

.code32
ap_tramp_pm:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    movl (ap_pae_flag - ap_tramp_start + 0x8000), %eax
    testl %eax, %eax
    jz ap_tramp_no_pae

    movl (ap_boot_cr3 - ap_tramp_start + 0x8000), %eax
    movl %eax, %cr3

    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4

    movl %cr0, %eax
    orl $0x80010000, %eax
    movl %eax, %cr0
    jmp ap_tramp_paged

ap_tramp_no_pae:
    movl (ap_boot_cr3 - ap_tramp_start + 0x8000), %eax
    movl %eax, %cr3

    movl %cr0, %eax
    orl $0x80010000, %eax
    movl %eax, %cr0

ap_tramp_paged:
    movl (ap_boot_stack - ap_tramp_start + 0x8000), %esp

    movl $1, (ap_boot_flag - ap_tramp_start + 0x8000)

    movl (ap_boot_entry - ap_tramp_start + 0x8000), %eax
    jmp *%eax

1:
    hlt
    jmp 1b

.align 4
ap_boot_cr3:    .long 0
ap_boot_stack:  .long 0
ap_boot_flag:   .long 0
ap_pae_flag:    .long 0
ap_boot_entry:  .long 0

.align 8
ap_tramp_gdt:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF
    .quad 0x00CF92000000FFFF
    .quad 0x00CFFA000000FFFF
    .quad 0x00CFF2000000FFFF

ap_tramp_gdt_ptr:
    .word (ap_tramp_gdt_ptr - ap_tramp_gdt - 1)
    .long (ap_tramp_gdt - ap_tramp_start + 0x8000)

ap_tramp_end:
    nop
