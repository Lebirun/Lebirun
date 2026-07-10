#include <lebirun/vring.h>
#include <lebirun/console.h>
#include <lebirun/tty.h>
#include <lebirun/mem_map.h>
#include <lebirun/common.h>
#include <lebirun/task.h>
#include <lebirun/io.h>
#include <lebirun/panic.h>
#include <lebirun/spinlock.h>
#include <lebirun/kstack.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

vring_t *subrings;
static int subrings_count;
kproc_t *kernel_procs;
static int kprocs_count;
kproc_t *current_kproc = NULL;
static int32_t next_kproc_pid = KPROC_PID_BASE;
static volatile int vring_initialized = 0;
static spinlock_t vring_lock = { .locked = 0 };
static volatile int kproc_initialized = 0;

static volatile uint64_t print_queue_head = 0;
static volatile uint64_t print_queue_tail = 0;
static volatile uint64_t print_queue_count = 0;

#define KLOG_MAX_ITEMS 8
#define KLOG_MAX_LEN   128

typedef struct {
    uint16_t len;
    uint8_t level;
    char msg[KLOG_MAX_LEN];
} klog_item_t;

static klog_item_t *klog_ring;
static uint64_t klog_capacity;
static volatile uint64_t klog_head = 0;
static volatile uint64_t klog_tail = 0;
static volatile uint64_t klog_count = 0;
static volatile uint64_t klog_dropped = 0;

#define KLOG_PERSIST_SZ 32768
static char *klog_persist_buf;
static volatile int klog_persist_pos = 0;
static int klog_persist_cap = 0;

#define KLOG_EARLY_SZ 512
static char klog_early_buf[KLOG_EARLY_SZ];
static volatile int klog_early_pos = 0;
static volatile int klog_early_done = 0;

#define KPRINT_MAX_ITEMS 32
#define KPRINT_MAX_LEN   128

typedef struct {
    uint16_t len;
    uint8_t con_id;
    uint8_t reserved;
    char msg[KPRINT_MAX_LEN];
} kprint_item_t;

static kprint_item_t *kprint_ring;
static uint64_t kprint_capacity;
static volatile uint64_t kprint_head = 0;
static volatile uint64_t kprint_tail = 0;
static volatile uint64_t kprint_count = 0;

static volatile int kprint_ready = 0;

static wait_queue_t kprint_waitq;

#define VRING_PTE_NX 0x8000000000000000ULL

extern char _kernel_text_start[];
extern char _kernel_text_end[];
extern char _kernel_rodata_end[];
extern char _kernel_end[];

static volatile int vring_selftest_stage = 0;
static volatile int vring_selftest_failed = 0;
static volatile int vring_selftest_violation_seen = 0;
static volatile int vring_selftest_started = 0;
static uint64_t vring_selftest_pml4;
static uint8_t *vring_selftest_buf;
static task_t *vring_selftest_task_ref;

#define SERIAL_RING_SIZE 4096
static char *serial_ring;
static uint64_t serial_ring_capacity = 0;
static volatile uint64_t serial_head = 0;
static volatile uint64_t serial_tail = 0;
static volatile uint64_t serial_count = 0;
static spinlock_t serial_lock = { .locked = 0 };
static spinlock_t klog_persist_lock = { .locked = 0 };

static size_t klog_strnlen(const char *s, size_t maxlen) {
    size_t n = 0;
    if (!s) return 0;
    while (n < maxlen && s[n]) n++;
    return n;
}

