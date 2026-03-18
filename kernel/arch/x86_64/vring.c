#include <kernel/vring.h>
#include <kernel/console.h>
#include <kernel/tty.h>
#include <kernel/mem_map.h>
#include <kernel/common.h>
#include <kernel/task.h>
#include <kernel/io.h>
#include <kernel/panic.h>
#include <kernel/spinlock.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

vring_t subrings[VRING_MAX_SUBRINGS];
kproc_t kernel_procs[KPROC_MAX];
kproc_t *current_kproc = NULL;
static int32_t next_kproc_pid = KPROC_PID_BASE;
static volatile int vring_initialized = 0;
static volatile int kproc_initialized = 0;

static volatile uint64_t print_queue_head = 0;
static volatile uint64_t print_queue_tail = 0;
static volatile uint64_t print_queue_count = 0;

#define KLOG_MAX_ITEMS 32
#define KLOG_MAX_LEN   128

typedef struct {
    uint16_t len;
    uint8_t level;
    char msg[KLOG_MAX_LEN];
} klog_item_t;

static klog_item_t *klog_ring;
static klog_item_t klog_ring_storage[KLOG_MAX_ITEMS];
static volatile uint64_t klog_head = 0;
static volatile uint64_t klog_tail = 0;
static volatile uint64_t klog_count = 0;
static volatile uint64_t klog_dropped = 0;

#define KPRINT_MAX_ITEMS 64
#define KPRINT_MAX_LEN   128

typedef struct {
    uint16_t len;
    uint8_t con_id;
    uint8_t reserved;
    char msg[KPRINT_MAX_LEN];
} kprint_item_t;

static kprint_item_t *kprint_ring;
static kprint_item_t kprint_ring_storage[KPRINT_MAX_ITEMS];
static volatile uint64_t kprint_head = 0;
static volatile uint64_t kprint_tail = 0;
static volatile uint64_t kprint_count = 0;
static volatile uint64_t kprint_dropped = 0;

static volatile int kprint_ready = 0;

static wait_queue_t kprint_waitq;

#define SERIAL_RING_SIZE 8192
static char *serial_ring;
static char serial_ring_storage[SERIAL_RING_SIZE];
static volatile uint64_t serial_head = 0;
static volatile uint64_t serial_tail = 0;
static volatile uint64_t serial_count = 0;
static spinlock_t serial_lock = { .locked = 0 };

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



static void serial_enqueue_nolock(const char *buf, size_t len) {
    size_t i;
    uint8_t ch;

    for (i = 0; i < len; i++) {
        ch = (uint8_t)buf[i];
        if (ch == '\n') {
            if (serial_count >= SERIAL_RING_SIZE) {
                while (!serial_thr_empty()) cpu_relax();
                outb(0x3F8, '\r');
            } else {
                serial_ring[serial_tail] = '\r';
                serial_tail = (serial_tail + 1) % SERIAL_RING_SIZE;
                serial_count++;
            }
        }
        if (serial_count >= SERIAL_RING_SIZE) {
            while (!serial_thr_empty()) cpu_relax();
            outb(0x3F8, ch);
            continue;
        }
        serial_ring[serial_tail] = ch;
        serial_tail = (serial_tail + 1) % SERIAL_RING_SIZE;
        serial_count++;
    }
}

static void serial_write_async(const char *buf, size_t len) {
    uint64_t flags;
    size_t i;
    uint8_t ch;

    if (!serial_ring) {
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
        serial_head = (serial_head + 1) % SERIAL_RING_SIZE;
        serial_count--;
        drained++;
    }
    spin_unlock(&serial_lock);
    klog_irqrestore(flags);
}

