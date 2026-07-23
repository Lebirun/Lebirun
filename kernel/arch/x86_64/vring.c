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

static vring_t *subrings;
static kproc_t *kernel_procs;
kproc_t *current_kproc = NULL;
static int32_t next_kproc_pid = KPROC_PID_BASE;
static volatile int vring_initialized = 0;
static spinlock_t vring_lock = { .locked = 0 };
static volatile int kproc_initialized = 0;
static spinlock_t kproc_lock = { .locked = 0 };

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
static spinlock_t klog_ring_lock = { .locked = 0 };
static volatile uint64_t klog_head = 0;
static volatile uint64_t klog_tail = 0;
static volatile uint64_t klog_count = 0;
static volatile uint64_t klog_dropped = 0;

#define KLOG_PERSIST_SZ 32768
#define KLOG_RECLAIM_RETAIN 4096
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

static spinlock_t serial_lock = { .locked = 0 };
static spinlock_t klog_persist_lock = { .locked = 0 };

typedef struct kproc_msg {
    uint64_t value;
    struct kproc_msg *next;
} kproc_msg_t;

static vring_t *vring_find(uint8_t minor) {
    vring_t *ring;

    ring = subrings;
    while (ring) {
        if (ring->ring_minor == minor && ring->active) return ring;
        ring = ring->next;
    }
    return NULL;
}

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

static int serial_wait_thr_empty(void) {
    uint64_t attempts;

    attempts = 0;
    while (!serial_thr_empty()) {
        attempts++;
        if (attempts >= 0x100000ULL) return 0;
        cpu_relax();
    }
    return 1;
}

static void serial_write_nolock(const char *buf, size_t len) {
    size_t i;
    uint8_t ch;

    for (i = 0; i < len; i++) {
        ch = (uint8_t)buf[i];
        if (ch == '\n') {
            if (!serial_wait_thr_empty()) return;
            outb(0x3F8, '\r');
        }
        if (!serial_wait_thr_empty()) return;
        outb(0x3F8, ch);
    }
}

static void serial_write_async(const char *buf, size_t len) {
    uint64_t flags;

    flags = klog_irqsave();
    spin_lock(&serial_lock);
    serial_write_nolock(buf, len);
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
    (void)max_chars;
}

static void klog_persist_append_locked(const char *buf, uint64_t len) {
    int ppos;
    int need;
    int newcap;
    int discard;
    int remaining;
    char *nb;
    uint64_t avail;

    if (!klog_persist_buf || !buf || len == 0) return;
    if (len >= KLOG_PERSIST_SZ) {
        buf += len - (KLOG_PERSIST_SZ - 1);
        len = KLOG_PERSIST_SZ - 1;
    }

    ppos = klog_persist_pos;
    need = ppos + (int)len + 1;
    if (need > klog_persist_cap && klog_persist_cap < KLOG_PERSIST_SZ) {
        newcap = (need + 4095) & ~4095;
        if (newcap > KLOG_PERSIST_SZ) newcap = KLOG_PERSIST_SZ;
        nb = krealloc(klog_persist_buf, newcap);
        if (nb) {
            klog_persist_buf = nb;
            klog_persist_cap = newcap;
        }
    }

    avail = (uint64_t)(klog_persist_cap - ppos - 1);
    if (len > avail && klog_persist_cap == KLOG_PERSIST_SZ) {
        discard = (int)(len - avail);
        while (discard < ppos && klog_persist_buf[discard - 1] != '\n') discard++;
        remaining = ppos - discard;
        if (remaining > 0) {
            memmove(klog_persist_buf, klog_persist_buf + discard, (size_t)remaining);
        }
        ppos = remaining;
        klog_persist_pos = ppos;
        avail = (uint64_t)(klog_persist_cap - ppos - 1);
    }

    if (len <= avail) {
        memcpy(klog_persist_buf + ppos, buf, (size_t)len);
        klog_persist_pos = ppos + (int)len;
        klog_persist_buf[klog_persist_pos] = '\0';
    }
}

void klog_persist_enable(void) {
    uint64_t flags;

    flags = klog_irqsave();
    spin_lock(&klog_persist_lock);
    klog_early_done = 1;
    spin_unlock(&klog_persist_lock);
    klog_irqrestore(flags);
}

