#ifndef _STDIO_H
#define _STDIO_H 1

#include <sys/cdefs.h>
#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

#define EOF (-1)

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define BUFSIZ 1024

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define FOPEN_MAX 64
#define FILENAME_MAX 256
#define L_tmpnam 20
#define TMP_MAX 238328

#define P_tmpdir "/tmp"

#ifndef P_tmpdir
#define P_tmpdir "/tmp"
#endif

typedef struct _FILE FILE;
typedef long fpos_t;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#ifdef __cplusplus
extern "C" {
#endif

int printf(const char* __restrict, ...);
int sprintf(char* __restrict, const char* __restrict, ...);
int snprintf(char* __restrict, size_t, const char* __restrict, ...);
int vsnprintf(char* __restrict, size_t, const char* __restrict, va_list);
int vprintf(const char* __restrict, va_list);
int vsprintf(char* __restrict, const char* __restrict, va_list);
int fprintf(FILE*, const char* __restrict, ...);
int vfprintf(FILE*, const char* __restrict, va_list);
int sscanf(const char* __restrict, const char* __restrict, ...);
int vsscanf(const char* __restrict, const char* __restrict, va_list);
int fscanf(FILE*, const char* __restrict, ...);
int scanf(const char* __restrict, ...);
int asprintf(char **strp, const char *format, ...);
int vasprintf(char **strp, const char *format, va_list ap);
int dprintf(int fd, const char *format, ...);
int vdprintf(int fd, const char *format, va_list ap);

int putchar(int);
int puts(const char*);
int getchar(void);

FILE *fopen(const char *pathname, const char *mode);
FILE *fdopen(int fd, const char *mode);
FILE *freopen(const char *pathname, const char *mode, FILE *stream);
int fclose(FILE *stream);

int fflush(FILE *stream);
int setvbuf(FILE *stream, char *buf, int mode, size_t size);
void setbuf(FILE *stream, char *buf);
void setbuffer(FILE *stream, char *buf, size_t size);
void setlinebuf(FILE *stream);

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);

int fgetc(FILE *stream);
int getc(FILE *stream);
int ungetc(int c, FILE *stream);
char *fgets(char *s, int size, FILE *stream);
ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream);
ssize_t getline(char **lineptr, size_t *n, FILE *stream);

int fputc(int, FILE*);
int putc(int c, FILE *stream);
int fputs(const char*, FILE*);

int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
void rewind(FILE *stream);
int fgetpos(FILE *stream, fpos_t *pos);
int fsetpos(FILE *stream, const fpos_t *pos);

int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int fileno(FILE *stream);

void perror(const char *s);

int getc_unlocked(FILE *stream);
int putc_unlocked(int c, FILE *stream);
int fflush_unlocked(FILE *stream);
size_t fread_unlocked(void *ptr, size_t size, size_t n, FILE *stream);
size_t fwrite_unlocked(const void *ptr, size_t size, size_t n, FILE *stream);
int getchar_unlocked(void);
int putchar_unlocked(int c);
int fgetc_unlocked(FILE *stream);
int fputc_unlocked(int c, FILE *stream);

void flockfile(FILE *stream);
void funlockfile(FILE *stream);
int ftrylockfile(FILE *stream);

int remove(const char *pathname);
int rename(const char *oldpath, const char *newpath);

FILE *tmpfile(void);
char *tmpnam(char *s);

FILE *popen(const char *command, const char *type);
int pclose(FILE *stream);

#ifdef __cplusplus
}
#endif

#endif
