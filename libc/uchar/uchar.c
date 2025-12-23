#include <uchar.h>
#include <wchar.h>
#include <errno.h>
#include <string.h>

size_t mbrtoc16(char16_t *pc16, const char *s, size_t n, mbstate_t *ps) {
    static mbstate_t internal_state;
    if (!ps) {
        ps = &internal_state;
    }
    
    if (s == NULL) {
        memset(ps, 0, sizeof(mbstate_t));
        return 0;
    }
    
    if (n == 0) {
        return (size_t)-2;
    }
    
    wchar_t wc;
    size_t result = mbrtowc(&wc, s, n, ps);
    
    if (result == (size_t)-1 || result == (size_t)-2) {
        return result;
    }
    
    if (wc <= 0xFFFF) {
        if (pc16) {
            *pc16 = (char16_t)wc;
        }
        return result;
    }
    
    if (wc <= 0x10FFFF) {
        wc -= 0x10000;
        if (pc16) {
            *pc16 = (char16_t)(0xD800 | (wc >> 10));
        }
        ps->__value.__wch = 0xDC00 | (wc & 0x3FF);
        ps->__count = -1;
        return result;
    }
    
    errno = EILSEQ;
    return (size_t)-1;
}

size_t c16rtomb(char *s, char16_t c16, mbstate_t *ps) {
    static mbstate_t internal_state;
    if (!ps) {
        ps = &internal_state;
    }
    
    if (s == NULL) {
        memset(ps, 0, sizeof(mbstate_t));
        return 1;
    }
    
    if (c16 >= 0xD800 && c16 <= 0xDBFF) {
        ps->__value.__wch = c16;
        ps->__count = -1;
        return 0;
    }
    
    wchar_t wc;
    if (c16 >= 0xDC00 && c16 <= 0xDFFF) {
        if (ps->__count != -1) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        wc = ((ps->__value.__wch - 0xD800) << 10) + (c16 - 0xDC00) + 0x10000;
        ps->__count = 0;
    } else {
        wc = c16;
    }
    
    return wcrtomb(s, wc, ps);
}

size_t mbrtoc32(char32_t *pc32, const char *s, size_t n, mbstate_t *ps) {
    static mbstate_t internal_state;
    if (!ps) {
        ps = &internal_state;
    }
    
    if (s == NULL) {
        memset(ps, 0, sizeof(mbstate_t));
        return 0;
    }
    
    if (n == 0) {
        return (size_t)-2;
    }
    
    wchar_t wc;
    size_t result = mbrtowc(&wc, s, n, ps);
    
    if (result == (size_t)-1 || result == (size_t)-2) {
        return result;
    }
    
    if (pc32) {
        *pc32 = (char32_t)wc;
    }
    
    return result;
}

size_t c32rtomb(char *s, char32_t c32, mbstate_t *ps) {
    static mbstate_t internal_state;
    if (!ps) {
        ps = &internal_state;
    }
    
    if (s == NULL) {
        memset(ps, 0, sizeof(mbstate_t));
        return 1;
    }
    
    if (c32 > 0x10FFFF || (c32 >= 0xD800 && c32 <= 0xDFFF)) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    
    return wcrtomb(s, (wchar_t)c32, ps);
}