void klog_reclaim_unused(void) {
    uint64_t flags;
    char *new_buf;
    klog_item_t *old_ring;
    int discard;
    int raw_discard;
    int needed;

    flags = klog_irqsave();
    spin_lock(&klog_persist_lock);
    if (klog_persist_buf && klog_persist_pos >= KLOG_RECLAIM_RETAIN) {
        raw_discard = klog_persist_pos - KLOG_RECLAIM_RETAIN + 1;
        discard = raw_discard;
        while (discard < klog_persist_pos &&
               klog_persist_buf[discard - 1] != '\n') discard++;
        if (discard >= klog_persist_pos) discard = raw_discard;
        klog_persist_pos -= discard;
        if (klog_persist_pos > 0) {
            memmove(klog_persist_buf, klog_persist_buf + discard,
                    (size_t)klog_persist_pos);
        }
        klog_persist_buf[klog_persist_pos] = '\0';
    }
    needed = klog_persist_pos + 1;
    if (klog_persist_buf && needed > 0 && needed < klog_persist_cap) {
        new_buf = (char *)krealloc(klog_persist_buf, (size_t)needed);
        if (new_buf) {
            klog_persist_buf = new_buf;
            klog_persist_cap = needed;
        }
    }
    spin_unlock(&klog_persist_lock);
    klog_irqrestore(flags);

    old_ring = NULL;
    flags = klog_irqsave();
    spin_lock(&klog_ring_lock);
    if (klog_count == 0 && klog_ring) {
        old_ring = klog_ring;
        klog_ring = NULL;
        klog_capacity = 0;
        klog_head = 0;
        klog_tail = 0;
    }
    spin_unlock(&klog_ring_lock);
    klog_irqrestore(flags);
    if (old_ring) kfree(old_ring);
}

static int klog_enqueue(uint8_t level, const char *buf, uint64_t len) {
    uint64_t flags;
    klog_item_t *it;
    klog_item_t *new_ring;
    klog_item_t *old_ring;
    int ppos;
    int newcap;
    int eavail;
    char *nb;
    uint64_t ring_len;
    uint64_t new_capacity;
    uint64_t i;

    if (!buf || len == 0) return 0;

    flags = klog_irqsave();
    spin_lock(&klog_persist_lock);
    if (klog_persist_buf) {
        klog_persist_append_locked(buf, len);
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
        newcap = ppos + (int)len + 1;
        if (newcap > KLOG_PERSIST_SZ) newcap = KLOG_PERSIST_SZ;
        nb = (char *)kmalloc(newcap);
        if (nb) {
            klog_persist_buf = nb;
            klog_persist_cap = newcap;
            klog_persist_pos = ppos;
            if (ppos > 0) memcpy(klog_persist_buf, klog_early_buf, ppos);
            klog_persist_buf[klog_persist_pos] = '\0';
            klog_persist_append_locked(buf, len);
        }
    }
    spin_unlock(&klog_persist_lock);
    klog_irqrestore(flags);

    ring_len = len;
    if (ring_len >= KLOG_MAX_LEN) ring_len = KLOG_MAX_LEN - 1;

    flags = klog_irqsave();
    spin_lock(&klog_ring_lock);
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
        spin_unlock(&klog_ring_lock);
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
    spin_unlock(&klog_ring_lock);
    klog_irqrestore(flags);
    waitq_wake_one(&kprint_waitq);
    return (int)len;
}

static int klog_dequeue(klog_item_t *out) {
    uint64_t flags;

    if (!out) return -1;
    flags = klog_irqsave();
    spin_lock(&klog_ring_lock);
    if (!klog_ring || klog_capacity == 0 || klog_count == 0) {
        spin_unlock(&klog_ring_lock);
        klog_irqrestore(flags);
        return -1;
    }
    *out = klog_ring[klog_head];
    klog_head = (klog_head + 1) % klog_capacity;
    klog_count--;
    spin_unlock(&klog_ring_lock);
    klog_irqrestore(flags);
    return 0;
}

