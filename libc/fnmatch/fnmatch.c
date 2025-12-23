#include <fnmatch.h>
#include <string.h>
#include <ctype.h>

static int match_char(char p, char s, int casefold) {
    if (casefold) {
        return tolower((unsigned char)p) == tolower((unsigned char)s);
    }
    return p == s;
}

int fnmatch(const char *pattern, const char *string, int flags) {
    const char *p = pattern;
    const char *s = string;
    int casefold = flags & FNM_CASEFOLD;
    
    while (*p && *s) {
        if (*p == '*') {
            p++;
            if (!*p) return 0;
            while (*s) {
                if (fnmatch(p, s, flags) == 0) return 0;
                if ((flags & FNM_PATHNAME) && *s == '/') break;
                s++;
            }
            return FNM_NOMATCH;
        } else if (*p == '?') {
            if ((flags & FNM_PATHNAME) && *s == '/') return FNM_NOMATCH;
            if ((flags & FNM_PERIOD) && *s == '.' && 
                (s == string || ((flags & FNM_PATHNAME) && *(s-1) == '/'))) {
                return FNM_NOMATCH;
            }
            p++;
            s++;
        } else if (*p == '[') {
            int invert = 0;
            int match = 0;
            p++;
            if (*p == '!' || *p == '^') {
                invert = 1;
                p++;
            }
            while (*p && *p != ']') {
                char c1 = *p++;
                if (*p == '-' && *(p+1) != ']') {
                    p++;
                    char c2 = *p++;
                    if (casefold) {
                        char sc = tolower((unsigned char)*s);
                        if (sc >= tolower((unsigned char)c1) && sc <= tolower((unsigned char)c2))
                            match = 1;
                    } else {
                        if (*s >= c1 && *s <= c2) match = 1;
                    }
                } else {
                    if (match_char(c1, *s, casefold)) match = 1;
                }
            }
            if (*p == ']') p++;
            if (invert) match = !match;
            if (!match) return FNM_NOMATCH;
            s++;
        } else {
            if (!(flags & FNM_NOESCAPE) && *p == '\\' && *(p+1)) {
                p++;
            }
            if (!match_char(*p, *s, casefold)) return FNM_NOMATCH;
            p++;
            s++;
        }
    }
    
    while (*p == '*') p++;
    
    if (*p == '\0' && *s == '\0') return 0;
    if ((flags & FNM_LEADING_DIR) && *s == '/') return 0;
    
    return FNM_NOMATCH;
}
