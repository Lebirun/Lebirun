#ifndef _ERROR_H
#define _ERROR_H 1

#include <sys/cdefs.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned int error_message_count;
extern int error_one_per_line;
extern void (*error_print_progname)(void);

void error(int status, int errnum, const char *format, ...);
void error_at_line(int status, int errnum, const char *filename, unsigned int linenum, const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif
