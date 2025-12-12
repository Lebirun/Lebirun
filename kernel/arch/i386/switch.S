.section .text

.global switch_to_asm

switch_to_asm:
    movl 4(%esp), %eax
    movl 8(%esp), %edx

    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    pushl %ds
    pushl %es
    pushl %fs
    pushl %gs

    movl %esp, (%eax)

    movl %edx, %esp

    popl %gs
    popl %fs
    popl %es
    popl %ds
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp

    ret