int klog_snapshot(char *buf, int bufsz) {
    int len;
    const char *src;
    uint64_t flags;

    flags = klog_irqsave();
    spin_lock(&klog_persist_lock);
    src = klog_persist_buf;
    len = klog_persist_pos;
    if (!src && klog_early_pos > 0) {
        src = klog_early_buf;
        len = klog_early_pos;
    }
    if (!buf || bufsz <= 0) {
        spin_unlock(&klog_persist_lock);
        klog_irqrestore(flags);
        return len;
    }

    if (!src || len <= 0) {
        buf[0] = '\0';
        spin_unlock(&klog_persist_lock);
        klog_irqrestore(flags);
        return 0;
    }
    if (len >= bufsz) len = bufsz - 1;
    memcpy(buf, src, len);
    buf[len] = '\0';
    spin_unlock(&klog_persist_lock);
    klog_irqrestore(flags);
    return len;
}

int klog_snapshot_range(char *buf, int offset, int count) {
    int len;
    const char *src;
    uint64_t flags;

    if (!buf || count <= 0) return 0;
    flags = klog_irqsave();
    spin_lock(&klog_persist_lock);
    src = klog_persist_buf;
    len = klog_persist_pos;
    if (!src && klog_early_pos > 0) {
        src = klog_early_buf;
        len = klog_early_pos;
    }
    if (!src || offset >= len) {
        spin_unlock(&klog_persist_lock);
        klog_irqrestore(flags);
        return 0;
    }
    if (offset + count > len) count = len - offset;
    memcpy(buf, src + offset, count);
    spin_unlock(&klog_persist_lock);
    klog_irqrestore(flags);
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

void KERNEL_INIT vring_init(void) {
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

    subrings = NULL;
    
    vring_initialized = 1;
}

int vring_create(uint8_t minor, const char *name) {
    vring_t *ring;
    uint64_t irq_flags;

    if (minor == 0) return -1;
    ring = (vring_t *)kmalloc(sizeof(vring_t));
    if (!ring) return -1;
    memset(ring, 0, sizeof(vring_t));
    ring->ring_minor = minor;
    ring->active = true;
    ring->name = name;

    irq_flags = klog_irqsave();
    spin_lock(&vring_lock);
    if (vring_find(minor)) {
        spin_unlock(&vring_lock);
        klog_irqrestore(irq_flags);
        kfree(ring);
        return -2;
    }
    ring->next = subrings;
    subrings = ring;
    spin_unlock(&vring_lock);
    klog_irqrestore(irq_flags);

    return 0;
}

int vring_create_sandboxed(uint8_t minor, const char *name, uint8_t caps) {
    int ret;
    uint64_t pml4;
    vring_t *ring;

    ret = vring_create(minor, name);
    if (ret != 0) return ret;
    ring = vring_get(minor);
    if (!ring) return -2;
    ring->caps = caps;
    ring->flags = VRING_FLAG_SANDBOXED;
    pml4 = vmm_create_vring_pml4();
    if (!pml4) {
        vring_remove(minor);
        return -3;
    }
    ring->vring_pml4 = pml4;
    vring_add_region(minor, (uint64_t)_kernel_text_start, (uint64_t)_kernel_text_end,
                     VRING_PERM_READ | VRING_PERM_EXEC);
    vring_add_region(minor, (uint64_t)_kernel_text_end, (uint64_t)_kernel_rodata_end,
                     VRING_PERM_READ);
    vring_add_region(minor, (uint64_t)_kernel_rodata_end, (uint64_t)_kernel_end,
                     VRING_PERM_READ | VRING_PERM_WRITE);
    return 0;
}

int vring_add_region(uint8_t minor, uint64_t start, uint64_t end, uint8_t perms) {
    uint64_t flags;
    uint64_t pte_flags;
    uint64_t v;
    uint64_t phys;
    uint64_t kernel_cr3;
    vring_t *ring;
    vring_mem_region_t *region;

    if (minor == 0) return -1;
    if (start >= end) return -4;
    if (perms == 0) return -5;
    region = (vring_mem_region_t *)kmalloc(sizeof(vring_mem_region_t));
    if (!region) return -3;
    region->start = start;
    region->end = end;
    region->permissions = perms;
    region->next = NULL;

    flags = klog_irqsave();
    spin_lock(&vring_lock);
    ring = vring_find(minor);
    if (!ring) {
        spin_unlock(&vring_lock);
        klog_irqrestore(flags);
        kfree(region);
        return -2;
    }
    if (ring->regions_tail) ring->regions_tail->next = region;
    else ring->allowed_regions = region;
    ring->regions_tail = region;
    ring->region_count++;

    if (ring->vring_pml4) {
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
                vmm_map_page_in_pml4(ring->vring_pml4, v, phys, pte_flags);
            }
        }
    }

    spin_unlock(&vring_lock);
    klog_irqrestore(flags);

    return 0;
}

