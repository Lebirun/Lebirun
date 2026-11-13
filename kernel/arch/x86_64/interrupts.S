.section .text

.extern interrupt_handler
.extern interrupt_prepare_page_fault
.extern kernel_irq_cr3
.extern smp_percpu_irq_ready

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
isr_noerr 50

isr_noerr 51
isr_noerr 52
isr_noerr 53
isr_noerr 54
isr_noerr 55
isr_noerr 56
isr_noerr 57
isr_noerr 58
isr_noerr 59
isr_noerr 60
isr_noerr 61
isr_noerr 62
isr_noerr 63
isr_noerr 64
isr_noerr 65
isr_noerr 66
isr_noerr 67
isr_noerr 68
isr_noerr 69
isr_noerr 70
isr_noerr 71
isr_noerr 72
isr_noerr 73
isr_noerr 74
isr_noerr 75
isr_noerr 76
isr_noerr 77
isr_noerr 78
isr_noerr 79
isr_noerr 80
isr_noerr 81
isr_noerr 82
isr_noerr 83
isr_noerr 84
isr_noerr 85
isr_noerr 86
isr_noerr 87
isr_noerr 88
isr_noerr 89
isr_noerr 90
isr_noerr 91
isr_noerr 92
isr_noerr 93
isr_noerr 94
isr_noerr 95
isr_noerr 96
isr_noerr 97
isr_noerr 98
isr_noerr 99
isr_noerr 100
isr_noerr 101
isr_noerr 102
isr_noerr 103
isr_noerr 104
isr_noerr 105
isr_noerr 106
isr_noerr 107
isr_noerr 108
isr_noerr 109
isr_noerr 110
isr_noerr 111
isr_noerr 112
isr_noerr 113
isr_noerr 114
isr_noerr 115
isr_noerr 116
isr_noerr 117
isr_noerr 118
isr_noerr 119
isr_noerr 120
isr_noerr 121
isr_noerr 122
isr_noerr 123
isr_noerr 124
isr_noerr 125
isr_noerr 126
isr_noerr 127

.section .rodata
.global isr_msi_table
isr_msi_table:
    .quad isr51
    .quad isr52
    .quad isr53
    .quad isr54
    .quad isr55
    .quad isr56
    .quad isr57
    .quad isr58
    .quad isr59
    .quad isr60
    .quad isr61
    .quad isr62
    .quad isr63
    .quad isr64
    .quad isr65
    .quad isr66
    .quad isr67
    .quad isr68
    .quad isr69
    .quad isr70
    .quad isr71
    .quad isr72
    .quad isr73
    .quad isr74
    .quad isr75
    .quad isr76
    .quad isr77
    .quad isr78
    .quad isr79
    .quad isr80
    .quad isr81
    .quad isr82
    .quad isr83
    .quad isr84
    .quad isr85
    .quad isr86
    .quad isr87
    .quad isr88
    .quad isr89
    .quad isr90
    .quad isr91
    .quad isr92
    .quad isr93
    .quad isr94
    .quad isr95
    .quad isr96
    .quad isr97
    .quad isr98
    .quad isr99
    .quad isr100
    .quad isr101
    .quad isr102
    .quad isr103
    .quad isr104
    .quad isr105
    .quad isr106
    .quad isr107
    .quad isr108
    .quad isr109
    .quad isr110
    .quad isr111
    .quad isr112
    .quad isr113
    .quad isr114
    .quad isr115
    .quad isr116
    .quad isr117
    .quad isr118
    .quad isr119
    .quad isr120
    .quad isr121
    .quad isr122
    .quad isr123
    .quad isr124
    .quad isr125
    .quad isr126
    .quad isr127
.section .text

.global isr128
isr128:
    cli
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
    cmpl $0, smp_percpu_irq_ready(%rip)
    je 2f
    movq %gs:72, %rcx
2:
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

    cld
    cmpl $0, smap_enabled(%rip)
    je 6f
    clac
6:
    movq %rsp, %r12
    xorq %r13, %r13
    cmpq $14, 160(%r12)
    jne 3f
    movq %cr2, %r13
    movq %r12, %rdi
    movq %r13, %rsi
    andq $-16, %rsp
    call interrupt_prepare_page_fault
    movq %rax, %r12
3:
    movq %r12, %rsp
    andq $-16, %rsp
    movq %r12, %rdi
    movq %r13, %rsi
    call interrupt_handler
    xorq %rdx, %rdx
    cmpq %r12, %rax
    setne %dl
    movq %rax, %rsp
    movq 0(%rsp), %rax
    testq %rax, %rax
    jz 1f
    testq %rdx, %rdx
    jnz 5f
    cmpq $14, 160(%rsp)
    je 5f
    movq %cr3, %rcx
    cmpq %rcx, %rax
    je 1f
5:
    movq %rax, %cr3
1:
    testq %rdx, %rdx
    jz 4f
    cmpl $0, smp_percpu_irq_ready(%rip)
    je 4f
    movq $0, %gs:160
4:
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
