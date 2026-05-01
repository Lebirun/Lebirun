.section .text

.extern interrupt_handler
.extern kernel_irq_cr3

.macro isr_noerr num
    .global isr\num
    isr\num:
        cli
        pushq $0
        pushq $\num
        jmp isr_common_stub
.endm

.macro isr_err num
    .global isr\num
    isr\num:
        cli
        pushq $\num
        jmp isr_common_stub
.endm

isr_noerr 0
isr_noerr 1
isr_noerr 2
isr_noerr 3
isr_noerr 4
isr_noerr 5
isr_noerr 6
isr_noerr 7
isr_err   8
isr_noerr 9
isr_err   10
isr_err   11
isr_err   12
isr_err   13
isr_err   14
isr_noerr 15
isr_noerr 16
isr_err   17
isr_noerr 18
isr_noerr 19
isr_noerr 20
isr_noerr 21
isr_noerr 22
isr_noerr 23
isr_noerr 24
isr_noerr 25
isr_noerr 26
isr_noerr 27
isr_noerr 28
isr_noerr 29
isr_noerr 30
isr_noerr 31

isr_noerr 32
isr_noerr 33
isr_noerr 34
isr_noerr 35
isr_noerr 36
isr_noerr 37
isr_noerr 38
isr_noerr 39
isr_noerr 40
isr_noerr 41
isr_noerr 42
isr_noerr 43
isr_noerr 44
isr_noerr 45
isr_noerr 46
isr_noerr 47
isr_noerr 48

isr_noerr 49

.global isr128
isr128:
    pushq $0
    pushq $128
    jmp isr_common_stub

isr_noerr 255

isr_common_stub:
    pushq %rax
    pushq %rcx

    movq %cr3, %rax
    pushq %rax
    movq kernel_irq_cr3(%rip), %rcx
    testq %rcx, %rcx
    jz 1f
    cmpq %rcx, %rax
    je 1f
    movq %rcx, %cr3
1:

    pushq %rdx
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    xorq %rax, %rax
    movw %ds, %ax
    pushq %rax
    xorq %rax, %rax
    movw %es, %ax
    pushq %rax
    movq 120(%rsp), %rax
    pushq %rax
    pushq %rax

    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es

    movq %rsp, %rdi
    call interrupt_handler
    movq %rax, %rsp
    movq 0(%rsp), %rax
    testq %rax, %rax
    jz 1f
    movq %rax, %cr3
1:
    addq $16, %rsp

    popq %rax
    movw %ax, %es
    popq %rax
    movw %ax, %ds

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    popq %rdx
    addq $8, %rsp
    popq %rcx
    popq %rax

    addq $16, %rsp
    iretq
