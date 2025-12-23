.global setjmp
.global _setjmp
.global longjmp
.global _longjmp
.global sigsetjmp
.global siglongjmp

.type setjmp, @function
.type _setjmp, @function
.type longjmp, @function
.type _longjmp, @function
.type sigsetjmp, @function
.type siglongjmp, @function

setjmp:
_setjmp:
    movl 4(%esp), %eax
    movl %ebx, 0(%eax)
    leal 4(%esp), %ecx
    movl %ecx, 4(%eax)
    movl %ebp, 8(%eax)
    movl %esi, 12(%eax)
    movl %edi, 16(%eax)
    movl 0(%esp), %ecx
    movl %ecx, 20(%eax)
    xorl %eax, %eax
    ret

longjmp:
_longjmp:
    movl 4(%esp), %edx
    movl 8(%esp), %eax
    testl %eax, %eax
    jnz 1f
    incl %eax
1:
    movl 0(%edx), %ebx
    movl 4(%edx), %esp
    movl 8(%edx), %ebp
    movl 12(%edx), %esi
    movl 16(%edx), %edi
    jmp *20(%edx)

sigsetjmp:
    movl 4(%esp), %eax
    movl 8(%esp), %ecx
    movl %ecx, 24(%eax)
    testl %ecx, %ecx
    jz 1f
    pushl $0
    pushl $0
    pushl $0
    call sigprocmask
    addl $12, %esp
    movl 4(%esp), %eax
    movl %eax, 28(%eax)
1:
    movl %ebx, 0(%eax)
    leal 4(%esp), %ecx
    movl %ecx, 4(%eax)
    movl %ebp, 8(%eax)
    movl %esi, 12(%eax)
    movl %edi, 16(%eax)
    movl 0(%esp), %ecx
    movl %ecx, 20(%eax)
    xorl %eax, %eax
    ret

siglongjmp:
    movl 4(%esp), %edx
    movl 8(%esp), %eax
    testl %eax, %eax
    jnz 1f
    incl %eax
1:
    movl 24(%edx), %ecx
    testl %ecx, %ecx
    jz 2f
    pushl %eax
    pushl %edx
    leal 28(%edx), %ecx
    pushl $0
    pushl %ecx
    pushl $2
    call sigprocmask
    addl $12, %esp
    popl %edx
    popl %eax
2:
    movl 0(%edx), %ebx
    movl 4(%edx), %esp
    movl 8(%edx), %ebp
    movl 12(%edx), %esi
    movl 16(%edx), %edi
    jmp *20(%edx)
