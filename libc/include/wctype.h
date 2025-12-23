#ifndef _WCTYPE_H
#define _WCTYPE_H 1

#include <sys/cdefs.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t wint_t;
typedef unsigned long wctype_t;
typedef unsigned long wctrans_t;

#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

int iswalnum(wint_t wc);
int iswalpha(wint_t wc);
int iswblank(wint_t wc);
int iswcntrl(wint_t wc);
int iswdigit(wint_t wc);
int iswgraph(wint_t wc);
int iswlower(wint_t wc);
int iswprint(wint_t wc);
int iswpunct(wint_t wc);
int iswspace(wint_t wc);
int iswupper(wint_t wc);
int iswxdigit(wint_t wc);

int iswctype(wint_t wc, wctype_t desc);
wctype_t wctype(const char *property);

wint_t towlower(wint_t wc);
wint_t towupper(wint_t wc);
wint_t towctrans(wint_t wc, wctrans_t desc);
wctrans_t wctrans(const char *charclass);

#ifdef __cplusplus
}
#endif

#endif
