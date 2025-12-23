#include <regex.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

int regcomp(regex_t *preg, const char *regex, int cflags) {
    (void)regex;
    (void)cflags;
    if (!preg) return REG_BADPAT;
    preg->re_nsub = 0;
    preg->__re_private = NULL;
    return 0;
}

int regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t pmatch[], int eflags) {
    (void)preg;
    (void)string;
    (void)eflags;
    for (size_t i = 0; i < nmatch; i++) {
        pmatch[i].rm_so = -1;
        pmatch[i].rm_eo = -1;
    }
    return REG_NOMATCH;
}

void regfree(regex_t *preg) {
    if (preg) {
        free(preg->__re_private);
        preg->__re_private = NULL;
        preg->re_nsub = 0;
    }
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size) {
    (void)preg;
    const char *msg;
    switch (errcode) {
        case 0: msg = "Success"; break;
        case REG_NOMATCH: msg = "No match"; break;
        case REG_BADPAT: msg = "Invalid pattern"; break;
        case REG_ECOLLATE: msg = "Invalid collating element"; break;
        case REG_ECTYPE: msg = "Invalid character class"; break;
        case REG_EESCAPE: msg = "Trailing backslash"; break;
        case REG_ESUBREG: msg = "Invalid back reference"; break;
        case REG_EBRACK: msg = "Unmatched brackets"; break;
        case REG_EPAREN: msg = "Unmatched parentheses"; break;
        case REG_EBRACE: msg = "Unmatched braces"; break;
        case REG_BADBR: msg = "Invalid range"; break;
        case REG_ERANGE: msg = "Invalid range end"; break;
        case REG_ESPACE: msg = "Out of memory"; break;
        case REG_BADRPT: msg = "Invalid repetition"; break;
        default: msg = "Unknown error"; break;
    }
    size_t len = strlen(msg) + 1;
    if (errbuf && errbuf_size > 0) {
        size_t copy = (len < errbuf_size) ? len : errbuf_size;
        memcpy(errbuf, msg, copy - 1);
        errbuf[copy - 1] = '\0';
    }
    return len;
}