int vring_remove(uint8_t minor) {
    uint64_t pml4;
    uint64_t flags;
    vring_t *ring;
    vring_t *previous;
    vring_mem_region_t *region;
    vring_mem_region_t *next_region;

    if (minor == 0) return -1;
    flags = klog_irqsave();
    spin_lock(&vring_lock);
    previous = NULL;
    ring = subrings;
    while (ring && ring->ring_minor != minor) {
        previous = ring;
        ring = ring->next;
    }
    if (!ring) {
        spin_unlock(&vring_lock);
        klog_irqrestore(flags);
        return -2;
    }
    if (previous) previous->next = ring->next;
    else subrings = ring->next;
    ring->active = false;
    spin_unlock(&vring_lock);
    klog_irqrestore(flags);

    pml4 = ring->vring_pml4;
    if (pml4) {
        vmm_free_vring_pml4(pml4);
    }
    region = ring->allowed_regions;
    while (region) {
        next_region = region->next;
        kfree(region);
        region = next_region;
    }
    kfree(ring);

    return 0;
}

vring_t *vring_get(uint8_t minor) {
    if (minor == 0) return NULL;
    return vring_find(minor);
}

bool vring_check_access(uint8_t minor, uint64_t addr, uint64_t size, uint8_t access_type) {
    vring_t *ring;
    vring_mem_region_t *region;
    uint64_t end_addr;
    uint64_t flags;
    bool result;

    if (!vring_initialized) return false;
    if (minor == 0) return true;
    if (size == 0) return false;
    if (addr > (uint64_t)(-1) - size) return false;
    end_addr = addr + size;

    flags = klog_irqsave();
    spin_lock(&vring_lock);
    ring = vring_find(minor);
    result = false;
    region = ring ? ring->allowed_regions : NULL;
    while (region) {
        if (region->start < region->end &&
            addr >= region->start && end_addr <= region->end) {
            if ((region->permissions & access_type) == access_type) {
                result = true;
                break;
            }
        }
        region = region->next;
    }
    spin_unlock(&vring_lock);
    klog_irqrestore(flags);
    
    return result;
}

bool vring_check_cap(uint8_t minor, uint8_t cap) {
    vring_t *ring;

    if (!vring_initialized) return false;
    if (minor == 0) return true;
    ring = vring_get(minor);
    if (!ring) return false;
    return (ring->caps & cap) != 0;
}