static int klog_enqueue(uint8_t level, const char *buf, uint64_t len) {
    uint64_t flags;
    klog_item_t *it;

    if (!buf || len == 0) return 0;
    if (len >= KLOG_MAX_LEN) len = KLOG_MAX_LEN - 1;

    serial_write(buf, len);

    if (!klog_ring) return (int)len;

    flags = klog_irqsave();
    if (klog_count >= KLOG_MAX_ITEMS) {
        klog_dropped++;
        klog_irqrestore(flags);
        return (int)len;
    }
    it = &klog_ring[klog_tail];
    it->level = level;
    it->len = (uint16_t)len;
    memcpy(it->msg, buf, len);
    it->msg[len] = '\0';
    klog_tail = (klog_tail + 1) % KLOG_MAX_ITEMS;
    klog_count++;
    klog_irqrestore(flags);
    waitq_wake_one(&kprint_waitq);
    return (int)len;
}

static int klog_dequeue(klog_item_t *out) {
    uint64_t flags;

    if (!out) return -1;
    if (!klog_ring) return -1;
    flags = klog_irqsave();
    if (klog_count == 0) {
        klog_irqrestore(flags);
        return -1;
    }
    *out = klog_ring[klog_head];
    klog_head = (klog_head + 1) % KLOG_MAX_ITEMS;
    klog_count--;
    klog_irqrestore(flags);
    return 0;
}

static int kprint_try_enqueue(uint8_t con_id, const char *buf, uint64_t len) {
    uint64_t flags;
    kprint_item_t *it;

    if (!buf || len == 0) return 0;
    if (len >= KPRINT_MAX_LEN) len = KPRINT_MAX_LEN - 1;

    if (con_id >= NUM_CONSOLES) con_id = 0;

    if (!kprint_ring) return -1;

    flags = klog_irqsave();
    if (kprint_count >= KPRINT_MAX_ITEMS) {
        kprint_dropped++;
        klog_irqrestore(flags);
        return -1;
    }
    it = &kprint_ring[kprint_tail];
    it->con_id = con_id;
    it->len = (uint16_t)len;
    memcpy(it->msg, buf, len);
    it->msg[len] = '\0';
    kprint_tail = (kprint_tail + 1) % KPRINT_MAX_ITEMS;
    kprint_count++;
    klog_irqrestore(flags);
    waitq_wake_one(&kprint_waitq);
    return (int)len;
}

static int kprint_dequeue(kprint_item_t *out) {
    uint64_t flags;

    if (!out) return -1;
    if (!kprint_ring) return -1;
    flags = klog_irqsave();
    if (kprint_count == 0) {
        klog_irqrestore(flags);
        return -1;
    }
    *out = kprint_ring[kprint_head];
    kprint_head = (kprint_head + 1) % KPRINT_MAX_ITEMS;
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
    int retries;
    
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

        retries = 0;
        while (kprint_try_enqueue((uint8_t)con_id, buf + off, chunk) < 0) {
            retries++;
            if (retries > 64) {
                console_write_to_fb_only(con_id, buf + off, chunk);
                break;
            }
            if (interrupts_enabled() && current_task) {
                yield();
            }
        }

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

    klog_ring = klog_ring_storage;
    kprint_ring = kprint_ring_storage;
    serial_ring = serial_ring_storage;

    memset(klog_ring, 0, sizeof(klog_ring_storage));
    memset(kprint_ring, 0, sizeof(kprint_ring_storage));
    memset(serial_ring, 0, sizeof(serial_ring_storage));
    klog_head = 0;
    klog_tail = 0;
    klog_count = 0;
    klog_dropped = 0;
    kprint_head = 0;
    kprint_tail = 0;
    kprint_count = 0;
    kprint_dropped = 0;
    serial_head = 0;
    serial_tail = 0;
    serial_count = 0;

    memset(subrings, 0, sizeof(subrings));
    for (i = 0; i < VRING_MAX_SUBRINGS; i++) {
        subrings[i].ring_major = 0;
        subrings[i].ring_minor = (uint8_t)i;
        subrings[i].active = false;
        subrings[i].region_count = 0;
        subrings[i].name = NULL;
    }
    
    vring_initialized = 1;
}

int vring_create(uint8_t minor, const char *name) {
    if (minor >= VRING_MAX_SUBRINGS) return -1;
    if (subrings[minor].active) return -2;
    
    subrings[minor].ring_major = 0;
    subrings[minor].ring_minor = minor;
    subrings[minor].active = true;
    subrings[minor].region_count = 0;
    subrings[minor].name = name;
    
    return 0;
}

