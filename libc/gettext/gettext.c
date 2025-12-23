#include <libintl.h>

char *gettext(const char *msgid) { return (char *) msgid; }
char *dgettext(const char *domainname, const char *msgid) { (void)domainname; return (char *) msgid; }
char *dcgettext(const char *domainname, const char *msgid, int category) { (void)domainname; (void)category; return (char *) msgid; }
char *ngettext(const char *msgid1, const char *msgid2, unsigned long n) { return (char *) (n == 1 ? msgid1 : msgid2); }
char *dngettext(const char *domainname, const char *msgid1, const char *msgid2, unsigned long n) { (void)domainname; return n == 1 ? (char *)msgid1 : (char *)msgid2; }
char *dcngettext(const char *domainname, const char *msgid1, const char *msgid2, unsigned long n, int category) { (void)domainname; (void)category; return n == 1 ? (char *)msgid1 : (char *)msgid2; }
char *bindtextdomain(const char *domainname, const char *dirname) { (void)domainname; return (char *) dirname; }
char *textdomain(const char *domainname) { return (char *) domainname; }
char *bind_textdomain_codeset(const char *domainname, const char *codeset) { (void)domainname; return (char *) codeset; }
