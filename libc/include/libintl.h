#ifndef _LIBINTL_H
#define _LIBINTL_H 1

#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

char *gettext(const char *msgid);
char *dgettext(const char *domainname, const char *msgid);
char *dcgettext(const char *domainname, const char *msgid, int category);
char *ngettext(const char *msgid1, const char *msgid2, unsigned long n);
char *dngettext(const char *domainname, const char *msgid1, const char *msgid2, unsigned long n);
char *dcngettext(const char *domainname, const char *msgid1, const char *msgid2, unsigned long n, int category);
char *bindtextdomain(const char *domainname, const char *dirname);
char *textdomain(const char *domainname);
char *bind_textdomain_codeset(const char *domainname, const char *codeset);

#ifdef __cplusplus
}
#endif

#endif
