#ifndef _LEBIRUN_VRING_H
#define _LEBIRUN_VRING_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define VRING_MAX_SUBRINGS 8
#define VRING_MAX_REGIONS 8

#define VRING_PERM_READ   0x01
#define VRING_PERM_WRITE  0x02
#define VRING_PERM_EXEC   0x04

typedef struct {
    uint64_t start;
    uint64_t end;
    uint8_t permissions;
} vring_mem_region_t;

typedef struct {
    uint8_t ring_major;
    uint8_t ring_minor;
    bool active;
    vring_mem_region_t allowed_regions[VRING_MAX_REGIONS];
    uint64_t region_count;
    const char *name;
} vring_t;

void vring_init(void);
int vring_create(uint8_t minor, const char *name);
int vring_add_region(uint8_t minor, uint64_t start, uint64_t end, uint8_t perms);
int vring_remove(uint8_t minor);
vring_t *vring_get(uint8_t minor);
bool vring_check_access(uint8_t minor, uint64_t addr, uint64_t size, uint8_t access_type);
void vring_panic_forbidden(uint8_t minor, uint64_t addr, uint8_t access_type);

extern vring_t *subrings;

#define KPROC_MAX 16
#define KPROC_PID_BASE (-1)

typedef enum {
    KPROC_STATE_NONE = 0,
    KPROC_STATE_RUNNING,
    KPROC_STATE_BLOCKED,
    KPROC_STATE_DEAD
} kproc_state_t;

struct kproc;

typedef void (*kproc_entry_t)(struct kproc *self);

typedef struct kproc {
    int32_t pid;
    kproc_state_t state;
    uint8_t vring_minor;
    const char *name;
    kproc_entry_t entry;
    void *private_data;
    uint64_t msg_queue[16];
    uint64_t msg_head;
    uint64_t msg_tail;
    uint64_t msg_count;
} kproc_t;

void kproc_init(void);
int32_t kproc_create(const char *name, uint8_t vring_minor, kproc_entry_t entry, void *priv);
kproc_t *kproc_get(int32_t pid);
kproc_t *kproc_current(void);
void kproc_set_current(int32_t pid);
int kproc_send_msg(int32_t pid, uint64_t msg);
int kproc_recv_msg(int32_t pid, uint64_t *msg);
void kproc_yield(void);

#define KPROC_MSG_PRINT_CHAR   0x0001
#define KPROC_MSG_PRINT_STRING 0x0002
#define KPROC_MSG_DEBUG_LOG    0x0003

typedef struct {
    uint64_t type;
    union {
        char ch;
        struct {
            const char *str;
            size_t len;
        } string;
        struct {
            const char *msg;
            int level;
        } debug;
    } data;
} print_msg_t;

void kproc_print_init(void);
void kproc_print_char(char c);
void kproc_print_string(const char *str, size_t len);
void kproc_debug_log(const char *msg, int level);

bool kproc_is_negative_pid(int32_t pid);
kproc_t *kproc_find_by_pid(int32_t pid);

int klog_printf(int level, const char *fmt, ...);
int klog_enqueue_raw(const char *buf, size_t len);
int klog_drain_console0(uint64_t max_items);
int klog_snapshot(char *buf, int bufsz);
int klog_snapshot_range(char *buf, int offset, int count);

int kprint_write(int console_id, const char *buf, size_t len);
void kprint_serial_async(const char *buf, size_t len);

bool kprint_is_ready(void);
void kprint_enable(void);
void kprint_flush(void);

extern kproc_t *current_kproc;

#ifndef KPRINT_DRAIN_IN_IRQ
#define KPRINT_DRAIN_IN_IRQ 1
#endif

void kprint_poll(uint64_t max_items);

#endif
