#include <string.h>
#include <errno.h>

static const char *error_strings[] = {
    "Success",
    "Operation not permitted",
    "No such file or directory",
    "No such process",
    "Interrupted system call",
    "I/O error",
    "No such device or address",
    "Argument list too long",
    "Exec format error",
    "Bad file number",
    "No child processes",
    "Try again",
    "Out of memory",
    "Permission denied",
    "Bad address",
    "Block device required",
    "Device or resource busy",
    "File exists",
    "Cross-device link",
    "No such device",
    "Not a directory",
    "Is a directory",
    "Invalid argument",
    "File table overflow",
    "Too many open files",
    "Not a typewriter",
    "Text file busy",
    "File too large",
    "No space left on device",
    "Illegal seek",
    "Read-only file system",
    "Too many links",
    "Broken pipe",
    "Math argument out of domain of func",
    "Math result not representable",
    "Resource deadlock would occur",
    "File name too long",
    "No record locks available",
    "Function not implemented",
    "Directory not empty",
    "Too many symbolic links encountered",
    "Unknown error",
    "No message of desired type",
    "Identifier removed",
};

#define NUM_ERRORS (sizeof(error_strings) / sizeof(error_strings[0]))

static char unknown_buf[32];

char *strerror(int errnum) {
    if (errnum >= 0 && (size_t)errnum < NUM_ERRORS) {
        return (char *)error_strings[errnum];
    }
    int len = 0;
    const char *prefix = "Unknown error ";
    while (*prefix) unknown_buf[len++] = *prefix++;
    if (errnum < 0) {
        unknown_buf[len++] = '-';
        errnum = -errnum;
    }
    char tmp[12];
    int i = 0;
    do {
        tmp[i++] = '0' + (errnum % 10);
        errnum /= 10;
    } while (errnum);
    while (i > 0) unknown_buf[len++] = tmp[--i];
    unknown_buf[len] = '\0';
    return unknown_buf;
}

int strerror_r(int errnum, char *buf, size_t buflen) {
    char *msg = strerror(errnum);
    size_t len = strlen(msg);
    if (len >= buflen) {
        if (buflen > 0) {
            memcpy(buf, msg, buflen - 1);
            buf[buflen - 1] = '\0';
        }
        return ERANGE;
    }
    memcpy(buf, msg, len + 1);
    return 0;
}
