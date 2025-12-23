#include <iconv.h>
#include <errno.h>

iconv_t iconv_open(const char *tocode, const char *fromcode) {
    (void)tocode;
    (void)fromcode;
    return (iconv_t)-1;
}

size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft,
             char **outbuf, size_t *outbytesleft) {
    (void)cd;
    if (!inbuf || !*inbuf) {
        return 0;
    }
    while (*inbytesleft > 0 && *outbytesleft > 0) {
        **outbuf = **inbuf;
        (*inbuf)++;
        (*outbuf)++;
        (*inbytesleft)--;
        (*outbytesleft)--;
    }
    return 0;
}

int iconv_close(iconv_t cd) {
    (void)cd;
    return 0;
}
