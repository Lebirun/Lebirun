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
    movl %esi, 4(%eax)
    movl %edi, 8(%eax)
    movl %ebp, 12(%eax)
    leal 4(%esp), %ecx
    movl %ecx, 16(%eax)
    movl (%esp), %ecx
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
    movl 4(%edx), %esi
    movl 8(%edx), %edi
    movl 12(%edx), %ebp
    movl 16(%edx), %esp
    movl 20(%edx), %ecx
    jmp *%ecx

sigsetjmp:
    movl 4(%esp), %eax
    movl 8(%esp), %ecx
    movl %ecx, 24(%eax)
    
    movl %ebx, 0(%eax)
    movl %esi, 4(%eax)
    movl %edi, 8(%eax)
    movl %ebp, 12(%eax)
    leal 4(%esp), %ecx
    movl %ecx, 16(%eax)
    movl (%esp), %ecx
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
    movl 0(%edx), %ebx
    movl 4(%edx), %esi
    movl 8(%edx), %edi
    movl 12(%edx), %ebp
    movl 16(%edx), %esp
    movl 20(%edx), %ecx
    jmp *%ecx
