#include <term.h>
#include <stdarg.h>
#include <stddef.h>
#include <unistd.h>

TERMINAL *cur_term = NULL;

static TERMINAL default_term = {
    .term_names = "dumb",
    .Columns = 80,
    .Lines = 24
};

int setupterm(const char *term, int filedes, int *errret) {
    (void)term;
    (void)filedes;
    cur_term = &default_term;
    if (errret) *errret = 1;
    return 0;
}

int set_curterm(TERMINAL *termp) {
    TERMINAL *old = cur_term;
    cur_term = termp;
    return old ? 0 : -1;
}

int del_curterm(TERMINAL *termp) {
    (void)termp;
    return 0;
}

int restartterm(const char *term, int filedes, int *errret) {
    return setupterm(term, filedes, errret);
}

char *tparm(const char *str, ...) {
    (void)str;
    return (char *)str;
}

static int (*tputs_putc)(int);

int tputs(const char *str, int affcnt, int (*putc_fn)(int)) {
    (void)affcnt;
    tputs_putc = putc_fn;
    if (!str) return -1;
    while (*str) {
        putc_fn(*str++);
    }
    return 0;
}

int putp(const char *str) {
    if (!str) return -1;
    write(1, str, 0);
    const char *p = str;
    while (*p) p++;
    write(1, str, p - str);
    return 0;
}

int tigetflag(const char *capname) {
    (void)capname;
    return 0;
}

int tigetnum(const char *capname) {
    (void)capname;
    if (capname[0] == 'c' && capname[1] == 'o' && capname[2] == 'l' && capname[3] == 's') {
        return 80;
    }
    if (capname[0] == 'l' && capname[1] == 'i' && capname[2] == 'n' && capname[3] == 'e') {
        return 24;
    }
    return -1;
}

char *tigetstr(const char *capname) {
    (void)capname;
    return NULL;
}

char *tgoto(const char *cap, int col, int row) {
    (void)cap;
    (void)col;
    (void)row;
    return NULL;
}
