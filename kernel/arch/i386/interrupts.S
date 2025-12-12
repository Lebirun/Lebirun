.section .text

.extern interrupt_handler

.macro isr_noerr num
    .global isr\num
    isr\num:
        cli
        pushl $0         
        pushl $\num
        jmp isr_common_stub
.endm

.macro isr_err num
    .global isr\num
    isr\num:
        cli
        pushl $\num          
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

isr_common_stub:
    pusha

    push %ds
    push %es
    push %fs
    push %gs

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    push %esp               
    call interrupt_handler
    add $4, %esp            

    mov %eax, %esp

    pop %gs
    pop %fs
    pop %es
    pop %ds

    popa
    add $8, %esp
    iret                    