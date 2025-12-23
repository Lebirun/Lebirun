#include <error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

unsigned int error_message_count = 0;
int error_one_per_line = 0;
void (*error_print_progname)(void) = NULL;

static const char *program_name = "program";

void error(int status, int errnum, const char *format, ...) {
    va_list args;
    
    if (error_print_progname) {
        error_print_progname();
    } else {
        fprintf(stderr, "%s: ", program_name);
    }
    
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    
    if (errnum != 0) {
        fprintf(stderr, ": %s", strerror(errnum));
    }
    
    fprintf(stderr, "\n");
    
    error_message_count++;
    
    if (status != 0) {
        exit(status);
    }
}

void error_at_line(int status, int errnum, const char *filename, unsigned int linenum, const char *format, ...) {
    static const char *last_filename = NULL;
    static unsigned int last_linenum = 0;
    va_list args;
    
    if (error_one_per_line) {
        if (last_filename && filename && strcmp(last_filename, filename) == 0 && last_linenum == linenum) {
            return;
        }
        last_filename = filename;
        last_linenum = linenum;
    }
    
    if (error_print_progname) {
        error_print_progname();
    } else {
        fprintf(stderr, "%s:", program_name);
    }
    
    if (filename) {
        fprintf(stderr, "%s:%u: ", filename, linenum);
    }
    
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    
    if (errnum != 0) {
        fprintf(stderr, ": %s", strerror(errnum));
    }
    
    fprintf(stderr, "\n");
    
    error_message_count++;
    
    if (status != 0) {
        exit(status);
    }
}
