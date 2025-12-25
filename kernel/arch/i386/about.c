#include <kernel/about.h>
#include <string.h>

static const char *build_date = __DATE__ " " __TIME__;

int sys_uname(struct utsname *buf) {
    if (!buf) return -1;

    strcpy(buf->sysname, SYSNAME);
    strcpy(buf->nodename, NODENAME);
    strcpy(buf->release, RELEASE);
    strcpy(buf->version, VERSION);
    strcpy(buf->machine, MACHINE);

    return 0;
}