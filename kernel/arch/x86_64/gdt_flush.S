.section .text
.global gdt_flush

gdt_flush:
    movw %di, %dx

    movw %dx, %ds
    movw %dx, %es
    movw %dx, %fs
    movw %dx, %gs
    movw %dx, %ss

    popq %rdi
    pushq $0x08
    pushq %rdi
    lretq