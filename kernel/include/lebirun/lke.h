#ifndef _LEBIRUN_LKE_H
#define _LEBIRUN_LKE_H

#include <stdint.h>

#define LKE_MAGIC       0x4C4B4558
#define LKE_MAX_MODULES 65536
#define LKE_NAME_MAX    128
#define LKE_DIR         "/lib/lke"

#define LKE_VERSION     1

typedef struct lke_module {
    char name[LKE_NAME_MAX];
    uint32_t magic;
    uint32_t version;
    int (*init)(void);
    void (*cleanup)(void);
    void *text_base;
    uint64_t text_size;
    int loaded;
} lke_module_t;

typedef struct lke_info {
    char name[LKE_NAME_MAX];
    int loaded;
} lke_info_t;

typedef struct lke_ksym {
    const char *name;
    uint64_t addr;
} lke_ksym_t;

#ifdef __is_kernel
void lke_init(void);
int lke_load(const char *path);
int lke_unload(const char *name);
int lke_list(lke_info_t *buf, int max);
void lke_autoload(void);
void lke_register_symbol(const char *name, void *addr);
#else
#define module_init(fn) int lke_module_init(void) { return fn(); }
#define module_exit(fn) void lke_module_cleanup(void) { fn(); }
#endif

#endif
