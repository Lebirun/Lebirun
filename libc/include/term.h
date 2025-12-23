#ifndef _TERM_H
#define _TERM_H 1

#include <termios.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct term {
    char *term_names;
    int Columns;
    int Lines;
} TERMINAL;

extern TERMINAL *cur_term;

int setupterm(const char *term, int filedes, int *errret);
int set_curterm(TERMINAL *termp);
int del_curterm(TERMINAL *termp);
int restartterm(const char *term, int filedes, int *errret);

char *tparm(const char *str, ...);
int tputs(const char *str, int affcnt, int (*putc)(int));
int putp(const char *str);

int tigetflag(const char *capname);
int tigetnum(const char *capname);
char *tigetstr(const char *capname);

char *tgoto(const char *cap, int col, int row);

#ifdef __cplusplus
}
#endif

#endif