static inline uint64_t klog_irqsave(void) {
    uint64_t flags;
    asm volatile("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void klog_irqrestore(uint64_t flags) {
    asm volatile("push %0; popf" : : "r"(flags) : "memory", "cc");
}

static inline bool interrupts_enabled(void) {
    uint64_t flags;
    asm volatile("pushf; pop %0" : "=r"(flags) : : "memory");
    return (flags & (1u << 9)) != 0;
}

static inline bool serial_thr_empty(void) {
    return (inb(0x3FD) & 0x20) != 0;
}

static int serial_reserve_nolock(uint64_t extra) {
    uint64_t new_capacity;
    uint64_t i;
    char *new_ring;
    char *old_ring;
    uint64_t old_capacity;

    if (extra > SERIAL_RING_SIZE) return 0;
    if (serial_count + extra <= serial_ring_capacity) return 1;
    new_capacity = serial_ring_capacity;
    if (new_capacity == 0) new_capacity = extra;
    while (new_capacity < SERIAL_RING_SIZE && serial_count + extra > new_capacity) {
        new_capacity *= 2;
    }
    if (new_capacity > SERIAL_RING_SIZE) new_capacity = SERIAL_RING_SIZE;
    if (serial_count + extra > new_capacity) return 0;
    new_ring = (char *)kmalloc(new_capacity);
    if (!new_ring) return 0;
    old_ring = serial_ring;
    old_capacity = serial_ring_capacity;
    for (i = 0; i < serial_count; i++) {
        new_ring[i] = old_ring[(serial_head + i) % old_capacity];
    }
    if (old_ring) kfree(old_ring);
    serial_ring = new_ring;
    serial_ring_capacity = new_capacity;
    serial_head = 0;
    serial_tail = serial_count % serial_ring_capacity;
    return 1;
}

static void serial_enqueue_nolock(const char *buf, size_t len) {
    size_t i;
    uint8_t ch;

    for (i = 0; i < len; i++) {
        ch = (uint8_t)buf[i];
        if (ch == '\n') {
            if (!serial_reserve_nolock(1)) {
                while (!serial_thr_empty()) cpu_relax();
                outb(0x3F8, '\r');
            } else {
                serial_ring[serial_tail] = '\r';
                serial_tail = (serial_tail + 1) % serial_ring_capacity;
                serial_count++;
            }
        }
        if (!serial_reserve_nolock(1)) {
            while (!serial_thr_empty()) cpu_relax();
            outb(0x3F8, ch);
            continue;
        }
        serial_ring[serial_tail] = ch;
        serial_tail = (serial_tail + 1) % serial_ring_capacity;
        serial_count++;
    }
}

static void serial_write_async(const char *buf, size_t len) {
    uint64_t flags;
    size_t i;
    uint8_t ch;

    if (!vring_initialized && !serial_ring) {
        for (i = 0; i < len; i++) {
            ch = (uint8_t)buf[i];
            if (ch == '\n') {
                while (!serial_thr_empty()) cpu_relax();
                outb(0x3F8, '\r');
            }
            while (!serial_thr_empty()) cpu_relax();
            outb(0x3F8, ch);
        }
        return;
    }
    flags = klog_irqsave();
    spin_lock(&serial_lock);
    serial_enqueue_nolock(buf, len);
    spin_unlock(&serial_lock);
    klog_irqrestore(flags);
}

static void serial_write(const char *buf, size_t len) {
    if (!buf || len == 0) return;
    serial_write_async(buf, len);
}

void serial_write_direct(const char *buf, size_t len) {
    serial_write(buf, len);
}

static void serial_drain(uint64_t max_chars) {
    uint64_t flags;
    uint64_t drained;

    if (!serial_ring) return;
    flags = klog_irqsave();
    spin_lock(&serial_lock);
    drained = 0;
    while (drained < max_chars && serial_count > 0) {
        if (!serial_thr_empty()) break;
        outb(0x3F8, (uint8_t)serial_ring[serial_head]);
        serial_head = (serial_head + 1) % serial_ring_capacity;
        serial_count--;
        drained++;
    }
    spin_unlock(&serial_lock);
    klog_irqrestore(flags);
}

static int klog_enqueue(uint8_t level, const char *buf, uint64_t len) {
    uint64_t flags;
    klog_item_t *it;
    klog_item_t *new_ring;
    klog_item_t *old_ring;
    int ppos;
    int need;
    int newcap;
    int eavail;
    char *nb;
    uint64_t avail;
    uint64_t ring_len;
    uint64_t new_capacity;
    uint64_t i;

    if (!buf || len == 0) return 0;

    flags = klog_irqsave();
    spin_lock(&klog_persist_lock);
    if (klog_persist_buf) {
        ppos = klog_persist_pos;
        need = ppos + (int)len + 1;
        if (need > klog_persist_cap) {
            newcap = klog_persist_cap ? klog_persist_cap * 2 : need;
            while (newcap < need) newcap *= 2;
            if (newcap > KLOG_PERSIST_SZ) newcap = KLOG_PERSIST_SZ;
            nb = krealloc(klog_persist_buf, newcap);
            if (nb) {
                klog_persist_buf = nb;
                klog_persist_cap = newcap;
            }
        }
        avail = klog_persist_cap - ppos - 1;
        if ((int)len <= (int)avail && avail > 0) {
            memcpy(klog_persist_buf + ppos, buf, len);
            klog_persist_pos = ppos + (int)len;
            klog_persist_buf[klog_persist_pos] = '\0';
        }
    } else if (!klog_early_done) {
        eavail = KLOG_EARLY_SZ - klog_early_pos - 1;
        if ((int)len <= eavail && eavail > 0) {
            memcpy(klog_early_buf + klog_early_pos, buf, len);
            klog_early_pos += (int)len;
            klog_early_buf[klog_early_pos] = '\0';
        }
    } else {
        ppos = klog_early_pos;
        if (ppos >= KLOG_PERSIST_SZ) ppos = KLOG_PERSIST_SZ - 1;
        need = ppos + (int)len + 1;
        if (need > KLOG_PERSIST_SZ) need = KLOG_PERSIST_SZ;
        newcap = need;
        nb = (char *)kmalloc(newcap);
        if (nb) {
            klog_persist_buf = nb;
            klog_persist_cap = newcap;
            klog_persist_pos = ppos;
            if (ppos > 0) memcpy(klog_persist_buf, klog_early_buf, ppos);
            klog_persist_buf[klog_persist_pos] = '\0';
            avail = klog_persist_cap - ppos - 1;
            if ((int)len <= (int)avail && avail > 0) {
                memcpy(klog_persist_buf + ppos, buf, len);
                klog_persist_pos = ppos + (int)len;
                klog_persist_buf[klog_persist_pos] = '\0';
            }
        }
    }
    spin_unlock(&klog_persist_lock);
    klog_irqrestore(flags);

    ring_len = len;
    if (ring_len >= KLOG_MAX_LEN) ring_len = KLOG_MAX_LEN - 1;

    flags = klog_irqsave();
    if (!klog_ring || klog_count >= klog_capacity) {
        if (klog_capacity < KLOG_MAX_ITEMS) {
            new_capacity = klog_capacity ? klog_capacity * 2 : 2;
            if (new_capacity > KLOG_MAX_ITEMS) new_capacity = KLOG_MAX_ITEMS;
            new_ring = (klog_item_t *)kmalloc(new_capacity * sizeof(klog_item_t));
            if (new_ring) {
                old_ring = klog_ring;
                for (i = 0; i < klog_count; i++) {
                    if (old_ring) {
                        new_ring[i] = old_ring[(klog_head + i) % klog_capacity];
                    }
                }
                if (old_ring) kfree(old_ring);
                klog_ring = new_ring;
                klog_capacity = new_capacity;
                klog_head = 0;
                klog_tail = klog_count % klog_capacity;
            }
        }
    }
    if (!klog_ring || klog_count >= klog_capacity) {
        klog_dropped++;
        klog_irqrestore(flags);
        return (int)len;
    }
    it = &klog_ring[klog_tail];
    it->level = level;
    it->len = (uint16_t)ring_len;
    memcpy(it->msg, buf, ring_len);
    it->msg[ring_len] = '\0';
    klog_tail = (klog_tail + 1) % klog_capacity;
    klog_count++;
    klog_irqrestore(flags);
    waitq_wake_one(&kprint_waitq);
    return (int)len;
}

static int klog_dequeue(klog_item_t *out) {
    uint64_t flags;

    if (!out) return -1;
    if (!klog_ring || klog_capacity == 0) return -1;
    flags = klog_irqsave();
    if (klog_count == 0) {
        klog_irqrestore(flags);
        return -1;
    }
    *out = klog_ring[klog_head];
    klog_head = (klog_head + 1) % klog_capacity;
    klog_count--;
    klog_irqrestore(flags);
    return 0;
}

int klog_snapshot(char *buf, int bufsz) {
    int len;
    const char *src;

    src = klog_persist_buf;
    len = klog_persist_pos;
    if (!src && klog_early_pos > 0) {
        src = klog_early_buf;
        len = klog_early_pos;
    }
    if (!buf || bufsz <= 0) return len;

    if (!src || len <= 0) {
        buf[0] = '\0';
        return 0;
    }
    if (len >= bufsz) len = bufsz - 1;
    memcpy(buf, src, len);
    buf[len] = '\0';
    return len;
}

int klog_snapshot_range(char *buf, int offset, int count) {
    int len;
    const char *src;

    if (!buf || count <= 0) return 0;
    src = klog_persist_buf;
    len = klog_persist_pos;
    if (!src && klog_early_pos > 0) {
        src = klog_early_buf;
        len = klog_early_pos;
    }
    if (!src) return 0;
    if (offset >= len) return 0;
    if (offset + count > len) count = len - offset;
    memcpy(buf, src + offset, count);
    return count;
}

static int kprint_dequeue(kprint_item_t *out) {
    uint64_t flags;

    if (!out) return -1;
    if (!kprint_ring || kprint_capacity == 0) return -1;
    flags = klog_irqsave();
    if (kprint_count == 0) {
        klog_irqrestore(flags);
        return -1;
    }
    *out = kprint_ring[kprint_head];
    kprint_head = (kprint_head + 1) % kprint_capacity;
    kprint_count--;
    klog_irqrestore(flags);
    return 0;
}

int kprint_write(int console_id, const char *buf, size_t len) {
    size_t i;
    int con_id_early;
    int con_id;
    size_t off;
    uint64_t chunk;
    
    if (!buf || len == 0) return 0;

    if (!console_is_initialized()) {
        for (i = 0; i < len; i++) terminal_putchar(buf[i]);
        return (int)len;
    }

    if (!kprint_ready) {
        con_id_early = console_id;
        if (con_id_early < 0 || con_id_early >= NUM_CONSOLES) con_id_early = 0;
        console_write_to(con_id_early, buf, len);
        return (int)len;
    }

    con_id = console_id;
    if (con_id < 0 || con_id >= NUM_CONSOLES) con_id = 0;

    off = 0;
    while (off < len) {
        chunk = (uint64_t)(len - off);
        if (chunk >= (KPRINT_MAX_LEN - 1)) chunk = (KPRINT_MAX_LEN - 1);

        if (con_id == 0) {
            serial_write(buf + off, chunk);
        }

        if (!console_alt_screen_active(con_id))
            console_write_to_fb_only(con_id, buf + off, chunk);

        off += chunk;
    }

    return (int)len;
}

int klog_printf(int level, const char *fmt, ...) {
    char tmp[KLOG_MAX_LEN];
    va_list ap;
    int n;
    uint64_t len;
    
    (void)level;
    if (!fmt) return 0;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    len = (uint64_t)n;
    if (len >= sizeof(tmp)) len = (uint64_t)sizeof(tmp) - 1;
    return klog_enqueue((uint8_t)level, tmp, len);
}

int klog_enqueue_raw(const char *buf, size_t len) {
    if (!buf || len == 0) return 0;
    return klog_enqueue(0, buf, (uint64_t)len);
}

int klog_drain_console0(uint64_t max_items) {
    kproc_t *kp;
    kproc_t *prev;
    uint64_t drained;
    klog_item_t it;
    int suppress;

    if (max_items == 0) return 0;
    if (!console_is_initialized()) return 0;

    suppress = console_alt_screen_active(0);

    kp = kproc_get(-1);
    prev = current_kproc;
    if (kp) current_kproc = kp;

    drained = 0;
    while (drained < max_items && klog_dequeue(&it) == 0) {
        if (!suppress)
            console_write_to_fb_only(0, it.msg, (size_t)it.len);
        drained++;
    }

    current_kproc = prev;
    return (int)drained;
}

void kprint_poll(uint64_t max_items) {
    serial_drain(max_items ? max_items : 256);
    if (klog_count > 0 || kprint_count > 0) waitq_wake_one(&kprint_waitq);
}

void vring_init(void) {
    int i;

    klog_ring = NULL;
    klog_capacity = 0;
    kprint_ring = NULL;
    kprint_capacity = 0;
    klog_early_done = 1;

    klog_head = 0;
    klog_tail = 0;
    klog_count = 0;
    klog_dropped = 0;
    kprint_head = 0;
    kprint_tail = 0;
    kprint_count = 0;
    serial_head = 0;
    serial_tail = 0;
    serial_count = 0;

    subrings_count = VRING_MAX_SUBRINGS;
    subrings = kmalloc(subrings_count * sizeof(vring_t));
    if (subrings) {
        memset(subrings, 0, subrings_count * sizeof(vring_t));
        for (i = 0; i < subrings_count; i++) {
            subrings[i].ring_major = 0;
            subrings[i].ring_minor = (uint8_t)i;
            subrings[i].active = false;
            subrings[i].region_count = 0;
            subrings[i].name = NULL;
            subrings[i].caps = 0;
            subrings[i].flags = 0;
        }
    }
    
    vring_initialized = 1;
}

int vring_create(uint8_t minor, const char *name) {
    if (!subrings || minor >= subrings_count) return -1;
    if (minor == 0) return -1;
    if (subrings[minor].active) return -2;
    
    subrings[minor].ring_major = 0;
    subrings[minor].ring_minor = minor;
    subrings[minor].active = true;
    subrings[minor].region_count = 0;
    subrings[minor].name = name;
    subrings[minor].caps = 0;
    subrings[minor].flags = 0;
    subrings[minor].vring_pml4 = 0;
    
    return 0;
}

int vring_create_sandboxed(uint8_t minor, const char *name, uint8_t caps) {
    int ret;
    uint64_t pml4;

    ret = vring_create(minor, name);
    if (ret != 0) return ret;
    subrings[minor].caps = caps;
    subrings[minor].flags = VRING_FLAG_SANDBOXED;
    pml4 = vmm_create_vring_pml4();
    if (!pml4) {
        subrings[minor].active = false;
        subrings[minor].flags = 0;
        return -3;
    }
    subrings[minor].vring_pml4 = pml4;
    vring_add_region(minor, (uint64_t)_kernel_text_start, (uint64_t)_kernel_text_end,
                     VRING_PERM_READ | VRING_PERM_EXEC);
    vring_add_region(minor, (uint64_t)_kernel_text_end, (uint64_t)_kernel_rodata_end,
                     VRING_PERM_READ);
    vring_add_region(minor, (uint64_t)_kernel_rodata_end, (uint64_t)_kernel_end,
                     VRING_PERM_READ | VRING_PERM_WRITE);
    return 0;
}

int vring_add_region(uint8_t minor, uint64_t start, uint64_t end, uint8_t perms) {
    uint64_t idx;
    uint64_t flags;
    uint64_t pte_flags;
    uint64_t v;
    uint64_t phys;
    uint64_t kernel_cr3;

    if (!subrings || minor >= subrings_count) return -1;
    if (minor == 0) return -1;
    if (!subrings[minor].active) return -2;
    if (subrings[minor].region_count >= VRING_MAX_REGIONS) return -3;
    if (start >= end) return -4;
    if (perms == 0) return -5;

    flags = klog_irqsave();
    spin_lock(&vring_lock);
    idx = subrings[minor].region_count;
    subrings[minor].allowed_regions[idx].start = start;
    subrings[minor].allowed_regions[idx].end = end;
    subrings[minor].allowed_regions[idx].permissions = perms;
    subrings[minor].region_count++;

    if (subrings[minor].vring_pml4) {
        pte_flags = 0x1;
        if (perms & VRING_PERM_WRITE) pte_flags |= 0x2;
        if (!(perms & VRING_PERM_EXEC)) pte_flags |= VRING_PTE_NX;
        kernel_cr3 = vmm_get_kernel_cr3();
        for (v = start & ~(PAGE_SIZE - 1); v < end; v += PAGE_SIZE) {
            phys = vmm_get_phys_in_pml4(read_cr3(), v);
            if (phys) {
                if (kernel_cr3 && !vmm_get_phys_in_pml4(kernel_cr3, v)) {
                    vmm_map_page_in_pml4(kernel_cr3, v, phys, pte_flags);
                }
                vmm_map_page_in_pml4(subrings[minor].vring_pml4, v, phys, pte_flags);
            }
        }
    }

    spin_unlock(&vring_lock);
    klog_irqrestore(flags);

    return 0;
}

int vring_remove(uint8_t minor) {
    uint64_t pml4;

    if (!subrings || minor >= subrings_count) return -1;
    if (!subrings[minor].active) return -2;

    pml4 = subrings[minor].vring_pml4;
    subrings[minor].active = false;
    subrings[minor].region_count = 0;
    subrings[minor].name = NULL;
    subrings[minor].vring_pml4 = 0;

    if (pml4) {
        vmm_free_vring_pml4(pml4);
    }

    return 0;
}

vring_t *vring_get(uint8_t minor) {
    if (!subrings || minor >= subrings_count) return NULL;
    if (!subrings[minor].active) return NULL;
    return &subrings[minor];
}

bool vring_check_access(uint8_t minor, uint64_t addr, uint64_t size, uint8_t access_type) {
    vring_t *ring;
    uint64_t end_addr;
    uint64_t i;
    uint64_t flags;
    bool result;

    if (!vring_initialized) return false;
    if (minor == 0) return true;
    if (!subrings || minor >= subrings_count) return false;
    if (!subrings[minor].active) return false;
    if (size == 0) return false;
    if (addr > (uint64_t)(-1) - size) return false;
    
    ring = &subrings[minor];
    end_addr = addr + size;

    flags = klog_irqsave();
    spin_lock(&vring_lock);
    result = false;
    for (i = 0; i < ring->region_count; i++) {
        vring_mem_region_t *region = &ring->allowed_regions[i];
        
        if (region->start >= region->end) continue;
        if (addr >= region->start && end_addr <= region->end) {
            if ((region->permissions & access_type) == access_type) {
                result = true;
                break;
            }
        }
    }
    spin_unlock(&vring_lock);
    klog_irqrestore(flags);
    
    return result;
}

bool vring_check_cap(uint8_t minor, uint8_t cap) {
    if (!vring_initialized) return false;
    if (minor == 0) return true;
    if (!subrings || minor >= subrings_count) return false;
    if (!subrings[minor].active) return false;
    return (subrings[minor].caps & cap) != 0;
}

void vring_handle_violation(uint8_t minor, uint64_t addr, uint8_t access_type) {
    bool sandboxed;
    uint64_t flags;
    int i;
    kproc_t *proc;

    if (!vring_initialized || !subrings || minor >= subrings_count) {
        vring_panic_forbidden(minor, addr, access_type);
        return;
    }

    flags = klog_irqsave();
    spin_lock(&vring_lock);
    sandboxed = subrings[minor].active && (subrings[minor].flags & VRING_FLAG_SANDBOXED);
    spin_unlock(&vring_lock);
    klog_irqrestore(flags);

    if (!sandboxed) {
        vring_panic_forbidden(minor, addr, access_type);
        return;
    }

    for (i = 0; i < kprocs_count; i++) {
        proc = &kernel_procs[i];
        if (proc->state != KPROC_STATE_NONE && proc->vring_minor == minor) {
            proc->state = KPROC_STATE_DEAD;
        }
    }

    if (current_task && !current_task->is_user && current_task->vring_minor == minor) {
        if (minor == 7) vring_selftest_violation_seen = 1;
        task_exit_deferred(1);
    }
}

void vring_panic_forbidden(uint8_t minor, uint64_t addr, uint8_t access_type) {
    char reason_buf[256];
    int off;
    int rem;
    int n;
    int i;
    vring_t *ring;

    off = 0;
    rem = (int)sizeof(reason_buf) - 1;

    n = snprintf(reason_buf + off, (size_t)rem, "Virtual Ring Violation - Ring 0.");
    if (n > 0 && n < rem) { off += n; rem -= n; }

    n = snprintf(reason_buf + off, (size_t)rem, "%d", minor);
    if (n > 0 && n < rem) { off += n; rem -= n; }

    if (subrings && minor < subrings_count && subrings[minor].name) {
        n = snprintf(reason_buf + off, (size_t)rem, " (%s)", subrings[minor].name);
        if (n > 0 && n < rem) { off += n; rem -= n; }
    }

    n = snprintf(reason_buf + off, (size_t)rem,
                 " forbidden access at 0x%016lX ", addr);
    if (n > 0 && n < rem) { off += n; rem -= n; }

    if (rem > 8 && (access_type & VRING_PERM_READ)) {
        n = snprintf(reason_buf + off, (size_t)rem, "READ ");
        if (n > 0 && n < rem) { off += n; rem -= n; }
    }
    if (rem > 8 && (access_type & VRING_PERM_WRITE)) {
        n = snprintf(reason_buf + off, (size_t)rem, "WRITE ");
        if (n > 0 && n < rem) { off += n; rem -= n; }
    }
    if (rem > 8 && (access_type & VRING_PERM_EXEC)) {
        n = snprintf(reason_buf + off, (size_t)rem, "EXEC ");
        if (n > 0 && n < rem) { off += n; rem -= n; }
    }

    ring = vring_get(minor);
    if (ring && rem > 20) {
        n = snprintf(reason_buf + off, (size_t)rem, "\nAllowed regions: ");
        if (n > 0 && n < rem) { off += n; rem -= n; }
        for (i = 0; (uint64_t)i < ring->region_count && i < 3 && rem > 40; i++) {
            n = snprintf(reason_buf + off, (size_t)rem,
                "[0x%016lX-0x%016lX] ",
                ring->allowed_regions[i].start,
                ring->allowed_regions[i].end);
            if (n > 0 && n < rem) { off += n; rem -= n; }
        }
    }
    reason_buf[sizeof(reason_buf) - 1] = '\0';
    
    kernel_panic(reason_buf, NULL);
}

void kproc_init(void) {
    int i;

    kprocs_count = KPROC_MAX;
    kernel_procs = kmalloc(kprocs_count * sizeof(kproc_t));
    if (kernel_procs) {
        memset(kernel_procs, 0, kprocs_count * sizeof(kproc_t));
        for (i = 0; i < kprocs_count; i++) {
            kernel_procs[i].pid = 0;
            kernel_procs[i].state = KPROC_STATE_NONE;
            kernel_procs[i].vring_minor = 0;
            kernel_procs[i].name = NULL;
            kernel_procs[i].entry = NULL;
            kernel_procs[i].private_data = NULL;
            kernel_procs[i].msg_head = 0;
            kernel_procs[i].msg_tail = 0;
            kernel_procs[i].msg_count = 0;
        }
    }
    current_kproc = NULL;
    next_kproc_pid = KPROC_PID_BASE;
    kproc_initialized = 1;
}

int32_t kproc_create(const char *name, uint8_t vring_minor, kproc_entry_t entry, void *priv) {
    int slot;
    int i;
    int32_t pid;

    if (!kproc_initialized) return 0;
    if (!kernel_procs) return 0;
    
    slot = -1;
    for (i = 0; i < kprocs_count; i++) {
        if (kernel_procs[i].state == KPROC_STATE_NONE) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) return 0;
    
    pid = next_kproc_pid;
    next_kproc_pid--;
    
    kernel_procs[slot].pid = pid;
    kernel_procs[slot].state = KPROC_STATE_RUNNING;
    kernel_procs[slot].vring_minor = vring_minor;
    kernel_procs[slot].name = name;
    kernel_procs[slot].entry = entry;
    kernel_procs[slot].private_data = priv;
    kernel_procs[slot].msg_head = 0;
    kernel_procs[slot].msg_tail = 0;
    kernel_procs[slot].msg_count = 0;
    
    return pid;
}

kproc_t *kproc_get(int32_t pid) {
    int i;

    if (pid >= 0) return NULL;
    if (!kernel_procs) return NULL;
    
    for (i = 0; i < kprocs_count; i++) {
        if (kernel_procs[i].pid == pid && kernel_procs[i].state != KPROC_STATE_NONE) {
            return &kernel_procs[i];
        }
    }
    return NULL;
}

kproc_t *kproc_current(void) {
    return current_kproc;
}

void kproc_set_current(int32_t pid) {
    if (pid >= 0) {
        current_kproc = NULL;
    } else {
        current_kproc = kproc_get(pid);
    }
}

int kproc_send_msg(int32_t pid, uint64_t msg) {
    kproc_t *proc = kproc_get(pid);
    if (!proc) return -1;
    if (proc->msg_count >= 16) return -2;
    
    proc->msg_queue[proc->msg_tail] = msg;
    proc->msg_tail = (proc->msg_tail + 1) % 16;
    proc->msg_count++;
    
    return 0;
}

int kproc_recv_msg(int32_t pid, uint64_t *msg) {
    kproc_t *proc = kproc_get(pid);
    if (!proc) return -1;
    if (proc->msg_count == 0) return -2;
    
    *msg = proc->msg_queue[proc->msg_head];
    proc->msg_head = (proc->msg_head + 1) % 16;
    proc->msg_count--;
    
    return 0;
}

void kproc_yield(void) {
}

void kproc_print_char(char c) {
    char b = c;
    kprint_write(0, &b, 1);
}

void kproc_print_string(const char *str, size_t len) {
    kprint_write(0, str, len);
}

void kproc_debug_log(const char *msg, int level) {
    (void)level;
    
    if (!msg) return;
    klog_enqueue((uint8_t)level, msg, (uint64_t)klog_strnlen(msg, KLOG_MAX_LEN - 2));
    klog_enqueue((uint8_t)level, "\n", 1);
}

static void klog_task_main(void) {
    while (1) {
        uint64_t did;
        uint64_t drained;
        kprint_item_t it;
        klog_item_t kit;
        int con_id;
        uint64_t backlog;

        waitq_wait(&kprint_waitq);

        while (kprint_count > 0 || klog_count > 0) {
            did = 0;

            drained = 0;
            while (drained < 32 && kprint_dequeue(&it) == 0) {
                con_id = it.con_id;
                if (con_id < 0 || con_id >= NUM_CONSOLES) con_id = 0;
                if (!console_alt_screen_active(con_id))
                    console_write_to_fb_only(con_id, it.msg, (size_t)it.len);
                drained++;
            }
            did += drained;

            drained = 0;
            while (drained < 8 && klog_dequeue(&kit) == 0) {
                drained++;
            }
            did += drained;

            if (did == 0) break;

            backlog = kprint_count + klog_count;
            if (backlog >= 256) {
                sleep_ms(1);
            } else {
                yield();
            }
        }
    }
}

void kproc_print_init(void) {
    int32_t pid;
    task_t *t;

    vring_create(1, "kprint");

    waitq_init(&kprint_waitq);
    
    vring_add_region(1, HEAP_START, kernel_heap.max_addr, 
                     VRING_PERM_READ | VRING_PERM_WRITE);
    
    vring_add_region(1, KERNEL_VMA, KERNEL_VMA + 0x400000,
                     VRING_PERM_READ | VRING_PERM_EXEC);
    
    vring_add_region(1, KERNEL_VMA + 0xFD000000ULL, KERNEL_VMA + 0xFE000000ULL,
                     VRING_PERM_READ | VRING_PERM_WRITE);
    
    vring_add_region(1, KERNEL_VMA + 0xB8000, KERNEL_VMA + 0xB9000,
                     VRING_PERM_READ | VRING_PERM_WRITE);
    
    pid = kproc_create("kprint", 1, NULL, NULL);

    if (pid == -1) {
        t = create_kernel_task(klog_task_main, TASK_READY);
        if (t) {
            t->pid = pid;
            t->is_user = false;
            task_set_vring(t, 1);
            t->console_id = 0;
            strcpy(t->name, "klog");
            lock_scheduler();
            add_task_to_runqueue(t);
            unlock_scheduler();
        }
        printf("VRING: Kernel print process created (PID %d, ring 0.1)\n", pid);
    }
}

static void vring_selftest_sandbox_task(void) {
    volatile uint8_t *allowed;
    volatile uint8_t *forbidden;
    uint8_t value;

    allowed = (volatile uint8_t *)vring_selftest_buf;
    forbidden = (volatile uint8_t *)(vring_selftest_buf + PAGE_SIZE);

    vring_selftest_stage = 1;
    allowed[0] = 0x5A;
    value = allowed[0];
    if (value != 0x5A) {
        vring_selftest_failed = 1;
        task_exit(2);
    }
    if (read_cr3() != vring_selftest_pml4) {
        vring_selftest_failed = 2;
        task_exit(3);
    }
    vring_selftest_stage = 2;
    value = forbidden[0];
    vring_selftest_failed = 3 + value;
    task_exit(4);
}

static void vring_selftest_cleanup(void) {
    vring_remove(7);
    if (vring_selftest_buf) {
        kfree_aligned(vring_selftest_buf);
        vring_selftest_buf = NULL;
    }
    vring_selftest_pml4 = 0;
    vring_selftest_task_ref = NULL;
}

static void vring_selftest_supervisor(void) {
    task_t *t;
    int i;
    int ret;
    uint64_t allowed_addr;
    uint64_t forbidden_addr;
    uint64_t allowed_phys;
    uint64_t forbidden_phys;
    vring_t *ring;

    printf("VRINGTEST: starting sandboxed VRing checks\n");
    vring_selftest_violation_seen = 0;

    ret = vring_create_sandboxed(7, "selftest", 0);
    if (ret != 0) {
        printf("VRINGTEST: FAIL create sandboxed ring ret=%d\n", ret);
        task_exit(1);
        return;
    }

    vring_selftest_buf = kmalloc_aligned(PAGE_SIZE * 2, PAGE_SIZE);
    if (!vring_selftest_buf) {
        printf("VRINGTEST: FAIL allocation\n");
        vring_selftest_cleanup();
        task_exit(1);
        return;
    }

    allowed_addr = (uint64_t)vring_selftest_buf;
    forbidden_addr = allowed_addr + PAGE_SIZE;
    ret = vring_add_region(7, allowed_addr, allowed_addr + PAGE_SIZE,
                           VRING_PERM_READ | VRING_PERM_WRITE);
    if (ret != 0) {
        printf("VRINGTEST: FAIL add allowed page ret=%d\n", ret);
        vring_selftest_cleanup();
        task_exit(1);
        return;
    }

    ring = vring_get(7);
    if (!ring || !ring->vring_pml4) {
        printf("VRINGTEST: FAIL missing sandbox PML4\n");
        vring_selftest_cleanup();
        task_exit(1);
        return;
    }
    vring_selftest_pml4 = ring->vring_pml4;

    allowed_phys = vmm_get_phys_in_pml4(ring->vring_pml4, allowed_addr);
    forbidden_phys = vmm_get_phys_in_pml4(ring->vring_pml4, forbidden_addr);
    if (!allowed_phys || forbidden_phys) {
        printf("VRINGTEST: FAIL page map allowed=0x%016lX forbidden=0x%016lX\n",
               allowed_phys, forbidden_phys);
        vring_selftest_cleanup();
        task_exit(1);
        return;
    }

    t = create_kernel_task(vring_selftest_sandbox_task, TASK_READY);
    if (!t) {
        printf("VRINGTEST: FAIL create task\n");
        vring_selftest_cleanup();
        task_exit(1);
        return;
    }
    if (current_task && current_task->stack_base && current_task->stack_size) {
        vring_add_region(7, (uint64_t)current_task->stack_base,
                         (uint64_t)current_task->stack_base + current_task->stack_size,
                         VRING_PERM_READ | VRING_PERM_WRITE);
    }
    vring_selftest_task_ref = t;
    task_set_vring(t, 7);
    strcpy(t->name, "vringtest");
    lock_scheduler();
    add_task_to_runqueue(t);
    unlock_scheduler();

    for (i = 0; i < 200; i++) {
        if (vring_selftest_violation_seen) break;
        yield();
    }

    if (vring_selftest_failed) {
        printf("VRINGTEST: FAIL sandbox task escaped with code=%d\n", vring_selftest_failed);
    } else if (vring_selftest_stage != 2) {
        printf("VRINGTEST: FAIL sandbox task stopped at stage=%d state=%d\n",
               vring_selftest_stage, vring_selftest_task_ref->state);
    } else if (!vring_selftest_violation_seen) {
        printf("VRINGTEST: FAIL forbidden access did not kill task\n");
    } else {
        printf("VRINGTEST: PASS allowed access worked and forbidden access killed sandboxed task\n");
    }

    if (vring_selftest_task_ref && vring_selftest_task_ref->state != TASK_DEAD)
        task_kill(vring_selftest_task_ref, 1);
    vring_selftest_cleanup();
    task_exit(0);
}

void vring_selftest_start(void) {
    task_t *t;

    if (vring_selftest_started) return;
    vring_selftest_started = 1;

    t = create_kernel_task(vring_selftest_supervisor, TASK_READY);
    if (!t) {
        printf("VRINGTEST: FAIL supervisor create\n");
        return;
    }
    strcpy(t->name, "vringtestd");
    lock_scheduler();
    add_task_to_runqueue(t);
    unlock_scheduler();
}

bool kprint_is_ready(void) {
    return kprint_ready != 0;
}

void kprint_enable(void) {
    kprint_ready = 1;
}

void kprint_flush(void) {
    kprint_item_t it;
    klog_item_t kit;
    uint64_t total;
    uint64_t retries;
    int con_id;

    retries = 0;
    do {
        total = 0;
        while (kprint_dequeue(&it) == 0) {
            con_id = it.con_id;
            if (con_id < 0 || con_id >= NUM_CONSOLES) con_id = 0;
            if (!console_alt_screen_active(con_id))
                console_write_to_fb_only(con_id, it.msg, (size_t)it.len);
            total++;
        }
        while (klog_dequeue(&kit) == 0) {
            total++;
        }
        retries++;
    } while (total > 0 && retries < 1024);
}

void kprint_serial_async(const char *buf, size_t len) {
    size_t i;
    size_t start;
    int in_esc;
    uint64_t flags;

    if (!buf || len == 0) return;
    if (!serial_ring) return;

    flags = klog_irqsave();
    spin_lock(&serial_lock);

    in_esc = 0;
    start = 0;
    for (i = 0; i < len; i++) {
        if (in_esc) {
            if ((buf[i] >= 'A' && buf[i] <= 'Z') ||
                (buf[i] >= 'a' && buf[i] <= 'z')) {
                in_esc = 0;
                start = i + 1;
            }
            continue;
        }
        if (buf[i] == '\033') {
            if (i > start) {
                serial_enqueue_nolock(buf + start, i - start);
            }
            in_esc = 1;
            start = i;
            continue;
        }
    }
    if (!in_esc && i > start) {
        serial_enqueue_nolock(buf + start, i - start);
    }

    spin_unlock(&serial_lock);
    klog_irqrestore(flags);
}

bool kproc_is_negative_pid(int32_t pid) {
    return pid < 0;
}

kproc_t *kproc_find_by_pid(int32_t pid) {
    if (pid >= 0) return NULL;
    return kproc_get(pid);
}
