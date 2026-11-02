#include <string.h>
#include <lebirun/fsname.h>

int fsname_to_utf16(const char *s, uint16_t *o) {
    const uint8_t *p = (const uint8_t *)s;
    uint32_t c;
    int n = 0;
    int k;

    if (!strcmp(s, ".") || !strcmp(s, "..")) return -1;
    while (*p) {
        c = *p++;
        k = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if ((c >= 0x80 && !k) || c >= 0xF8) return -1;
        if (k) c &= 0x3F >> k;
        while (k--) {
            if ((*p & 0xC0) != 0x80) return -1;
            c = c << 6 | (*p++ & 0x3F);
        }
        if (c < 0x20 || c == 0x7F || (c < 0x80 && strchr("\"*/:<>?\\|", (int)c)) ||
            (c >= 0xD800 && c < 0xE000) || c > 0x10FFFF)
            return -1;
        if (n + (c >= 0x10000) >= FSNAME_MAX) return -1;
        if (c >= 0x10000) {
            c -= 0x10000;
            o[n++] = (uint16_t)(0xD800 | c >> 10);
            c = 0xDC00 | (c & 0x3FF);
        }
        o[n++] = (uint16_t)c;
    }
    return n;
}

void fsname_to_utf8(const uint16_t *s, int n, char *o) {
    uint32_t c;
    int i;

    for (i = 0; i < n; i++) {
        c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 &&
            s[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (c < 0x80) {
            *o++ = (char)c;
        } else if (c < 0x800) {
            *o++ = (char)(0xC0 | c >> 6);
            *o++ = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            *o++ = (char)(0xE0 | c >> 12);
            *o++ = (char)(0x80 | (c >> 6 & 0x3F));
            *o++ = (char)(0x80 | (c & 0x3F));
        } else {
            *o++ = (char)(0xF0 | c >> 18);
            *o++ = (char)(0x80 | (c >> 12 & 0x3F));
            *o++ = (char)(0x80 | (c >> 6 & 0x3F));
            *o++ = (char)(0x80 | (c & 0x3F));
        }
    }
    *o = 0;
}
