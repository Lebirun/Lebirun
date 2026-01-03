.section .text
.global gdt_flush

gdt_flush:
    mov 4(%esp), %eax
    
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    
    ljmp $0x08, $flush2

flush2:
    ret                   