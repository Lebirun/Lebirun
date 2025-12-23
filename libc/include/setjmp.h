#ifndef _SETJMP_H
#define _SETJMP_H 1

#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned long __ebx;
    unsigned long __esp;
    unsigned long __ebp;
    unsigned long __esi;
    unsigned long __edi;
    unsigned long __eip;
} jmp_buf[1];

typedef struct {
    jmp_buf       __jmpbuf;
    int           __mask_was_saved;
    unsigned long __saved_mask;
} sigjmp_buf[1];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((__noreturn__));

int _setjmp(jmp_buf env);
void _longjmp(jmp_buf env, int val) __attribute__((__noreturn__));

int sigsetjmp(sigjmp_buf env, int savesigs);
void siglongjmp(sigjmp_buf env, int val) __attribute__((__noreturn__));

#ifdef __cplusplus
}
#endif

#endif
