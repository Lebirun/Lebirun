#ifndef _LEBIRUN_LKE_H
#define _LEBIRUN_LKE_H

#include <stdint.h>

#define LKE_MAGIC       0x4C4B4558
#define LKE_DIR         "/lib/lke"

#define LKE_VERSION     1

typedef struct lke_module {
    char *name;
    void (*cleanup)(void);
    void *text_base;
    uint64_t text_pages;
} lke_module_t;

typedef struct lke_ksym {
    const char *name;
    uint64_t addr;
} lke_ksym_t;

#ifdef __is_kernel
void lke_init(void);
int lke_load(const char *path);
int lke_unload(const char *name);
int lke_list(char *buf, int size);
void lke_autoload(void);
void lke_register_symbol(const char *name, void *addr);
int lke_register_syscall(int num, void *fn);
void lke_unregister_syscall(int num, void *fn);
#else
int lke_register_syscall(int num, void *fn);
void lke_unregister_syscall(int num, void *fn);

#define module_init(fn) int lke_module_init(void) { return fn(); }
#define module_exit(fn) void lke_module_cleanup(void) { fn(); }

#define LKE_NAME(n)       static const char __lke_meta_name[]    __attribute__((used, section(".lke_info"))) = "name=" n
#define LKE_DESC(d)       static const char __lke_meta_desc[]    __attribute__((used, section(".lke_info"))) = "description=" d
#define LKE_LICENSE(l)    static const char __lke_meta_license[] __attribute__((used, section(".lke_info"))) = "license=" l
#define LKE_AUTHOR(a)     static const char __lke_meta_author[]  __attribute__((used, section(".lke_info"))) = "author=" a
#define LKE_VERSION_STR(v) static const char __lke_meta_version[] __attribute__((used, section(".lke_info"))) = "version=" v
#endif

#endif