void vring_handle_violation(uint8_t minor, uint64_t addr, uint8_t access_type) {
    bool sandboxed;
    uint64_t flags;
    kproc_t *proc;
    vring_t *ring;

    if (!vring_initialized) {
        vring_panic_forbidden(minor, addr, access_type);
        return;
    }

    flags = klog_irqsave();
    spin_lock(&vring_lock);
    ring = vring_find(minor);
    sandboxed = ring && (ring->flags & VRING_FLAG_SANDBOXED);
    spin_unlock(&vring_lock);
    klog_irqrestore(flags);

    if (!sandboxed) {
        vring_panic_forbidden(minor, addr, access_type);
        return;
    }

    proc = kernel_procs;
    while (proc) {
        if (proc->state != KPROC_STATE_NONE && proc->vring_minor == minor) {
            proc->state = KPROC_STATE_DEAD;
        }
        proc = proc->next;
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
    vring_t *ring;
    vring_mem_region_t *region;
    int displayed;

    off = 0;
    rem = (int)sizeof(reason_buf) - 1;

    n = snprintf(reason_buf + off, (size_t)rem, "Virtual Ring Violation - Ring 0.");
    if (n > 0 && n < rem) { off += n; rem -= n; }

    n = snprintf(reason_buf + off, (size_t)rem, "%d", minor);
    if (n > 0 && n < rem) { off += n; rem -= n; }

    ring = vring_get(minor);
    if (ring && ring->name) {
        n = snprintf(reason_buf + off, (size_t)rem, " (%s)", ring->name);
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

    if (ring && rem > 20) {
        n = snprintf(reason_buf + off, (size_t)rem, "\nAllowed regions: ");
        if (n > 0 && n < rem) { off += n; rem -= n; }
        region = ring->allowed_regions;
        displayed = 0;
        while (region && displayed < 3 && rem > 40) {
            n = snprintf(reason_buf + off, (size_t)rem,
                "[0x%016lX-0x%016lX] ",
                region->start, region->end);
            if (n > 0 && n < rem) { off += n; rem -= n; }
            region = region->next;
            displayed++;
        }
    }
    reason_buf[sizeof(reason_buf) - 1] = '\0';
    
    kernel_panic(reason_buf, NULL);
}

void KERNEL_INIT kproc_init(void) {
    kernel_procs = NULL;
    current_kproc = NULL;
    next_kproc_pid = KPROC_PID_BASE;
    kproc_initialized = 1;
}

int32_t kproc_create(const char *name, uint8_t vring_minor, kproc_entry_t entry, void *priv) {
    int32_t pid;
    uint64_t flags;
    kproc_t *proc;

    if (!kproc_initialized) return 0;
    proc = (kproc_t *)kmalloc(sizeof(kproc_t));
    if (!proc) return 0;
    memset(proc, 0, sizeof(kproc_t));

    flags = klog_irqsave();
    spin_lock(&kproc_lock);
    pid = next_kproc_pid;
    next_kproc_pid--;
    proc->pid = pid;
    proc->state = KPROC_STATE_RUNNING;
    proc->vring_minor = vring_minor;
    proc->name = name;
    proc->entry = entry;
    proc->private_data = priv;
    proc->next = kernel_procs;
    kernel_procs = proc;
    spin_unlock(&kproc_lock);
    klog_irqrestore(flags);
    
    return pid;
}

kproc_t *kproc_get(int32_t pid) {
    kproc_t *proc;

    if (pid >= 0) return NULL;
    proc = kernel_procs;
    while (proc) {
        if (proc->pid == pid && proc->state != KPROC_STATE_NONE) return proc;
        proc = proc->next;
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
    kproc_t *proc;
    kproc_msg_t *item;
    uint64_t flags;

    proc = kproc_get(pid);
    if (!proc) return -1;
    item = (kproc_msg_t *)kmalloc(sizeof(kproc_msg_t));
    if (!item) return -2;
    item->value = msg;
    item->next = NULL;

    flags = klog_irqsave();
    spin_lock(&kproc_lock);
    if (proc->msg_tail) proc->msg_tail->next = item;
    else proc->msg_head = item;
    proc->msg_tail = item;
    proc->msg_count++;
    spin_unlock(&kproc_lock);
    klog_irqrestore(flags);

    return 0;
}

int kproc_recv_msg(int32_t pid, uint64_t *msg) {
    kproc_t *proc;
    kproc_msg_t *item;
    uint64_t flags;

    proc = kproc_get(pid);
    if (!proc) return -1;
    if (!msg) return -1;

    flags = klog_irqsave();
    spin_lock(&kproc_lock);
    item = proc->msg_head;
    if (!item) {
        spin_unlock(&kproc_lock);
        klog_irqrestore(flags);
        return -2;
    }
    proc->msg_head = item->next;
    if (!proc->msg_head) proc->msg_tail = NULL;
    proc->msg_count--;
    spin_unlock(&kproc_lock);
    klog_irqrestore(flags);

    *msg = item->value;
    kfree(item);
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

void KERNEL_INIT kproc_print_init(void) {
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
                serial_write_nolock(buf + start, i - start);
            }
            in_esc = 1;
            start = i;
            continue;
        }
    }
    if (!in_esc && i > start) {
        serial_write_nolock(buf + start, i - start);
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