int vring_add_region(uint8_t minor, uint64_t start, uint64_t end, uint8_t perms) {
    uint64_t idx;

    if (minor >= VRING_MAX_SUBRINGS) return -1;
    if (!subrings[minor].active) return -2;
    if (subrings[minor].region_count >= VRING_MAX_REGIONS) return -3;
    
    idx = subrings[minor].region_count;
    subrings[minor].allowed_regions[idx].start = start;
    subrings[minor].allowed_regions[idx].end = end;
    subrings[minor].allowed_regions[idx].permissions = perms;
    subrings[minor].region_count++;
    
    return 0;
}

int vring_remove(uint8_t minor) {
    if (minor >= VRING_MAX_SUBRINGS) return -1;
    if (!subrings[minor].active) return -2;
    
    subrings[minor].active = false;
    subrings[minor].region_count = 0;
    subrings[minor].name = NULL;
    
    return 0;
}

vring_t *vring_get(uint8_t minor) {
    if (minor >= VRING_MAX_SUBRINGS) return NULL;
    if (!subrings[minor].active) return NULL;
    return &subrings[minor];
}

bool vring_check_access(uint8_t minor, uint64_t addr, uint64_t size, uint8_t access_type) {
    vring_t *ring;
    uint64_t end_addr;
    uint64_t i;

    if (!vring_initialized) return true;
    if (minor == 0) return true;
    if (minor >= VRING_MAX_SUBRINGS) return false;
    if (!subrings[minor].active) return false;
    
    ring = &subrings[minor];
    end_addr = addr + size;
    
    for (i = 0; i < ring->region_count; i++) {
        vring_mem_region_t *region = &ring->allowed_regions[i];
        
        if (addr >= region->start && end_addr <= region->end) {
            if ((region->permissions & access_type) == access_type) {
                return true;
            }
        }
    }
    
    return false;
}

void vring_panic_forbidden(uint8_t minor, uint64_t addr, uint8_t access_type) {
    char reason_buf[256];
    char *p = reason_buf;
    int i;
    vring_t *ring;
    
    p += sprintf(p, "Virtual Ring Violation - Ring 0.");
    p += sprintf(p, "%d", minor);
    
    if (subrings[minor].name) {
        p += sprintf(p, " (%s)", subrings[minor].name);
    }
    
    p += sprintf(p, " forbidden access at 0x%016lX ", addr);
    
    if (access_type & VRING_PERM_READ) p += sprintf(p, "READ ");
    if (access_type & VRING_PERM_WRITE) p += sprintf(p, "WRITE ");
    if (access_type & VRING_PERM_EXEC) p += sprintf(p, "EXEC ");
    
    ring = vring_get(minor);
    if (ring) {
        p += sprintf(p, "\nAllowed regions: ");
        for (i = 0; (uint64_t)i < ring->region_count && i < 3; i++) {
            p += sprintf(p, "[0x%016lX-0x%016lX] ", 
                ring->allowed_regions[i].start,
                ring->allowed_regions[i].end);
        }
    }
    
    kernel_panic(reason_buf, NULL);
}

void kproc_init(void) {
    int i;

    memset(kernel_procs, 0, sizeof(kernel_procs));
    for (i = 0; i < KPROC_MAX; i++) {
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
    current_kproc = NULL;
    next_kproc_pid = KPROC_PID_BASE;
    kproc_initialized = 1;
}

int32_t kproc_create(const char *name, uint8_t vring_minor, kproc_entry_t entry, void *priv) {
    int slot;
    int i;
    int32_t pid;

    if (!kproc_initialized) return 0;
    
    slot = -1;
    for (i = 0; i < KPROC_MAX; i++) {
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
    
    for (i = 0; i < KPROC_MAX; i++) {
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
                if (!console_alt_screen_active(0))
                    console_write_to_fb_only(0, kit.msg, (size_t)kit.len);
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

bool kprint_is_ready(void) {
    return kprint_ready != 0;
}

void kprint_enable(void) {
    kprint_ready = 1;
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
