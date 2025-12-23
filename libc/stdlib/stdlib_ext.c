#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

void _Exit(int status) {
    _exit(status);
}

static void (*atexit_handlers[32])(void);
static int atexit_count = 0;

typedef struct {
    void (*func)(int, void *);
    void *arg;
} onexit_entry;

static onexit_entry onexit_handlers[32];
static int onexit_count = 0;
static int exit_status_saved = 0;

int atexit(void (*function)(void)) {
    if (atexit_count >= 32) return -1;
    atexit_handlers[atexit_count++] = function;
    return 0;
}

int on_exit(void (*function)(int, void *), void *arg) {
    if (onexit_count >= 32) return -1;
    onexit_handlers[onexit_count].func = function;
    onexit_handlers[onexit_count].arg = arg;
    onexit_count++;
    return 0;
}

void __run_atexit(void) {
    while (onexit_count > 0) {
        onexit_count--;
        onexit_handlers[onexit_count].func(exit_status_saved, onexit_handlers[onexit_count].arg);
    }
    while (atexit_count > 0) {
        atexit_handlers[--atexit_count]();
    }
}

void __set_exit_status(int status) {
    exit_status_saved = status;
}

long long atoll(const char *s) {
    return strtoll(s, NULL, 10);
}

double atof(const char *s) {
    return strtod(s, NULL);
}

long long strtoll(const char *s, char **endptr, int base) {
    return (long long)strtol(s, endptr, base);
}

unsigned long long strtoull(const char *s, char **endptr, int base) {
    return (unsigned long long)strtoul(s, endptr, base);
}

double strtod(const char *s, char **endptr) {
    double result = 0.0;
    int sign = 1;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        result = result * 10.0 + (*s - '0');
        s++;
    }
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') {
            result += (*s - '0') * frac;
            frac *= 0.1;
            s++;
        }
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        int esign = 1;
        int exp = 0;
        if (*s == '-') { esign = -1; s++; }
        else if (*s == '+') s++;
        while (*s >= '0' && *s <= '9') {
            exp = exp * 10 + (*s - '0');
            s++;
        }
        double mul = 1.0;
        for (int i = 0; i < exp; i++) mul *= 10.0;
        if (esign < 0) result /= mul;
        else result *= mul;
    }
    if (endptr) *endptr = (char *)s;
    return result * sign;
}

float strtof(const char *s, char **endptr) {
    return (float)strtod(s, endptr);
}

static unsigned int rand_seed = 1;

int rand(void) {
    rand_seed = rand_seed * 1103515245 + 12345;
    return (int)((rand_seed >> 16) & RAND_MAX);
}

void srand(unsigned int seed) {
    rand_seed = seed;
}

int rand_r(unsigned int *seedp) {
    *seedp = *seedp * 1103515245 + 12345;
    return (int)((*seedp >> 16) & RAND_MAX);
}

static void swap_bytes(void *a, void *b, size_t size) {
    char *pa = a;
    char *pb = b;
    while (size--) {
        char t = *pa;
        *pa++ = *pb;
        *pb++ = t;
    }
}

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    if (nmemb < 2) return;
    char *arr = base;
    for (size_t i = 0; i < nmemb - 1; i++) {
        for (size_t j = 0; j < nmemb - i - 1; j++) {
            if (compar(arr + j * size, arr + (j + 1) * size) > 0) {
                swap_bytes(arr + j * size, arr + (j + 1) * size, size);
            }
        }
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    const char *arr = base;
    size_t low = 0, high = nmemb;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        int cmp = compar(key, arr + mid * size);
        if (cmp == 0) return (void *)(arr + mid * size);
        if (cmp < 0) high = mid;
        else low = mid + 1;
    }
    return NULL;
}

int system(const char *command) {
    (void)command;
    return -1;
}

int mkstemp(char *template) {
    return mkstemps(template, 0);
}

int mkstemps(char *template, int suffixlen) {
    size_t len = 0;
    char *p = template;
    while (*p) { len++; p++; }
    
    if (len < 6 + (size_t)suffixlen) return -1;
    
    char *x = template + len - suffixlen - 6;
    for (int i = 0; i < 6; i++) {
        if (x[i] != 'X') return -1;
    }
    
    static unsigned int counter = 0;
    counter++;
    unsigned int val = counter ^ (unsigned int)(long)template;
    
    const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int i = 0; i < 6; i++) {
        x[i] = chars[val % 62];
        val /= 62;
    }
    
    int fd = open(template, 0102, 0600); 
    return fd;
}

char *mkdtemp(char *template) {
    (void)template;
    return NULL;
}

char *realpath(const char *path, char *resolved_path) {
    (void)path;
    (void)resolved_path;
    return NULL;
}

void *aligned_alloc(size_t alignment, size_t size) {
    (void)alignment;
    return malloc(size);
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
    (void)alignment;
    *memptr = malloc(size);
    return *memptr ? 0 : -1;
}
