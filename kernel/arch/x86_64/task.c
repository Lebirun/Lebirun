#include <lebirun/task.h>
#include <lebirun/pipe.h>
#include <lebirun/debug.h>
#include <lebirun/registers.h>
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <lebirun/gdt.h>
#include <lebirun/tty.h>
#include <lebirun/elf.h>
#include <lebirun/console.h>
#include <lebirun/creds.h>
#include <lebirun/vring.h>
#include <lebirun/kstack.h>
#include <lebirun/spinlock.h>
#include <lebirun/smp.h>
#include <lebirun/vfs.h>
#include <lebirun/overlayfs.h>
#include <lebirun/rng.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

extern void temp_map_raw(uint64_t temp_virt, uint64_t phys_addr);
extern void temp_unmap_raw(uint64_t temp_virt);

#define TASK_FILE_FAULT_TEMP TEMP_SLOT(5)

#define KERR_EPERM   1
#define KERR_EIO     5
#define KERR_ENOEXEC 8
#define KERR_EINTR   4
#define KERR_ENOMEM  12

#define TASK_STACK_SIZE 4096
#define SCHED_DEFAULT_TIMESLICE 3
#define KERNEL_STACK_SIZE 8192

#define USER_STACK_TOP 0x00800000u
#define USER_STACK_SIZE 0x10000u
#define USER_STACK_INITIAL_MIN 0x1000u
#define USER_STACK_GAP  0x1000u
#define USER_STACK_INIT_ESP (USER_STACK_TOP - USER_STACK_GAP - 16u)
#define USER_MMAP_BASE 0x10000000u
#define USER_MMAP_LIMIT 0x40000000u
#define FPU_STATE_SIZE 512

static spinlock_t sched_lock = {0};
int scheduler_initialized = 0;
extern volatile uint64_t tick_count;
extern uint64_t kernel_irq_cr3;

extern void switch_to_asm(uint64_t* old_rsp, uint64_t new_rsp);

task_t* current_task = NULL;
task_t* ready_queue_head = NULL;
task_t* all_tasks_head = NULL;
static task_t* sleep_queue_head = NULL;
static task_t* dead_queue_head = NULL;
static int task_ptr_valid(task_t *t);
static void task_release_exit_resources(task_t *t);
static int task_is_current_on_any_cpu(task_t *task);

int task_set_cwd(task_t *task, const char *cwd) {
    char *copy;
    size_t len;

    if (!task || !cwd) return -1;
    len = strlen(cwd);
    if (len >= VFS_MAX_PATH) return -1;
    copy = (char *)kmalloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, cwd, len + 1);
    if (task->cwd) kfree(task->cwd);
    task->cwd = copy;
    return 0;
}

int task_copy_cwd(task_t *task, const task_t *source) {
    if (!source || !source->cwd) return task_set_cwd(task, "/");
    return task_set_cwd(task, source->cwd);
}

static void task_free_cwd(task_t *task) {
    if (!task || !task->cwd) return;
    kfree(task->cwd);
    task->cwd = NULL;
}

static volatile int reap_pending = 0;
static volatile int exec_drain_pending = 0;

static uint64_t next_task_id = 1;
static pid_t next_kernel_pid = -1;
static uint8_t default_fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));
static int fpu_state_ready = 0;

typedef struct exec_page_cache_entry {
    vfs_node_t *node;
    uint64_t offset;
    uint64_t phys;
    uint64_t last_access;
    struct exec_page_cache_entry *next;
} exec_page_cache_entry_t;

static exec_page_cache_entry_t *exec_page_cache_head = NULL;
static spinlock_t exec_page_cache_lock = {0};

static uint64_t task_align_up(uint64_t v, uint64_t align) {
    if (align == 0) return v;
    return (v + align - 1u) & ~(align - 1u);
}

static uint64_t task_initial_stack_size(int argc, char **argv, int envc, char **envp, const char *fallback_name, int aux_entries) {
    uint64_t bytes;
    uint64_t size;
    int i;
    int len;
    const char *s;

    bytes = 16;
    bytes += 16;
    bytes += (uint64_t)aux_entries * 16;
    bytes += 8;
    bytes += 8;
    bytes += ((uint64_t)envc + 1u) * 8u;
    bytes += ((uint64_t)argc + 1u) * 8u;
    bytes += 8;

    if (argc > 0 && argv) {
        for (i = 0; i < argc; i++) {
            len = 0;
            if (argv[i]) {
                while (argv[i][len]) len++;
            }
            bytes += task_align_up((uint64_t)len + 1u, 8);
        }
    } else {
        s = fallback_name ? fallback_name : "program";
        len = 0;
        while (s[len]) len++;
        bytes += task_align_up((uint64_t)len + 1u, 8);
    }

    if (envc > 0 && envp) {
        for (i = 0; i < envc; i++) {
            len = 0;
            if (envp[i]) {
                while (envp[i][len]) len++;
            }
            bytes += task_align_up((uint64_t)len + 1u, 8);
        }
    }

    size = task_align_up(bytes, PAGE_SIZE);
    if (size < USER_STACK_INITIAL_MIN) size = USER_STACK_INITIAL_MIN;
    return size;
}

static inline void fpu_save_area(uint8_t *area) {
    if (!area) return;
    __asm__ volatile ("fxsave64 %0" : "=m"(*(uint8_t (*)[FPU_STATE_SIZE])area) : : "memory");
}

static inline void fpu_restore_area(uint8_t *area) {
    uint8_t *state;

    state = area ? area : default_fpu_state;
    __asm__ volatile ("fxrstor64 %0" : : "m"(*(uint8_t (*)[FPU_STATE_SIZE])state) : "memory");
}

static void fpu_init_default_state(void) {
    if (fpu_state_ready) return;
    __asm__ volatile ("fninit" ::: "memory");
    fpu_save_area(default_fpu_state);
    fpu_state_ready = 1;
}

static int task_init_fpu_state(task_t *task) {
    uint8_t *state;

    if (!task) return -1;
    fpu_init_default_state();
    state = (uint8_t *)kmalloc_aligned(FPU_STATE_SIZE, 16);
    if (!state) return -1;
    memcpy(state, default_fpu_state, FPU_STATE_SIZE);
    task->fpu_state = state;
    return 0;
}

static int task_ensure_fpu_state(task_t *task) {
    if (!task) return -1;
    if (task->fpu_state) return 0;
    return task_init_fpu_state(task);
}

static void task_reset_fpu_state(task_t *task) {
    if (task_ensure_fpu_state(task) != 0) return;
    memcpy(task->fpu_state, default_fpu_state, FPU_STATE_SIZE);
}

static void task_free_fpu_state(task_t *task) {
    if (!task || !task->fpu_state) return;
    kfree_aligned(task->fpu_state);
    task->fpu_state = NULL;
}

static void task_write_fs_base(uint64_t base) {
    __asm__ volatile (
        "wrmsr"
        :
        : "c"(0xC0000100u),
          "a"((uint32_t)(base & 0xFFFFFFFF)),
          "d"((uint32_t)(base >> 32))
        : "memory"
    );
}
static uint64_t exec_page_cache_pages = 0;
static uint64_t exec_page_cache_clock = 0;

#define EXEC_CLEANUP_QUEUE_INIT 4
#define EXEC_CLEANUP_QUEUE_MAX 64

typedef struct {
    uint64_t old_pd;
    uint64_t *old_pages;
    uint64_t old_pages_count;
} exec_cleanup_entry_t;

static exec_cleanup_entry_t *exec_cleanup_queue;
static int exec_cleanup_capacity = 0;
static volatile int exec_cleanup_head = 0;
static volatile int exec_cleanup_tail = 0;
static volatile int exec_cleanup_lock = 0;

static inline void exec_cleanup_lock_acquire(void) {
    while (__sync_lock_test_and_set(&exec_cleanup_lock, 1)) {
        __asm__ volatile ("pause" ::: "memory");
    }
}

static inline void exec_cleanup_lock_release(void) {
    __sync_lock_release(&exec_cleanup_lock);
}

static int exec_cleanup_ensure_queue(void) {
    exec_cleanup_entry_t *queue;
    int ok;

    if (exec_cleanup_queue) return 1;
    queue = (exec_cleanup_entry_t *)kmalloc(EXEC_CLEANUP_QUEUE_INIT * sizeof(exec_cleanup_entry_t));
    if (!queue) return 0;
    memset(queue, 0, EXEC_CLEANUP_QUEUE_INIT * sizeof(exec_cleanup_entry_t));
    ok = 1;
    exec_cleanup_lock_acquire();
    if (!exec_cleanup_queue) {
        exec_cleanup_queue = queue;
        exec_cleanup_capacity = EXEC_CLEANUP_QUEUE_INIT;
        exec_cleanup_head = 0;
        exec_cleanup_tail = 0;
        queue = NULL;
    }
    exec_cleanup_lock_release();
    if (queue) kfree(queue);
    return ok;
}

static int exec_cleanup_grow_queue(void) {
    exec_cleanup_entry_t *new_queue;
    exec_cleanup_entry_t *old_queue;
    int old_capacity;
    int new_capacity;
    int count;
    int idx;
    int i;

    if (!exec_cleanup_queue) return exec_cleanup_ensure_queue();

    exec_cleanup_lock_acquire();
    old_capacity = exec_cleanup_capacity;
    if (old_capacity >= EXEC_CLEANUP_QUEUE_MAX) {
        exec_cleanup_lock_release();
        return 0;
    }
    exec_cleanup_lock_release();

    new_capacity = old_capacity * 2;
    if (new_capacity > EXEC_CLEANUP_QUEUE_MAX) new_capacity = EXEC_CLEANUP_QUEUE_MAX;
    new_queue = (exec_cleanup_entry_t *)kmalloc(new_capacity * sizeof(exec_cleanup_entry_t));
    if (!new_queue) return 0;
    memset(new_queue, 0, new_capacity * sizeof(exec_cleanup_entry_t));

    exec_cleanup_lock_acquire();
    if (exec_cleanup_capacity >= new_capacity) {
        exec_cleanup_lock_release();
        kfree(new_queue);
        return 1;
    }
    count = 0;
    idx = exec_cleanup_head;
    while (idx != exec_cleanup_tail && count < new_capacity - 1) {
        new_queue[count] = exec_cleanup_queue[idx];
        idx = (idx + 1) % exec_cleanup_capacity;
        count++;
    }
    old_queue = exec_cleanup_queue;
    exec_cleanup_queue = new_queue;
    exec_cleanup_capacity = new_capacity;
    exec_cleanup_head = 0;
    exec_cleanup_tail = count;
    for (i = count; i < new_capacity; i++) {
        memset(&exec_cleanup_queue[i], 0, sizeof(exec_cleanup_entry_t));
    }
    exec_cleanup_lock_release();

    kfree(old_queue);
    return 1;
}

static int task_is_current_on_any_cpu(task_t *task) {
    int i;

    if (!task) return 0;
    for (i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        if (!cpus[i].active) continue;
        if (cpus[i].current_task == task) return 1;
    }
    if (current_task == task) return 1;
    return 0;
}

static int task_refs_pml4(task_t *t, uint64_t pd_page, int include_exec_old) {
    registers_t *frame;

    if (!t || t->resources_released) return 0;
    frame = t->syscall_frame;
    if ((t->pml4_phys & ~0xFFFULL) == pd_page ||
        (t->cr3 & ~0xFFFULL) == pd_page ||
        (include_exec_old && (t->exec_old_pml4 & ~0xFFFULL) == pd_page) ||
        (t->regs.entry_cr3 & ~0xFFFULL) == pd_page ||
        (t->regs.return_cr3 & ~0xFFFULL) == pd_page ||
        (t->regs.saved_entry_cr3 & ~0xFFFULL) == pd_page ||
        (frame && ((frame->entry_cr3 & ~0xFFFULL) == pd_page ||
                   (frame->return_cr3 & ~0xFFFULL) == pd_page ||
                   (frame->saved_entry_cr3 & ~0xFFFULL) == pd_page))) {
        return 1;
    }
    return 0;
}

static int task_pml4_has_owner(task_t *owner, uint64_t pd, int check_owner, int include_owner_exec_old) {
    task_t *t;
    uint64_t pd_page;
    int limit;
    int found;

    if (!pd) return 0;
    pd_page = pd & ~0xFFFULL;
    found = 0;
    lock_scheduler();
    t = all_tasks_head;
    limit = 1024;
    while (t && limit > 0) {
        if (!task_ptr_valid(t)) break;
        if (t == owner) {
            if (check_owner && task_refs_pml4(t, pd_page, include_owner_exec_old)) {
                found = 1;
                break;
            }
        } else {
            if (task_refs_pml4(t, pd_page, 1)) {
                found = 1;
                break;
            }
        }
        t = t->all_next;
        limit--;
    }
    unlock_scheduler();
    return found;
}

static int task_pml4_has_other_owner(task_t *owner, uint64_t pd) {
    return task_pml4_has_owner(owner, pd, 0, 0);
}

static int task_free_pml4_if_unowned(task_t *owner, uint64_t pd) {
    if (!pd) return 1;
    if (task_is_current_on_any_cpu(owner)) return 0;
    if ((read_cr3() & ~0xFFFULL) == (pd & ~0xFFFULL)) return 0;
    if (task_pml4_has_other_owner(owner, pd)) return 0;
    vmm_free_pml4(pd);
    return 1;
}

static int task_free_exec_old_pml4_if_unowned(task_t *owner, uint64_t pd) {
    if (!pd) return 1;
    if (task_is_current_on_any_cpu(owner)) return 0;
    if ((read_cr3() & ~0xFFFULL) == (pd & ~0xFFFULL)) return 0;
    if (task_pml4_has_owner(owner, pd, 1, 0)) return 0;
    vmm_free_pml4(pd);
    return 1;
}

void exec_cleanup_enqueue(uint64_t pd, uint64_t *pages, uint64_t count) {
    int next_tail;
    int inserted;

    if (!exec_cleanup_ensure_queue()) {
        if (pages) {
            kfree(pages);
        }
        task_free_pml4_if_unowned(NULL, pd);
        return;
    }

    inserted = 0;
    while (!inserted) {
        exec_cleanup_lock_acquire();
        next_tail = (exec_cleanup_tail + 1) % exec_cleanup_capacity;
        if (next_tail != exec_cleanup_head) {
            exec_cleanup_queue[exec_cleanup_tail].old_pd = pd;
            exec_cleanup_queue[exec_cleanup_tail].old_pages = pages;
            exec_cleanup_queue[exec_cleanup_tail].old_pages_count = count;
            exec_cleanup_tail = next_tail;
            inserted = 1;
        }
        exec_cleanup_lock_release();
        if (!inserted && !exec_cleanup_grow_queue()) {
            if (pages) {
                kfree(pages);
            }
            task_free_pml4_if_unowned(NULL, pd);
            return;
        }
    }
}

void exec_cleanup_drain(void) {
    uint64_t *pages;
    uint64_t pd;
    int head;
    int freed;

    if (!exec_cleanup_queue) return;

    exec_cleanup_lock_acquire();
    while (exec_cleanup_head != exec_cleanup_tail) {
        head = exec_cleanup_head;
        pages = exec_cleanup_queue[head].old_pages;
        pd = exec_cleanup_queue[head].old_pd;

        exec_cleanup_queue[head].old_pd = 0;
        exec_cleanup_queue[head].old_pages = NULL;
        exec_cleanup_queue[head].old_pages_count = 0;
        exec_cleanup_head = (head + 1) % exec_cleanup_capacity;
        exec_cleanup_lock_release();

        if (pages) {
            kfree(pages);
        }
        freed = task_free_pml4_if_unowned(NULL, pd);
        if (!freed) {
            exec_cleanup_enqueue(pd, NULL, 0);
            return;
        }

        exec_cleanup_lock_acquire();
    }
    exec_cleanup_lock_release();
}

static void task_discard_exec_old(task_t *t) {
    uint64_t pd;
    uint64_t *pages;
    uint64_t count;

    if (!t || !t->exec_old_pml4) return;
    pd = t->exec_old_pml4;
    pages = t->exec_old_pages;
    count = t->exec_old_pages_count;
    t->exec_old_pml4 = 0;
    t->exec_old_pages = NULL;
    t->exec_old_pages_count = 0;
    exec_cleanup_enqueue(pd, pages, count);
}

static uint64_t task_stack_pages(task_t *task) {
    if (!task || task->stack_size == 0) return 0;
    return (task->stack_size + PAGE_SIZE - 1) / PAGE_SIZE;
}

static uint64_t task_file_pages(task_t *task) {
    uint64_t pages;
    int i;

    if (!task || !task->pml4_phys) return 0;
    pages = 0;
    for (i = 0; i < task->file_map_count; i++) {
        if (!task->file_maps[i].node) continue;
        pages += vmm_count_present_pages_in_range(task->pml4_phys,
                                                  task->file_maps[i].vaddr,
                                                  task->file_maps[i].vaddr + task->file_maps[i].memsz);
    }
    return pages;
}

static uint64_t task_heap_pages(task_t *task) {
    if (!task || !task->pml4_phys) return 0;
    if (task->user_brk <= task->user_brk_start) return 0;
    return vmm_count_present_pages_in_range(task->pml4_phys, task->user_brk_start, task->user_brk);
}

static uint64_t task_mmap_pages(task_t *task) {
    uint64_t pages;
    uint64_t file_mmap_pages;
    int i;

    if (!task || !task->pml4_phys) return 0;
    pages = vmm_count_present_pages_in_range(task->pml4_phys, USER_MMAP_BASE, USER_MMAP_LIMIT);
    file_mmap_pages = 0;
    for (i = 0; i < task->file_map_count; i++) {
        if (!task->file_maps[i].node) continue;
        if (task->file_maps[i].vaddr < USER_MMAP_BASE) continue;
        if (task->file_maps[i].vaddr >= USER_MMAP_LIMIT) continue;
        file_mmap_pages += vmm_count_present_pages_in_range(task->pml4_phys,
                                                            task->file_maps[i].vaddr,
                                                            task->file_maps[i].vaddr + task->file_maps[i].memsz);
    }
    if (pages >= file_mmap_pages) return pages - file_mmap_pages;
    return 0;
}

static void exec_cleanup_stats(uint64_t *entries, uint64_t *pages) {
    int head;
    uint64_t e;
    uint64_t p;

    e = 0;
    p = 0;
    if (!exec_cleanup_queue) {
        if (entries) *entries = 0;
        if (pages) *pages = 0;
        return;
    }
    exec_cleanup_lock_acquire();
    head = exec_cleanup_head;
    while (head != exec_cleanup_tail) {
        e++;
        p += exec_cleanup_queue[head].old_pages_count;
        head = (head + 1) % exec_cleanup_capacity;
    }
    exec_cleanup_lock_release();
    if (entries) *entries = e;
    if (pages) *pages = p;
}

static void task_error(const char *fmt, ...) {
    char buf[256];
    int n;
    va_list ap;
    
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s", buf);
    if (current_task && current_task->console_id >= 0 && console_is_initialized()) {
        console_write_to(current_task->console_id, buf, (size_t)n);
    }
}

static void sleepq_remove(task_t* t) {
    task_t **ptr;
    task_t *cur;

    if (!t) return;
    ptr = &sleep_queue_head;
    while (*ptr) {
        cur = *ptr;
        if (!task_ptr_valid(cur)) {
            *ptr = NULL;
            return;
        }
        if (cur == t) {
            *ptr = t->sleep_next;
            t->sleep_next = NULL;
            t->in_sleep_queue = 0;
            return;
        }
        ptr = &cur->sleep_next;
    }
}

pid_t getpid(void) {
    return current_task ? (pid_t)current_task->pid : 0;
}

static int task_ptr_valid(task_t *t) {
    uint64_t a;

    if (!t) return 0;
    a = (uint64_t)t;
    if ((a & 0xFFFF0000u) == 0xFEFE0000u) return 0;
    if (a < KERNEL_VMA) return 0;
    return 1;
}

task_t* task_find(pid_t pid) {
    int limit;
    task_t *t;

    lock_scheduler();
    t = all_tasks_head;
    limit = 1024;
    while (t && limit > 0) {
        if (!task_ptr_valid(t)) break;
        if (t->pid == pid) {
            unlock_scheduler();
            return t;
        }
        t = t->all_next;
        limit--;
    }
    unlock_scheduler();
    return NULL;
}

task_t* task_find_by_pml4(uint64_t pml4_phys) {
    task_t *t;
    int limit;

    if (!pml4_phys) return NULL;
    t = all_tasks_head;
    limit = 1024;
    while (t && limit > 0) {
        if (!task_ptr_valid(t)) break;
        if (t->pml4_phys == pml4_phys) {
            return t;
        }
        t = t->all_next;
        limit--;
    }
    return NULL;
}

static void task_wake_parent_waiter_locked(task_t *task) {
    task_t *parent;
    int limit;

    if (!task || task->ppid <= 0) return;
    parent = all_tasks_head;
    limit = 4096;
    while (parent && limit > 0) {
        if (!task_ptr_valid(parent)) break;
        if (parent->pid == task->ppid) {
            if (parent->waiting_for_any_child && parent->state == TASK_BLOCKED) {
                parent->state = TASK_READY;
            }
            return;
        }
        parent = parent->all_next;
        limit--;
    }
}

static void task_finish_death_locked(task_t *task) {
    waitq_wake_all(&task->join_waiters);
    task_wake_parent_waiter_locked(task);
}

int task_has_child_of(pid_t parent_pid, pid_t pgid_filter) {
    int found;
    int limit;
    task_t *t;
    found = 0;

    lock_scheduler();

    t = all_tasks_head;
    limit = 1024;
    while (t && limit > 0) {
        if (!task_ptr_valid(t)) break;
        if (t->ppid == parent_pid) {
            if (pgid_filter <= 0 || t->pgid == pgid_filter) {
                found = 1;
                break;
            }
        }
        t = t->all_next;
        limit--;
    }

    unlock_scheduler();
    return found;
}

task_t* task_find_dead_child_of(pid_t parent_pid, pid_t pgid_filter) {
    task_t *d;
    
    lock_scheduler();
    d = dead_queue_head;
    while (d && task_ptr_valid(d)) {
        if (d->ppid == parent_pid) {
            if (pgid_filter <= 0 || d->pgid == pgid_filter) {
                unlock_scheduler();
                return d;
            }
        }
        d = d->wait_next;
    }
    unlock_scheduler();
    return NULL;
}

int task_prepare_wait_any_child(pid_t parent_pid, pid_t pgid_filter) {
    task_t *t;
    task_t *d;
    int found;
    int limit;

    if (!current_task) return -1;
    lock_scheduler();

    d = dead_queue_head;
    while (d && task_ptr_valid(d)) {
        if (d->ppid == parent_pid) {
            if (pgid_filter <= 0 || d->pgid == pgid_filter) {
                unlock_scheduler();
                return 1;
            }
        }
        d = d->wait_next;
    }

    found = 0;
    t = all_tasks_head;
    limit = 1024;
    while (t && limit > 0) {
        if (!task_ptr_valid(t)) break;
        if (t->ppid == parent_pid) {
            if (pgid_filter <= 0 || t->pgid == pgid_filter) {
                found = 1;
                break;
            }
        }
        t = t->all_next;
        limit--;
    }

    if (!found) {
        current_task->join_target = NULL;
        unlock_scheduler();
        return -1;
    }

    current_task->join_target = t;
    current_task->waiting_for_any_child = 1;
    current_task->state = TASK_BLOCKED;
    unlock_scheduler();
    return 0;
}

void task_finish_wait_any_child(void) {
    lock_scheduler();
    if (current_task) {
        current_task->waiting_for_any_child = 0;
        current_task->join_target = NULL;
    }
    unlock_scheduler();
}

void waitq_init(wait_queue_t* q) {
    if (!q) return;
    q->head = NULL;
}

void waitq_add(wait_queue_t* q, task_t* t) {
    uint64_t flags;

    if (!q || !t) return;
    if (t->in_wait_queue || t->waiting_queue) return;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));
    t->wait_next = q->head;
    q->head = t;
    t->in_wait_queue = 1;
    t->waiting_queue = q;
    __asm__ volatile("push %0; popf" : : "r"(flags));
}

task_t* waitq_pop(wait_queue_t* q) {
    task_t *t;

    if (!q || !q->head) return NULL;
    t = q->head;
    while (t) {
        if (!task_ptr_valid(t)) {
            q->head = NULL;
            return NULL;
        }
        if (t->state != TASK_DEAD) break;
        q->head = t->wait_next;
        t->wait_next = NULL;
        t->in_wait_queue = 0;
        t->waiting_queue = NULL;
        t = q->head;
    }
    if (!t || !task_ptr_valid(t)) return NULL;
    q->head = t->wait_next;
    t->wait_next = NULL;
    t->in_wait_queue = 0;
    t->waiting_queue = NULL;
    return t;
}

void waitq_wake_all(wait_queue_t* q) {
    uint64_t flags;
    task_t *t;

    if (!q) return;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));
    while ((t = waitq_pop(q))) {
        if (t->state == TASK_BLOCKED) t->state = TASK_READY;
    }
    __asm__ volatile("push %0; popf" : : "r"(flags));
}

void waitq_wake_one(wait_queue_t *q) {
    uint64_t flags;
    task_t *t;

    if (!q) return;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));
    t = waitq_pop(q);
    if (t && t->state == TASK_BLOCKED) t->state = TASK_READY;
    __asm__ volatile("push %0; popf" : : "r"(flags));
}

void waitq_wait(wait_queue_t *q) {
    uint64_t flags;
    if (!q || !current_task) return;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));
    waitq_add(q, current_task);
    current_task->state = TASK_BLOCKED;
    __asm__ volatile("push %0; popf" : : "r"(flags));
    yield();
}

void waitq_remove(wait_queue_t* q, task_t* t) {
    task_t *prev;
    task_t *cur;

    if (!q || !t || !q->head) return;
    if (!task_ptr_valid(q->head)) {
        q->head = NULL;
        return;
    }
    if (q->head == t) {
        q->head = t->wait_next;
        t->wait_next = NULL;
        t->in_wait_queue = 0;
        t->waiting_queue = NULL;
        return;
    }
    prev = q->head;
    cur = prev->wait_next;
    while (cur) {
        if (!task_ptr_valid(cur)) {
            prev->wait_next = NULL;
            return;
        }
        if (cur == t) {
            prev->wait_next = cur->wait_next;
            cur->wait_next = NULL;
            cur->in_wait_queue = 0;
            cur->waiting_queue = NULL;
            return;
        }
        prev = cur;
        cur = cur->wait_next;
    }
}

void init_tasks(void) {
    uint8_t *task0_kstack;
    uint64_t boot_rsp = 0;

    current_task = (task_t*)kmalloc(sizeof(task_t));
    memset(current_task, 0, sizeof(task_t));
    fpu_init_default_state();
    current_task->id = 0;
    current_task->pid = 0;
    strcpy(current_task->name, "idle");
    current_task->state = TASK_RUNNING;
    current_task->cr3 = read_cr3();
    current_task->next = current_task;
    current_task->time_slice = SCHED_DEFAULT_TIMESLICE;
    current_task->base_time_slice = SCHED_DEFAULT_TIMESLICE;
    current_task->stack_base = NULL;
    current_task->stack_size = 0;
    
    task0_kstack = kstack_alloc();
    if (task0_kstack) {
        current_task->kernel_stack_base = task0_kstack;
        current_task->kernel_stack_size = KSTACK_USABLE_SIZE;
    } else {
        current_task->kernel_stack_base = kmalloc(KERNEL_STACK_SIZE);
        if (current_task->kernel_stack_base) {
            current_task->kernel_stack_size = KERNEL_STACK_SIZE;
        } else {
            current_task->kernel_stack_size = 0;
        }
    }
    
    current_task->wake_tick = 0;
    current_task->sleep_next = NULL;
    current_task->in_sleep_queue = 0;
    current_task->in_wait_queue = 0;
    current_task->wait_next = NULL;
    current_task->waiting_queue = NULL;
    current_task->join_target = NULL;
    waitq_init(&current_task->join_waiters);
    current_task->join_refs = 0;
    current_task->exit_code = 0;
    current_task->syscall_frame = NULL;
    asm volatile ("mov %%rsp, %0" : "=r" (boot_rsp));
    current_task->regs.rsp = boot_rsp;
    
    if (current_task->kernel_stack_base) {
        tss_set_rsp0((uint64_t)current_task->kernel_stack_base + current_task->kernel_stack_size);
    } else {
        tss_set_rsp0(boot_rsp);
    }

    current_task->regs.rip = 0;
    current_task->regs.rflags = 0x202;
    current_task->regs.cs = 0x08;
    current_task->regs.ds = current_task->regs.es = 0x10;
    current_task->vring_minor = 0;
    current_task->is_kernel_task = false;
    ready_queue_head = current_task;
    all_tasks_head = current_task;
    current_task->all_next = NULL;

    {
        cpu_info_t *bsp;
        bsp = smp_this_cpu();
        bsp->current_task = current_task;
        bsp->scheduler_lock_depth = 0;
        bsp->sched_saved_rflags = 0;
        bsp->schedule_force = 0;
    }
    
    scheduler_initialized = 1;
}

void lock_scheduler(void) {
    uint64_t eflags;
    cpu_info_t *cpu;

    __asm__ __volatile__ ("pushf; pop %0; cli" : "=r"(eflags) :: "memory");
    cpu = smp_this_cpu();
    if (cpu->scheduler_lock_depth == 0) {
        spin_lock(&sched_lock);
        cpu->sched_saved_rflags = eflags;
    }
    cpu->scheduler_lock_depth++;
}

void unlock_scheduler(void) {
    uint64_t eflags;
    cpu_info_t *cpu;

    cpu = smp_this_cpu();
    cpu->scheduler_lock_depth--;
    if (cpu->scheduler_lock_depth == 0) {
        eflags = cpu->sched_saved_rflags;
        spin_unlock(&sched_lock);
        if (eflags & (1 << 9)) {
            __asm__ __volatile__ ("sti" ::: "memory");
        }
    }
}

void add_task_to_runqueue(task_t* new_task) {
    task_t *tail;

    if (!ready_queue_head) {
        ready_queue_head = new_task;
        new_task->next = new_task;
    } else {
        tail = ready_queue_head;
        while (tail->next != ready_queue_head) tail = tail->next;
        tail->next = new_task;
        new_task->next = ready_queue_head;
    }
}

static inline void remove_task_from_runqueue(task_t* task) {
    task_t *prev;

    if (!ready_queue_head || !task) return;
    
    if (task->next == task) {
        if (task == ready_queue_head) {
            ready_queue_head = NULL;
        }
        return;
    }
    
    prev = ready_queue_head;
    while (prev->next != task) {
        prev = prev->next;
        if (prev == ready_queue_head) {
            return;
        }
    }
    
    prev->next = task->next;
    
    if (task == ready_queue_head) {
        ready_queue_head = task->next;
    }
}

static int task_in_runqueue_locked(task_t *task) {
    task_t *t;
    int limit;

    if (!task || !ready_queue_head) return 0;
    t = ready_queue_head;
    limit = 0;
    do {
        if (t == task) return 1;
        t = t->next;
        limit++;
    } while (t && t != ready_queue_head && limit < 10000);
    return 0;
}

static task_t *task_find_idle_locked(void) {
    task_t *t;
    int limit;

    t = all_tasks_head;
    limit = 0;
    while (t && limit < 4096) {
        if (!task_ptr_valid(t)) break;
        if (t->id == 0 && !t->is_user && !t->resources_released) return t;
        t = t->all_next;
        limit++;
    }
    return NULL;
}

task_t* create_task(void (*entry)(void), task_state_t initial_state, bool user_mode) {
    return create_task_with_cr3(entry, initial_state, user_mode, read_cr3());
}

task_t* create_task_with_cr3(void (*entry)(void), task_state_t initial_state, bool user_mode, uint64_t cr3) {
    uint8_t *stack_base;
    uint8_t *kernel_stack_base;
    uint64_t *krsp;
    uint64_t *rsp_ptr;
    uint64_t user_rsp;
    task_t *new_task;

    if (!entry) return NULL;

    new_task = (task_t*)kmalloc(sizeof(task_t));
    stack_base = user_mode ? NULL : (uint8_t*)kmalloc(TASK_STACK_SIZE);
    kernel_stack_base = kstack_alloc();
    if (!new_task || (!user_mode && !stack_base) || !kernel_stack_base) {
        printf("Task alloc fail!\n");
        if (new_task) kfree(new_task);
        if (stack_base) kfree(stack_base);
        if (kernel_stack_base) kstack_free(kernel_stack_base);
        return NULL;
    }
    memset(new_task, 0, sizeof(task_t));
    if (user_mode && task_set_cwd(new_task, "/") != 0) {
        kfree(new_task);
        if (stack_base) kfree(stack_base);
        kstack_free(kernel_stack_base);
        return NULL;
    }
    if (user_mode && task_init_fpu_state(new_task) != 0) {
        printf("Task fpu alloc fail!\n");
        task_free_cwd(new_task);
        kfree(new_task);
        if (stack_base) kfree(stack_base);
        if (kernel_stack_base) kstack_free(kernel_stack_base);
        return NULL;
    }
    task_init_fds(new_task);
    if (stack_base) memset(stack_base, 0, TASK_STACK_SIZE);

    lock_scheduler();
    if (user_mode) {
        new_task->id = next_task_id;
        new_task->pid = next_task_id;
        next_task_id++;
    } else {
        new_task->id = 0x8000000000000000ULL |
                       (uint64_t)(-(int64_t)next_kernel_pid);
        new_task->pid = next_kernel_pid;
        next_kernel_pid--;
    }
    unlock_scheduler();

    creds_init_task(new_task);
    signals_init_task(new_task);

    new_task->start_tick = tick_count;
    new_task->state = initial_state;
    new_task->next = NULL;
    new_task->cr3 = cr3;
    new_task->console_id = -1; 
    DEBUG_TASK("new task id=%u cr3=0x%016lX\n", new_task->id, cr3);
    new_task->time_slice = SCHED_DEFAULT_TIMESLICE;
    new_task->base_time_slice = SCHED_DEFAULT_TIMESLICE;
    new_task->stack_base = stack_base;
    new_task->stack_size = TASK_STACK_SIZE;
    new_task->kernel_stack_base = kernel_stack_base;
    new_task->kernel_stack_size = KSTACK_USABLE_SIZE;
    new_task->wake_tick = 0;
    new_task->sleep_next = NULL;
    new_task->in_sleep_queue = 0;
    new_task->in_wait_queue = 0;
    new_task->wait_next = NULL;
    new_task->waiting_queue = NULL;
    new_task->join_target = NULL;
    waitq_init(&new_task->join_waiters);
    new_task->join_refs = 0;
    new_task->exit_code = 0;
    new_task->waiting_for_any_child = 0;
    new_task->is_user = user_mode;
    new_task->syscall_frame = NULL;
    new_task->vring_minor = 0;
    new_task->is_kernel_task = false;

    if (user_mode) {
        krsp = (uint64_t*)(kernel_stack_base + KSTACK_USABLE_SIZE);
        
        user_rsp = USER_STACK_INIT_ESP;
        
        *--krsp = 0x23;
        *--krsp = user_rsp;
        *--krsp = 0x202;
        *--krsp = 0x1B;
        *--krsp = (uint64_t)entry;
        
        *--krsp = 0;
        *--krsp = 0;
        
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        *--krsp = 0;
        
        *--krsp = 0x23;
        *--krsp = 0x23;
        *--krsp = cr3;
        *--krsp = cr3;

        new_task->regs.rsp = (uint64_t)krsp;
        new_task->regs.rip = (uint64_t)entry;
        new_task->regs.rflags = 0x202;
        new_task->regs.cs = 0x1B;
        new_task->regs.ds = new_task->regs.es = 0x23;
        new_task->user_brk = 0;
        new_task->user_brk_start = 0;
        new_task->mmap_next_addr = 0x10000000;
        
    } else {
        rsp_ptr = (uint64_t*)(stack_base + TASK_STACK_SIZE);
        *--rsp_ptr = 0x10;
        *--rsp_ptr = (uint64_t)(stack_base + TASK_STACK_SIZE);
        *--rsp_ptr = 0x202;
        *--rsp_ptr = 0x08;
        *--rsp_ptr = (uint64_t)entry;
        *--rsp_ptr = 0;
        *--rsp_ptr = 48;
        *--rsp_ptr = 0;
        *--rsp_ptr = 0;
        *--rsp_ptr = 0;
        *--rsp_ptr = 0;
        *--rsp_ptr = 0;
        *--rsp_ptr = 0;
        *--rsp_ptr = 0;
        *--rsp_ptr = 0;
        *--rsp_ptr = 0;
        *--rsp_ptr = 0;
        *--rsp_ptr = 0;
        *--rsp_ptr = 0;
        *--rsp_ptr = 0;
        *--rsp_ptr = 0;
        *--rsp_ptr = 0;
        *--rsp_ptr = 0;
        *--rsp_ptr = 0x10;
        *--rsp_ptr = 0x10;
        *--rsp_ptr = cr3;
        *--rsp_ptr = cr3;
        new_task->regs.rsp = (uint64_t)rsp_ptr;
        new_task->regs.rip = (uint64_t)entry;
        new_task->regs.rflags = 0x202;
        new_task->regs.cs = 0x08;
        new_task->regs.ds = new_task->regs.es = 0x10;
    }

    DEBUG_TASK("Task created: id=%u pid=%u RIP=0x%016lX RSP=0x%016lX%s\n", new_task->id, new_task->pid, (uint64_t)entry, new_task->regs.rsp, user_mode ? " (USER)" : "");

    lock_scheduler();
    new_task->all_next = all_tasks_head;
    all_tasks_head = new_task;
    unlock_scheduler();

    return new_task;
}

task_t* create_kernel_task(void (*entry)(void), task_state_t initial_state) {
    task_t *t;

    t = create_task(entry, initial_state, false);
    if (!t) return NULL;
    t->is_kernel_task = true;
    return t;
}

void sleep_ticks(uint64_t ticks) {
    uint64_t new_wake;

    if (!current_task || ticks == 0) return;
    lock_scheduler();
    new_wake = tick_count + ticks;
    sleepq_remove(current_task);
    if (current_task->in_sleep_queue) {
        current_task->wake_tick = new_wake;
        unlock_scheduler();
        schedule();
        return;
    }
    current_task->wake_tick = new_wake;
    current_task->state = TASK_BLOCKED;
    current_task->in_sleep_queue = 1;
    current_task->sleep_next = sleep_queue_head;
    sleep_queue_head = current_task;
    unlock_scheduler();
    schedule();
}

void wake_sleeping_tasks(void) {
    int safety;
    task_t **ptr;
    task_t *t;
    task_t *next;
    safety = 0;

    if (!sleep_queue_head) return;
    lock_scheduler();
    ptr = &sleep_queue_head;
    while (*ptr) {
        if (++safety > 10000) {
            *ptr = NULL;
            break;
        }
        t = *ptr;
        if (!task_ptr_valid(t)) {
            *ptr = NULL;
            break;
        }
        if (t->wake_tick <= tick_count) {
            *ptr = t->sleep_next;
            t->sleep_next = NULL;
            t->in_sleep_queue = 0;
            if (t->state == TASK_BLOCKED) t->state = TASK_READY;
        } else {
            next = t->sleep_next;
            if (next && !task_ptr_valid(next)) {
                t->sleep_next = NULL;
                break;
            }
            ptr = &t->sleep_next;
        }
    }
    unlock_scheduler();
}

void task_exit(uint64_t exit_code) {
    task_exit_deferred(exit_code);
    schedule();
    for (;;) asm volatile ("hlt");
}

void task_exit_deferred(uint64_t exit_code) {
    cpu_info_t *cpu;
    task_t *task;

    cpu = smp_this_cpu();
    task = cpu ? cpu->current_task : current_task;
    if (!task) return;
    lock_scheduler();
    if (task->state == TASK_DEAD) {
        unlock_scheduler();
        return;
    }
    task->exit_code = exit_code;
    sleepq_remove(task);
    if (task->waiting_queue) {
        waitq_remove(task->waiting_queue, task);
    }
    if (task->join_target && !task->waiting_for_any_child) {
        if (task->join_target->join_refs) task->join_target->join_refs--;
        waitq_remove(&task->join_target->join_waiters, task);
    }
    task->join_target = NULL;
    task->waiting_for_any_child = 0;
    task->state = TASK_DEAD;
    remove_task_from_runqueue(task);
    task->wait_next = dead_queue_head;
    dead_queue_head = task;
    task_finish_death_locked(task);
    unlock_scheduler();
    reap_request();
}

void sleep_ms(uint64_t ms) {
    extern uint64_t pit_freq;
    uint64_t ticks;

    if (ms == 0) return;
    ticks = (ms * pit_freq + 999) / 1000; 
    if (ticks == 0) ticks = 1;
    sleep_ticks(ticks);
}

static void task_clear_file_mappings(task_t *t) {
    int i;

    if (!t) return;
    if (!t->file_maps) {
        t->file_map_count = 0;
        t->file_map_capacity = 0;
        return;
    }
    for (i = 0; i < t->file_map_count; i++) {
        if (t->file_maps[i].node) {
            vfs_close(t->file_maps[i].node);
            t->file_maps[i].node = NULL;
        }
    }
    kfree(t->file_maps);
    t->file_maps = NULL;
    t->file_map_count = 0;
    t->file_map_capacity = 0;
}

static int task_track_user_page(task_t *task, uint64_t phys) {
    uint64_t *new_user_pages;

    if (!task || !phys) return -1;
    new_user_pages = (uint64_t *)krealloc(task->user_pages, (task->user_pages_count + 1) * sizeof(uint64_t));
    if (!new_user_pages) return -1;
    task->user_pages = new_user_pages;
    task->user_pages[task->user_pages_count] = phys;
    task->user_pages_count++;
    return 0;
}

int task_replace_user_page(task_t *task, uint64_t old_phys, uint64_t new_phys) {
    uint64_t i;

    if (!task || !new_phys) return -1;
    for (i = 0; i < task->user_pages_count; i++) {
        if (task->user_pages[i] == old_phys) {
            task->user_pages[i] = new_phys;
            return 0;
        }
    }
    return task_track_user_page(task, new_phys);
}

static int task_ensure_file_map_capacity(task_t *task, int needed) {
    task_file_map_t *new_maps;
    int new_cap;

    if (!task || needed < 0 || needed > TASK_MAX_FILE_MAPS) return -1;
    if (needed <= task->file_map_capacity) return 0;
    new_cap = task->file_map_capacity;
    if (new_cap < TASK_INIT_FILE_MAPS) new_cap = TASK_INIT_FILE_MAPS;
    while (new_cap < needed) {
        new_cap *= 2;
        if (new_cap > TASK_MAX_FILE_MAPS) new_cap = TASK_MAX_FILE_MAPS;
    }
    if (new_cap < needed) return -1;
    new_maps = (task_file_map_t *)kmalloc(new_cap * sizeof(task_file_map_t));
    if (!new_maps) return -1;
    memset(new_maps, 0, new_cap * sizeof(task_file_map_t));
    if (task->file_maps && task->file_map_count > 0) {
        memcpy(new_maps, task->file_maps, task->file_map_count * sizeof(task_file_map_t));
        kfree(task->file_maps);
    }
    task->file_maps = new_maps;
    task->file_map_capacity = new_cap;
    return 0;
}

static exec_page_cache_entry_t *exec_page_cache_find_locked(vfs_node_t *node, uint64_t offset) {
    exec_page_cache_entry_t *entry;

    entry = exec_page_cache_head;
    while (entry) {
        if (entry->node == node && entry->offset == offset) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

static uint64_t exec_page_cache_target_pages(void) {
    return 0;
}

static int exec_page_cache_reclaim_one(void) {
    exec_page_cache_entry_t *entry;
    exec_page_cache_entry_t *prev;
    exec_page_cache_entry_t *best;
    exec_page_cache_entry_t *best_prev;
    vfs_node_t *node;
    uint64_t phys;

    best = NULL;
    best_prev = NULL;
    spin_lock(&exec_page_cache_lock);
    prev = NULL;
    entry = exec_page_cache_head;
    while (entry) {
        if (pfa_ref_get(entry->phys) == 1) {
            if (!best || entry->last_access < best->last_access) {
                best = entry;
                best_prev = prev;
            }
        }
        prev = entry;
        entry = entry->next;
    }
    if (!best) {
        spin_unlock(&exec_page_cache_lock);
        return 0;
    }
    if (best_prev) {
        best_prev->next = best->next;
    } else {
        exec_page_cache_head = best->next;
    }
    if (exec_page_cache_pages > 0) {
        exec_page_cache_pages--;
    }
    node = best->node;
    phys = best->phys;
    spin_unlock(&exec_page_cache_lock);

    pfa_ref_dec(phys);
    pfa_free(phys);
    if (node) vfs_close(node);
    kfree(best);
    return 1;
}

void exec_page_cache_reclaim(uint64_t target_pages) {
    while (exec_page_cache_pages > target_pages) {
        if (!exec_page_cache_reclaim_one()) break;
    }
}

uint64_t exec_page_cache_get_pages(void) {
    return exec_page_cache_pages;
}

uint64_t exec_page_cache_get_reclaimable_pages(void) {
    exec_page_cache_entry_t *entry;
    uint64_t pages;

    pages = 0;
    spin_lock(&exec_page_cache_lock);
    entry = exec_page_cache_head;
    while (entry) {
        if (pfa_ref_get(entry->phys) == 1) {
            pages++;
        }
        entry = entry->next;
    }
    spin_unlock(&exec_page_cache_lock);
    return pages;
}

static uint64_t exec_page_find_active(vfs_node_t *node, uint64_t offset) {
    task_t *task;
    uint64_t vaddr;
    uint64_t phys;
    uint8_t old_ref;
    int added_existing_ref;
    int i;

    if (!node) return 0;
    phys = 0;
    lock_scheduler();
    task = all_tasks_head;
    while (task && task_ptr_valid(task)) {
        if (task != current_task && task->is_user && task->state != TASK_DEAD && task->pml4_phys) {
            for (i = 0; i < task->file_map_count; i++) {
                if (!overlay_same_file(task->file_maps[i].node, node)) continue;
                if (offset < task->file_maps[i].offset) continue;
                if (offset >= task->file_maps[i].offset + task->file_maps[i].filesz) continue;
                vaddr = task->file_maps[i].vaddr + (offset - task->file_maps[i].offset);
                phys = vmm_get_phys_in_pml4(task->pml4_phys, vaddr);
                if (phys) break;
            }
        }
        if (phys) break;
        task = task->all_next;
    }
    if (phys) {
        added_existing_ref = 0;
        old_ref = pfa_ref_get(phys);
        if (old_ref == 0) {
            pfa_ref_inc(phys);
            if (pfa_ref_get(phys) == 0) phys = 0;
            else added_existing_ref = 1;
        }
        if (phys) {
            old_ref = pfa_ref_get(phys);
            pfa_ref_inc(phys);
            if (pfa_ref_get(phys) <= old_ref) {
                if (added_existing_ref) pfa_ref_dec(phys);
                phys = 0;
            }
        }
    }
    unlock_scheduler();
    return phys;
}

static int exec_page_cache_get(vfs_node_t *node, uint64_t offset, uint64_t read_len,
                               uint64_t *out_phys) {
    exec_page_cache_entry_t *entry;
    exec_page_cache_entry_t *existing;
    uint64_t phys;
    uint64_t old_ref;
    uint8_t *dst;

    if (!node || !out_phys) return -1;
    *out_phys = 0;
    offset &= ~(PAGE_SIZE - 1);
    if (exec_page_cache_target_pages() == 0) {
        phys = exec_page_find_active(node, offset);
        if (!phys) return -1;
        *out_phys = phys;
        return 0;
    }
    spin_lock(&exec_page_cache_lock);
    entry = exec_page_cache_find_locked(node, offset);
    if (entry) {
        old_ref = pfa_ref_get(entry->phys);
        pfa_ref_inc(entry->phys);
        if (pfa_ref_get(entry->phys) > old_ref) {
            phys = entry->phys;
            entry->last_access = ++exec_page_cache_clock;
            spin_unlock(&exec_page_cache_lock);
            *out_phys = phys;
            return 0;
        }
        spin_unlock(&exec_page_cache_lock);
        return -1;
    }
    spin_unlock(&exec_page_cache_lock);

    phys = pfa_alloc();
    if (!phys) return -1;
    pmm_zero_page_phys(phys);

    if (read_len > PAGE_SIZE) read_len = PAGE_SIZE;
    if (read_len > 0) {
        temp_map_raw(TASK_FILE_FAULT_TEMP, phys);
        dst = (uint8_t *)TASK_FILE_FAULT_TEMP;
        if (vfs_read(node, offset, read_len, dst) != read_len) {
            temp_unmap_raw(TASK_FILE_FAULT_TEMP);
            pfa_free(phys);
            return -1;
        }
        temp_unmap_raw(TASK_FILE_FAULT_TEMP);
    }

    entry = (exec_page_cache_entry_t *)kmalloc(sizeof(exec_page_cache_entry_t));
    if (!entry) {
        pfa_free(phys);
        return -1;
    }
    entry->node = node;
    entry->offset = offset;
    entry->phys = phys;
    entry->last_access = ++exec_page_cache_clock;
    entry->next = NULL;

    pfa_ref_inc(phys);
    if (pfa_ref_get(phys) == 0) {
        kfree(entry);
        pfa_free(phys);
        return -1;
    }
    old_ref = pfa_ref_get(phys);
    pfa_ref_inc(phys);
    if (pfa_ref_get(phys) <= old_ref) {
        pfa_ref_dec(phys);
        kfree(entry);
        pfa_free(phys);
        return -1;
    }

    vfs_open(node, 0);

    spin_lock(&exec_page_cache_lock);
    existing = exec_page_cache_find_locked(node, offset);
    if (existing) {
        spin_unlock(&exec_page_cache_lock);
        vfs_close(node);
        pfa_ref_dec(phys);
        pfa_ref_dec(phys);
        pfa_free(phys);

        spin_lock(&exec_page_cache_lock);
        old_ref = pfa_ref_get(existing->phys);
        pfa_ref_inc(existing->phys);
        if (pfa_ref_get(existing->phys) > old_ref) {
            phys = existing->phys;
            existing->last_access = ++exec_page_cache_clock;
            spin_unlock(&exec_page_cache_lock);
            kfree(entry);
            *out_phys = phys;
            return 0;
        }
        spin_unlock(&exec_page_cache_lock);
        kfree(entry);
        return -1;
    }
    entry->next = exec_page_cache_head;
    exec_page_cache_head = entry;
    exec_page_cache_pages++;
    spin_unlock(&exec_page_cache_lock);
    exec_page_cache_reclaim(exec_page_cache_target_pages());

    *out_phys = phys;
    return 0;
}

void exec_page_cache_on_page_release(uint64_t phys) {
    if (!phys) return;
}

void task_free_user_memory(task_t* t) {
    int freed;

    if (!t) return;

    task_clear_file_mappings(t);

    if (t->pml4_phys) {
        freed = task_free_pml4_if_unowned(t, t->pml4_phys);
        if (!freed) return;
        t->pml4_phys = 0;
        exec_page_cache_reclaim(exec_page_cache_target_pages());
        pfa_ref_gc();
    }

    if (t->user_pages) {
        kfree(t->user_pages);
        t->user_pages = NULL;
        t->user_pages_count = 0;
    }
}

static void task_release_exit_resources(task_t *t) {
    int freed;

    if (!t || t->resources_released) return;
    if (task_is_current_on_any_cpu(t)) return;

    if (t->exec_old_pml4) {
        freed = task_free_exec_old_pml4_if_unowned(t, t->exec_old_pml4);
        if (freed) {
            t->exec_old_pml4 = 0;
            if (t->exec_old_pages) {
                kfree(t->exec_old_pages);
                t->exec_old_pages = NULL;
                t->exec_old_pages_count = 0;
            }
        } else {
            return;
        }
    } else if (t->exec_old_pages) {
        kfree(t->exec_old_pages);
        t->exec_old_pages = NULL;
        t->exec_old_pages_count = 0;
    }
    task_free_user_memory(t);
    if (t->pml4_phys) return;
    t->cr3 = 0;
    t->regs.entry_cr3 = 0;
    t->regs.return_cr3 = 0;
    t->regs.saved_entry_cr3 = 0;
    if (t->syscall_frame) {
        t->syscall_frame->entry_cr3 = 0;
        t->syscall_frame->return_cr3 = 0;
        t->syscall_frame->saved_entry_cr3 = 0;
    }
    task_fd_close_all(t);
    if (t->fds) {
        kfree(t->fds);
        t->fds = NULL;
        t->fds_capacity = 0;
    }
    task_free_signal_data(t);
    if (t->creds_data) {
        kfree(t->creds_data);
        t->creds_data = NULL;
    }
    task_free_cwd(t);
    t->resources_released = 1;
}

static void task_release_dead_resources(task_t *t) {
    if (!t) return;

    task_release_exit_resources(t);
    if (!t->resources_released) return;
    task_free_fpu_state(t);
    if (t->stack_base) {
        kfree(t->stack_base);
        t->stack_base = NULL;
        t->stack_size = 0;
    }
    if (t->kernel_stack_base) {
        kstack_free(t->kernel_stack_base);
        t->kernel_stack_base = NULL;
        t->kernel_stack_size = 0;
    }
}

int task_add_file_mapping(task_t *task, vfs_node_t *node, uint64_t vaddr,
                          uint64_t memsz, uint64_t filesz, uint64_t offset,
                          uint64_t flags) {
    uint64_t start;
    uint64_t end;
    uint64_t delta;
    int idx;

    if (!task || !node || memsz == 0) return -1;
    if (task->file_map_count >= TASK_MAX_FILE_MAPS) return -1;
    start = vaddr & ~(PAGE_SIZE - 1);
    delta = vaddr - start;
    if (offset < delta) return -1;
    end = (vaddr + memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (task_ensure_file_map_capacity(task, task->file_map_count + 1) != 0) return -1;
    idx = task->file_map_count;
    vfs_open(node, 0);
    task->file_maps[idx].node = node;
    task->file_maps[idx].vaddr = start;
    task->file_maps[idx].memsz = end - start;
    task->file_maps[idx].filesz = filesz + delta;
    task->file_maps[idx].offset = offset - delta;
    task->file_maps[idx].flags = flags;
    task->file_map_count++;
    return 0;
}

int task_handle_file_page_fault(task_t *task, uint64_t fault_addr) {
    uint64_t page;
    uint64_t phys;
    uint64_t read_start;
    uint64_t read_end;
    uint64_t read_off;
    uint64_t read_len;
    uint64_t mapped_phys;
    uint64_t map_flags;
    int cached_page;
    int match;
    uint8_t *dst;
    int i;

    if (!task || !task->is_user) return 0;
    page = fault_addr & ~(PAGE_SIZE - 1);
    for (i = 0; i < task->file_map_count; i++) {
        if (!task->file_maps[i].node) continue;
        if (page < task->file_maps[i].vaddr) continue;
        if (page >= task->file_maps[i].vaddr + task->file_maps[i].memsz) continue;
        if (task->file_maps[i].flags & 0x2) {
            break;
        }
    }
    match = i;
    if (match >= task->file_map_count) {
        for (i = 0; i < task->file_map_count; i++) {
            if (!task->file_maps[i].node) continue;
            if (page < task->file_maps[i].vaddr) continue;
            if (page >= task->file_maps[i].vaddr + task->file_maps[i].memsz) continue;
            match = i;
            break;
        }
    }
    if (match >= task->file_map_count) return 0;

    phys = 0;
    cached_page = 0;
    map_flags = task->file_maps[match].flags;
    read_start = page;
    read_end = page + PAGE_SIZE;
    if (read_start < task->file_maps[match].vaddr) read_start = task->file_maps[match].vaddr;
    if (read_end > task->file_maps[match].vaddr + task->file_maps[match].filesz) {
        read_end = task->file_maps[match].vaddr + task->file_maps[match].filesz;
    }
    if (read_end > read_start) {
        read_off = task->file_maps[match].offset + (read_start - task->file_maps[match].vaddr);
        read_len = read_end - read_start;
        if (read_start == page && !(task->file_maps[match].flags & 0x2)) {
            if (exec_page_cache_get(task->file_maps[match].node, read_off, read_len,
                                    &phys) != 0) {
                phys = 0;
            } else {
                cached_page = 1;
            }
        }
        if (!phys) {
            phys = pfa_alloc();
            if (!phys) return 0;
            pmm_zero_page_phys(phys);
            temp_map_raw(TASK_FILE_FAULT_TEMP, phys);
            dst = (uint8_t *)(TASK_FILE_FAULT_TEMP + (read_start - page));
            if (vfs_read(task->file_maps[match].node, read_off, read_len, dst) != read_len) {
                temp_unmap_raw(TASK_FILE_FAULT_TEMP);
                pfa_free(phys);
                return 0;
            }
            temp_unmap_raw(TASK_FILE_FAULT_TEMP);
        }
    } else {
        phys = pfa_alloc();
        if (!phys) return 0;
        pmm_zero_page_phys(phys);
    }
    vmm_map_page_in_pml4(task->pml4_phys, page, phys, map_flags);
    mapped_phys = vmm_get_phys_in_pml4(task->pml4_phys, page);
    if (mapped_phys == 0) {
        if (cached_page) {
            pfa_ref_dec(phys);
        } else {
            pfa_free(phys);
        }
        return 0;
    }
    if (task_track_user_page(task, phys) != 0) {
        vmm_unmap_page_in_pml4(task->pml4_phys, page);
        if (cached_page) {
            pfa_ref_dec(phys);
        } else {
            pfa_free(phys);
        }
        return 0;
    }
    return 1;
}

int task_handle_file_write_fault(task_t *task, uint64_t fault_addr) {
    uint64_t page;
    uint64_t old_phys;
    uint64_t new_phys;
    uint64_t map_flags;
    uint8_t *old_ptr;
    uint8_t *new_ptr;
    int match;
    int i;

    if (!task || !task->is_user) return 0;
    page = fault_addr & ~(PAGE_SIZE - 1);
    match = -1;
    for (i = 0; i < task->file_map_count; i++) {
        if (!task->file_maps[i].node) continue;
        if (!(task->file_maps[i].flags & 0x2)) continue;
        if (page < task->file_maps[i].vaddr) continue;
        if (page >= task->file_maps[i].vaddr + task->file_maps[i].memsz) continue;
        match = i;
        break;
    }
    if (match < 0) return 0;

    old_phys = vmm_get_phys_in_pml4(task->pml4_phys, page);
    if (!old_phys) return 0;

    new_phys = pfa_alloc();
    if (!new_phys) return 0;

    old_ptr = (uint8_t *)TEMP_SLOT(6);
    new_ptr = (uint8_t *)TEMP_SLOT(7);
    temp_map_raw((uint64_t)old_ptr, old_phys);
    temp_map_raw((uint64_t)new_ptr, new_phys);
    memcpy(new_ptr, old_ptr, PAGE_SIZE);
    temp_unmap_raw((uint64_t)old_ptr);
    temp_unmap_raw((uint64_t)new_ptr);

    map_flags = task->file_maps[match].flags | 0x2ULL;
    vmm_map_page_in_pml4(task->pml4_phys, page, new_phys, map_flags);
    if (vmm_get_phys_in_pml4(task->pml4_phys, page) != new_phys) {
        pfa_free(new_phys);
        return 0;
    }

    if (task_replace_user_page(task, old_phys, new_phys) != 0) {
        vmm_unmap_page_in_pml4(task->pml4_phys, page);
        pfa_free(new_phys);
        return 0;
    }
    exec_page_cache_on_page_release(old_phys);
    pfa_cow_release64(old_phys);
    return 1;
}

void reap_dead_tasks(void) {
    task_t *t;
    task_t *keep = NULL;
    task_t *next;
    task_t *k;
    task_t *knext;
    int current_on_cpu;

    if (!dead_queue_head) return;
    lock_scheduler();
    t = dead_queue_head;
    dead_queue_head = NULL;
    unlock_scheduler();

    while (t && task_ptr_valid(t)) {
        next = t->wait_next;
        t->wait_next = NULL;

        current_on_cpu = task_is_current_on_any_cpu(t);
        if (current_on_cpu || t->join_refs != 0 || t->in_wait_queue ||
            (t->ppid > 0 && !t->waited && task_find(t->ppid))) {
            if (!current_on_cpu && t != current_task) {
                task_release_dead_resources(t);
            }
            t->wait_next = keep;
            keep = t;
            t = next;
            continue;
        }

        task_release_dead_resources(t);
        if (!t->resources_released) {
            t->wait_next = keep;
            keep = t;
            t = next;
            continue;
        }

        {
            task_t **pp;
            lock_scheduler();
            pp = &all_tasks_head;
            while (*pp) {
                if (*pp == t) {
                    *pp = t->all_next;
                    break;
                }
                pp = &(*pp)->all_next;
            }
            unlock_scheduler();
        }

        kfree(t);
        t = next;
    }

    if (keep) {
        lock_scheduler();
        k = keep;
        while (k && task_ptr_valid(k)) {
            knext = k->wait_next;
            k->wait_next = dead_queue_head;
            dead_queue_head = k;
            k = knext;
        }
        unlock_scheduler();
    }
}

void reap_request(void) {
    reap_pending = 1;
}

void exec_drain_request(void) {
    exec_drain_pending = 1;
}

static void task_reclaim_stale_exec_now(void) {
    task_t *t;
    task_t *owner;
    task_t *scan;
    uint64_t pd;
    uint64_t *pages;
    uint64_t count;
    int found;
    int freed;
    int limit;

    do {
        owner = NULL;
        pd = 0;
        pages = NULL;
        count = 0;
        found = 0;

        lock_scheduler();
        t = all_tasks_head;
        while (t && task_ptr_valid(t)) {
            if (t->is_user && !t->resources_released && !t->exec_completed &&
                t->exec_old_pml4 && t->pml4_phys != t->exec_old_pml4 &&
                !task_is_current_on_any_cpu(t)) {
                owner = t;
                pd = t->exec_old_pml4;
                found = 1;
                break;
            }
            t = t->all_next;
        }
        unlock_scheduler();

        if (found) {
            freed = task_free_exec_old_pml4_if_unowned(owner, pd);
            if (freed) {
                lock_scheduler();
                scan = all_tasks_head;
                limit = 1024;
                while (scan && limit > 0) {
                    if (!task_ptr_valid(scan)) break;
                    if (scan == owner) {
                        if (scan->exec_old_pml4 == pd) {
                            pages = scan->exec_old_pages;
                            count = scan->exec_old_pages_count;
                            scan->exec_old_pml4 = 0;
                            scan->exec_old_pages = NULL;
                            scan->exec_old_pages_count = 0;
                        }
                        break;
                    }
                    scan = scan->all_next;
                    limit--;
                }
                unlock_scheduler();
                (void)count;
                if (pages) kfree(pages);
            } else {
                found = 0;
            }
        }
    } while (found);
}

static void task_relax_all_single_cow_pages(void) {
    task_t *t;
    uint64_t pml4s[64];
    int count;
    int i;

    count = 0;
    lock_scheduler();
    t = all_tasks_head;
    while (t && task_ptr_valid(t) && count < (int)(sizeof(pml4s) / sizeof(pml4s[0]))) {
        if (t->is_user && t->state != TASK_DEAD && !t->resources_released && t->pml4_phys) {
            pml4s[count++] = t->pml4_phys;
        }
        t = t->all_next;
    }
    unlock_scheduler();

    for (i = 0; i < count; i++) {
        vmm_relax_single_cow_pages(pml4s[i]);
    }
}

void task_reclaim_exited_now(void) {
    task_reclaim_stale_exec_now();
    exec_cleanup_drain();
    reap_dead_tasks();
    task_relax_all_single_cow_pages();
    exec_page_cache_reclaim(0);
    slab_gc();
    pfa_ref_gc();
    heap_reclaim_unused();
    exec_cleanup_drain();
    exec_page_cache_reclaim(0);
    pfa_ref_gc();
}

void task_memory_collect_for_report(void) {
    task_reclaim_exited_now();
}

void task_get_memory_stats_for_pml4(task_mem_stats_t *stats, uint64_t current_pml4) {
    task_t *t;
    task_t *d;
    uint64_t exec_cache;
    uint64_t exec_reclaim;
    uint64_t user_pt_pages;
    uint64_t heap_pages;
    uint64_t mmap_pages;
    uint64_t file_pages;

    if (!stats) return;
    memset(stats, 0, sizeof(*stats));

    lock_scheduler();
    t = all_tasks_head;
    while (t && task_ptr_valid(t)) {
        if (t->is_user && t->state != TASK_DEAD && !t->resources_released) {
            heap_pages = task_heap_pages(t);
            file_pages = task_file_pages(t);
            mmap_pages = task_mmap_pages(t);
            stats->active_user_pages += t->user_pages_count;
            stats->active_heap_pages += heap_pages;
            stats->active_mmap_pages += mmap_pages;
            stats->active_file_pages += file_pages;
            stats->active_stack_pages += task_stack_pages(t);
            if (t->pml4_phys) {
                stats->active_pd_pages++;
                user_pt_pages = vmm_count_user_page_tables(t->pml4_phys);
                stats->active_user_pt_pages += user_pt_pages;
            } else {
                user_pt_pages = 0;
            }
            if ((current_pml4 && t->pml4_phys == current_pml4) || (!current_pml4 && t == current_task)) {
                stats->current_user_pages += t->user_pages_count;
                stats->current_heap_pages += heap_pages;
                stats->current_mmap_pages += mmap_pages;
                stats->current_file_pages += file_pages;
                stats->current_stack_pages += task_stack_pages(t);
                if (t->pml4_phys) {
                    stats->current_pd_pages++;
                    stats->current_user_pt_pages += user_pt_pages;
                }
            }
        }
        t = t->all_next;
    }

    d = dead_queue_head;
    while (d && task_ptr_valid(d)) {
        if (d->is_user && !d->resources_released) {
            stats->dead_user_pages += d->user_pages_count;
            stats->dead_stack_pages += task_stack_pages(d);
            if (d->pml4_phys) {
                stats->dead_pd_pages++;
                stats->dead_user_pt_pages += vmm_count_user_page_tables(d->pml4_phys);
            }
            stats->dead_exec_old_pages += d->exec_old_pages_count;
        }
        d = d->wait_next;
    }
    unlock_scheduler();

    exec_cleanup_stats(&stats->exec_cleanup_entries, &stats->exec_cleanup_user_pages);
    exec_cache = exec_page_cache_get_pages();
    exec_reclaim = exec_page_cache_get_reclaimable_pages();
    stats->exec_cache_pages = exec_cache;
    stats->exec_reclaim_pages = exec_reclaim;
    if (exec_cache >= exec_reclaim) {
        stats->exec_nonreclaim_pages = exec_cache - exec_reclaim;
    } else {
        stats->exec_nonreclaim_pages = 0;
    }
}

void task_get_memory_stats(task_mem_stats_t *stats) {
    task_get_memory_stats_for_pml4(stats, 0);
}

void task_deferred_work(void) {
    int reclaim;

    reclaim = 0;
    if (reap_pending) {
        reap_pending = 0;
        reap_dead_tasks();
        reclaim = 1;
    }
    if (exec_drain_pending) {
        exec_drain_pending = 0;
        exec_cleanup_drain();
        reclaim = 1;
    }
    if (reclaim) {
        exec_cleanup_drain();
        exec_page_cache_reclaim(0);
        slab_gc();
        pfa_ref_gc();
        heap_reclaim_unused();
        exec_cleanup_drain();
        pfa_ref_gc();
    }
    if ((!current_task || !current_task->is_user) &&
        smp_processor_id() == 0 && tick_count >= 10000) {
        extern void ext4_background_writeback(uint32_t max_blocks);
        ext4_background_writeback(4);
        exec_page_cache_reclaim(exec_page_cache_target_pages());
    }
}

void switch_to(task_t* next) {
    uint64_t rsp0;
    task_t *prev;

    if (next == current_task) return;

    prev = current_task;
    current_task = next;
    smp_this_cpu()->current_task = next;
    prev->state = TASK_READY;
    next->state = TASK_RUNNING;

    if (next->kernel_stack_base && next->kernel_stack_size) {
        rsp0 = (uint64_t)next->kernel_stack_base + next->kernel_stack_size;
        tss_set_rsp0(rsp0);
    }

    if (next->regs.rsp != 0 && 
        !((next->regs.rsp >= KERNEL_VMA && next->regs.rsp < HEAP_START) ||
          (next->regs.rsp >= HEAP_START && next->regs.rsp < kernel_heap.max_addr) ||
          kstack_is_in_region(next->regs.rsp))) {
        printf("Switch guard: bad rsp 0x%016lX for task %d\n", next->regs.rsp, next->id);
        return;
    }

    prev->cr3 = read_cr3();
    fpu_save_area(prev->fpu_state);
    fpu_restore_area(next->fpu_state);

    switch_to_asm(&prev->regs.rsp, next->regs.rsp);

    next->cr3 = read_cr3();
}

void schedule(void) {
    smp_this_cpu()->schedule_force = 1;
    asm volatile ("int $48");
}

void yield(void) {
    task_deferred_work();
    schedule();
}

void block_current(void) {
    lock_scheduler();
    if (current_task) current_task->state = TASK_BLOCKED;
    unlock_scheduler();
    smp_this_cpu()->schedule_force = 1;
    schedule();
}

void wake_task(task_t* task) {
    if (!task) return;
    lock_scheduler();
    if (task->state == TASK_BLOCKED) {
        if (task->waiting_queue) {
            waitq_remove(task->waiting_queue, task);
        }
        task->state = TASK_READY;
        sleepq_remove(task);
    }
    unlock_scheduler();
}

void task_kill(task_t* task, uint64_t exit_code) {
    if (!task) return;
    if (task == current_task) {
        task_exit(exit_code);
        return;
    }
    lock_scheduler();
    if (task->state == TASK_DEAD) {
        unlock_scheduler();
        return;
    }
    task->exit_code = exit_code;
    sleepq_remove(task);
    if (task->waiting_queue) {
        waitq_remove(task->waiting_queue, task);
    }
    if (task->join_target && !task->waiting_for_any_child) {
        if (task->join_target->join_refs) task->join_target->join_refs--;
        waitq_remove(&task->join_target->join_waiters, task);
    }
    task->join_target = NULL;
    task->waiting_for_any_child = 0;
    task->state = TASK_DEAD;
    remove_task_from_runqueue(task);
    task->wait_next = dead_queue_head;
    dead_queue_head = task;
    task_finish_death_locked(task);
    unlock_scheduler();
    reap_request();
}

int task_join(task_t* task, uint64_t* exit_code) {
    extern int task_has_pending_signals(void);

    if (!task || task == current_task) return -1;

    lock_scheduler();
    if (task->state == TASK_DEAD) {
        if (exit_code) *exit_code = task->exit_code;
        unlock_scheduler();
        return 0;
    }

    if (current_task->in_wait_queue || current_task->waiting_queue) {
        unlock_scheduler();
        return -1;
    }
    task->join_refs++;
    current_task->join_target = task;
    waitq_add(&task->join_waiters, current_task);
    current_task->state = TASK_BLOCKED;
    unlock_scheduler();

    for (;;) {
        schedule();

        lock_scheduler();
        if (task->state == TASK_DEAD) {
            break;
        }
        if (task_has_pending_signals()) {
            waitq_remove(&task->join_waiters, current_task);
            if (task->join_refs) task->join_refs--;
            current_task->join_target = NULL;
            unlock_scheduler();
            return -KERR_EINTR;
        }
        if (!current_task->waiting_queue) {
            if (task->join_refs) task->join_refs--;
            current_task->join_target = NULL;
            unlock_scheduler();
            return -KERR_EINTR;
        }
        current_task->state = TASK_BLOCKED;
        unlock_scheduler();
    }
    if (exit_code) *exit_code = task->exit_code;
    if (current_task->waiting_queue) {
        waitq_remove(current_task->waiting_queue, current_task);
    }
    if (task->join_refs) task->join_refs--;
    current_task->join_target = NULL;
    unlock_scheduler();
    return 0;
}

static inline void save_irq_frame_into_task(task_t* task, const registers_t* regs, uint64_t regs_ptr, uint64_t entry_cr3) {
    task->regs.return_cr3 = regs->return_cr3;
    task->regs.entry_cr3 = regs->entry_cr3;
    task->regs.es = regs->es;
    task->regs.ds = regs->ds;

    task->regs.r15 = regs->r15;
    task->regs.r14 = regs->r14;
    task->regs.r13 = regs->r13;
    task->regs.r12 = regs->r12;
    task->regs.r11 = regs->r11;
    task->regs.r10 = regs->r10;
    task->regs.r9 = regs->r9;
    task->regs.r8 = regs->r8;
    task->regs.rdi = regs->rdi;
    task->regs.rsi = regs->rsi;
    task->regs.rbp = regs->rbp;
    task->regs.rsp = regs_ptr;
    task->regs.rbx = regs->rbx;
    task->regs.rdx = regs->rdx;
    task->regs.saved_entry_cr3 = regs->saved_entry_cr3;
    task->regs.rcx = regs->rcx;
    task->regs.rax = regs->rax;

    task->regs.int_no = regs->int_no;
    task->regs.err_code = regs->err_code;
    task->regs.rip = regs->rip;
    task->regs.cs = regs->cs;
    task->regs.rflags = regs->rflags;
    task->regs.ss = regs->ss;
    task->cr3 = entry_cr3;
}

static int task_irq_return_frame(task_t *task, registers_t **frame_out) {
    registers_t *frame;
    uint64_t rsp;
    uint64_t es_val;
    uint64_t ds_val;
    uint64_t cs_val;
    uint64_t rip_val;
    int valid_es;
    int valid_ds;
    int valid_cs;
    int valid_rip;

    if (frame_out) *frame_out = NULL;
    if (!task) return 0;
    rsp = task->regs.rsp;
    if (rsp == 0) return 0;
    if (!((rsp >= KERNEL_VMA && rsp < HEAP_START) ||
          (rsp >= HEAP_START && rsp < kernel_heap.max_addr) ||
          kstack_is_in_region(rsp))) {
        return 0;
    }
    frame = (registers_t*)rsp;
    es_val = frame->es & 0xFFFF;
    ds_val = frame->ds & 0xFFFF;
    cs_val = frame->cs & 0xFFFF;
    rip_val = frame->rip;
    valid_es = (es_val == 0x10 || es_val == 0x23 || es_val == 0);
    valid_ds = (ds_val == 0x10 || ds_val == 0x23 || ds_val == 0);
    valid_cs = (cs_val == 0x08 || cs_val == 0x1B);
    valid_rip = (cs_val == 0x08 && rip_val >= KERNEL_VMA) ||
                (cs_val == 0x1B && rip_val >= 0x1000 && rip_val < KERNEL_VMA);
    if (!valid_es || !valid_ds || !valid_cs || !valid_rip) return 0;
    if (frame_out) *frame_out = frame;
    return 1;
}

registers_t* schedule_from_irq(registers_t* regs) {
    uint64_t entry_cr3;
    uint64_t kernel_cr3;
    int must_switch;
    int dead_switch;
    bool is_idle;
    int safety;
    uint64_t rsp0;
    int got_lock;
    int run_deferred;
    task_t* prev_task;
    task_t* next;
    task_t* start;
    registers_t* return_frame;
    cpu_info_t *this_cpu;
    registers_t *result;
    int selectable;
    int forced_state_switch;
    task_t *idle_task;

    this_cpu = smp_this_cpu();
    if (!this_cpu || !this_cpu->current_task || !ready_queue_head) return regs;

    prev_task = this_cpu->current_task;
    run_deferred = 0;
    forced_state_switch = prev_task->state == TASK_DEAD ||
                          prev_task->state == TASK_BLOCKED ||
                          prev_task->state == TASK_STOPPED;

    if (this_cpu->scheduler_lock_depth > 0) return regs;

    got_lock = spin_trylock(&sched_lock);
    if (!got_lock) {
        if (!forced_state_switch) return regs;
        spin_lock(&sched_lock);
    }

    result = regs;

    entry_cr3 = regs->entry_cr3;
    if (!entry_cr3) {
        __asm__ volatile ("mov %%cr3, %0" : "=r"(entry_cr3));
    }
    regs->entry_cr3 = entry_cr3;
    regs->return_cr3 = entry_cr3;
    kernel_cr3 = vmm_get_kernel_cr3();
    
    dead_switch = (prev_task->state == TASK_DEAD);
    must_switch = dead_switch || prev_task->state == TASK_BLOCKED || prev_task->state == TASK_STOPPED;

    if (regs->int_no != 48 && prev_task->syscall_frame && !must_switch) {
        goto out_restore_cr3;
    }

    if (!dead_switch) {
        save_irq_frame_into_task(prev_task, regs, (uint64_t)regs, entry_cr3);
    }

    is_idle = (prev_task->id == 0 && !prev_task->is_user);
    
    if (!this_cpu->schedule_force && !must_switch && !is_idle) {
        if (prev_task->time_slice > 0) prev_task->time_slice--;
        if (prev_task->time_slice != 0) {
            goto out_restore_cr3;
        }
    }
    this_cpu->schedule_force = 0;

    if (!must_switch) {
        prev_task->time_slice = prev_task->base_time_slice;
    }

    if (must_switch) {
        next = ready_queue_head;
    } else {
        next = prev_task->next ? prev_task->next : ready_queue_head;
    }
    start = next;
    safety = 0;
    return_frame = NULL;
    
    while (next) {
        if ((next->state == TASK_READY || next->state == TASK_RUNNING) && next != prev_task && !next->resources_released) {
            selectable = 0;
            if (task_irq_return_frame(next, &return_frame)) selectable = 1;
            if (selectable) {
                break;
            }
        }
        next = next->next;
        if (next == start || ++safety > 10000) {
            next = NULL;
            break;
        }
    }

    if (!next && must_switch) {
        idle_task = task_find_idle_locked();
        if (idle_task &&
                ((idle_task == prev_task && is_idle) ||
                 (idle_task != prev_task && !task_is_current_on_any_cpu(idle_task))) &&
                task_irq_return_frame(idle_task, &return_frame)) {
            sleepq_remove(idle_task);
            idle_task->wake_tick = 0;
            idle_task->state = TASK_READY;
            if (!task_in_runqueue_locked(idle_task)) {
                add_task_to_runqueue(idle_task);
            }
            next = idle_task;
        }
    }

    if (!next && must_switch) {
        spin_unlock(&sched_lock);
        printf("schedule: no runnable tasks, system halted\n");
        for (;;) asm volatile ("hlt");
    }
    
    if (!next) {
        goto out_restore_cr3;
    }

    if (!dead_switch) {
        fpu_save_area(prev_task->fpu_state);
    }
    fpu_restore_area(next->fpu_state);

    if (prev_task->state == TASK_RUNNING) prev_task->state = TASK_READY;
    next->state = TASK_RUNNING;
    current_task = next;
    this_cpu->current_task = next;

    if (next->is_user && ((return_frame->cs & 0x3) == 0x3)) {
        return_frame->ds = return_frame->es = 0x23;
        return_frame->cs = 0x1B;
        return_frame->ss = 0x23;
    }

    if (next->kernel_stack_base && next->kernel_stack_size) {
        rsp0 = (uint64_t)next->kernel_stack_base + next->kernel_stack_size;
        tss_set_rsp0(rsp0);
    }

    if (next->is_user) {
        task_write_fs_base(next->tls_base);
    }

    {
        uint64_t target_cr3 = next->cr3;
        int sandboxed_cr3 = 0;
        vring_t *ring = NULL;
        if (next->pml4_phys) target_cr3 = next->pml4_phys;
        if (next->vring_minor != 0) {
            ring = vring_get(next->vring_minor);
            if (ring && ring->vring_pml4 == target_cr3) sandboxed_cr3 = 1;
        }
        if (sandboxed_cr3) {
            kernel_irq_cr3 = kernel_cr3;
        } else {
            kernel_irq_cr3 = 0;
        }
        return_frame->entry_cr3 = target_cr3;
        return_frame->return_cr3 = target_cr3;
        if (target_cr3 && target_cr3 != read_cr3()) {
            if (!sandboxed_cr3) {
                uint64_t check_virt;
                uint64_t *check_tbl;
                uint64_t entry511;
                uint64_t pdpt_phys;
                uint64_t pdpt510;
                uint64_t pd_phys;
                uint64_t pd0;
                extern uint64_t boot_pdpt_high[] __attribute__((aligned(4096)));
                uint64_t expected;
                check_virt = TEMP_SLOT(0);
                expected = ((uint64_t)(uintptr_t)boot_pdpt_high & ~0xFFFULL) | 3;
                temp_map_raw(check_virt, target_cr3);
                check_tbl = (uint64_t *)check_virt;
                entry511 = check_tbl[511];
                temp_unmap_raw(check_virt);
                if ((entry511 & 0xFFFFFFFFF000ULL) != (expected & 0xFFFFFFFFF000ULL)) {
                    printf("SCHED: PML4[511] BAD pid=%d pml4=0x%lX got=0x%lX want=0x%lX\n",
                           next->id, target_cr3, entry511, expected);
                    __asm__ volatile ("cli; hlt");
                } else {
                    pdpt_phys = entry511 & 0xFFFFFFFFF000ULL;
                    temp_map_raw(check_virt, pdpt_phys);
                    check_tbl = (uint64_t *)check_virt;
                    pdpt510 = check_tbl[510];
                    temp_unmap_raw(check_virt);
                    if (!(pdpt510 & 1)) {
                        printf("SCHED: PDPT[510] NOT PRESENT pid=%d pdpt=0x%lX val=0x%lX\n",
                               next->id, pdpt_phys, pdpt510);
                        __asm__ volatile ("cli; hlt");
                    } else {
                        pd_phys = pdpt510 & 0xFFFFFFFFF000ULL;
                        temp_map_raw(check_virt, pd_phys);
                        check_tbl = (uint64_t *)check_virt;
                        pd0 = check_tbl[0];
                        temp_unmap_raw(check_virt);
                        if (!(pd0 & 1)) {
                            printf("SCHED: PD[0] NOT PRESENT pid=%d pd=0x%lX val=0x%lX\n",
                                   next->id, pd_phys, pd0);
                            __asm__ volatile ("cli; hlt");
                        }
                    }
                }
            }
        }
    }

    if (!must_switch && (reap_pending || exec_drain_pending)) {
        run_deferred = 1;
    }

    spin_unlock(&sched_lock);
    if (run_deferred) {
        task_deferred_work();
    }
    return return_frame;

out_restore_cr3:
    regs->return_cr3 = entry_cr3;
    if ((reap_pending || exec_drain_pending) && (!current_task || current_task->state != TASK_DEAD)) {
        run_deferred = 1;
    }
    spin_unlock(&sched_lock);
    if (run_deferred) {
        task_deferred_work();
    }
    return result;
}

void set_syscall_frame(registers_t *frame) {
    if (current_task) {
        current_task->syscall_frame = frame;
    }
}

void clear_syscall_frame(void) {
    if (current_task) {
        current_task->syscall_frame = NULL;
    }
}

void task_init_fds(task_t *task) {
    if (!task) return;
    task->fds = (task_fd_t *)kmalloc(TASK_INIT_FDS * sizeof(task_fd_t));
    if (!task->fds) {
        task->fds_capacity = 0;
        return;
    }
    task->fds_capacity = TASK_INIT_FDS;
    memset(task->fds, 0, TASK_INIT_FDS * sizeof(task_fd_t));
    task->fds[0].in_use = 1;
    task->fds[0].type = FD_TYPE_STDIN;
    task->fds[0].ref_count = 1;
    task->fds[1].in_use = 1;
    task->fds[1].type = FD_TYPE_STDOUT;
    task->fds[1].ref_count = 1;
    task->fds[2].in_use = 1;
    task->fds[2].type = FD_TYPE_STDERR;
    task->fds[2].ref_count = 1;
}

int task_fd_alloc(task_t *task) {
    int i;
    int ret;

    if (!task || !task->fds) return -1;
    for (i = 3; i < task->fds_capacity; i++) {
        if (!task->fds[i].in_use) {
            memset(&task->fds[i], 0, sizeof(task_fd_t));
            task->fds[i].in_use = 1;
            task->fds[i].ref_count = 1;
            return i;
        }
    }
    ret = task_fd_ensure_capacity(task, task->fds_capacity);
    if (ret != 0) return -1;
    for (i = 3; i < task->fds_capacity; i++) {
        if (!task->fds[i].in_use) {
            memset(&task->fds[i], 0, sizeof(task_fd_t));
            task->fds[i].in_use = 1;
            task->fds[i].ref_count = 1;
            return i;
        }
    }
    return -1;
}

int task_fd_ensure_capacity(task_t *task, int min_fd) {
    task_fd_t *new_fds;
    int old_cap;
    int new_cap;

    if (!task || !task->fds) return -1;
    if (min_fd < 0) return -1;
    if (min_fd >= TASK_MAX_FDS) return -1;
    if (min_fd < task->fds_capacity) return 0;
    old_cap = task->fds_capacity;
    new_cap = old_cap;
    if (new_cap < TASK_INIT_FDS) new_cap = TASK_INIT_FDS;
    while (new_cap <= min_fd) {
        if (new_cap >= TASK_MAX_FDS) return -1;
        new_cap *= 2;
        if (new_cap > TASK_MAX_FDS) new_cap = TASK_MAX_FDS;
    }
    new_fds = (task_fd_t *)krealloc(task->fds, new_cap * sizeof(task_fd_t));
    if (!new_fds) return -1;
    memset(&new_fds[old_cap], 0, (new_cap - old_cap) * sizeof(task_fd_t));
    task->fds = new_fds;
    task->fds_capacity = new_cap;
    return 0;
}

void task_fd_reclaim_unused(task_t *task) {
    task_fd_t *new_fds;
    int new_cap;
    int i;

    if (!task || !task->fds) return;
    if (task->fds_capacity <= TASK_INIT_FDS) return;
    new_cap = TASK_INIT_FDS;
    for (i = task->fds_capacity - 1; i >= TASK_INIT_FDS; i--) {
        if (task->fds[i].in_use) {
            new_cap = i + 1;
            break;
        }
    }
    if (new_cap >= task->fds_capacity) return;
    new_fds = (task_fd_t *)kmalloc(new_cap * sizeof(task_fd_t));
    if (!new_fds) return;
    memcpy(new_fds, task->fds, new_cap * sizeof(task_fd_t));
    kfree(task->fds);
    task->fds = new_fds;
    task->fds_capacity = new_cap;
}

void task_fd_free(task_t *task, int fd) {
    if (!task || !task->fds || fd < 0 || fd >= task->fds_capacity) return;
    if (!task->fds[fd].in_use) return;
    memset(&task->fds[fd], 0, sizeof(task_fd_t));
    task_fd_reclaim_unused(task);
}

task_fd_t *task_fd_get(task_t *task, int fd) {
    if (!task || !task->fds || fd < 0 || fd >= task->fds_capacity) return NULL;
    if (!task->fds[fd].in_use) return NULL;
    return &task->fds[fd];
}

void task_fd_close_all(task_t *task) {
    int i;
    task_fd_t *tfd;
    pipe_t *p;
    
    if (!task || !task->fds) return;
    for (i = 0; i < task->fds_capacity; i++) {
        tfd = &task->fds[i];
        if (!tfd->in_use) continue;
        if (tfd->type == FD_TYPE_PIPE_R || tfd->type == FD_TYPE_PIPE_W) {
            p = (pipe_t *)tfd->private_data;
            if (p) {
                if (tfd->type == FD_TYPE_PIPE_R) p->readers--;
                else p->writers--;
                waitq_wake_all(&p->read_waitq);
                waitq_wake_all(&p->write_waitq);
                if (p->readers <= 0 && p->writers <= 0) {
                    if (p->buffer) kfree(p->buffer);
                    kfree(p);
                }
            }
        } else if (tfd->type == FD_TYPE_FILE && tfd->node) {
            vfs_close((vfs_node_t *)tfd->node);
        }
        tfd->in_use = 0;
        tfd->ref_count = 0;
        tfd->node = NULL;
        tfd->offset = 0;
        tfd->flags = 0;
        tfd->private_data = NULL;
    }
    task_fd_reclaim_unused(task);
}

void task_fd_close_cloexec(task_t *task) {
    int i;
    task_fd_t *tfd;
    pipe_t *p;

    if (!task || !task->fds) return;
    for (i = 3; i < task->fds_capacity; i++) {
        tfd = &task->fds[i];
        if (!tfd->in_use) continue;
        if (!(tfd->flags & 1)) continue;
        if (tfd->type == FD_TYPE_PIPE_R || tfd->type == FD_TYPE_PIPE_W) {
            p = (pipe_t *)tfd->private_data;
            if (p) {
                if (tfd->type == FD_TYPE_PIPE_R) p->readers--;
                else p->writers--;
                waitq_wake_all(&p->read_waitq);
                waitq_wake_all(&p->write_waitq);
                if (p->readers <= 0 && p->writers <= 0) {
                    if (p->buffer) kfree(p->buffer);
                    kfree(p);
                }
            }
        } else if (tfd->type == FD_TYPE_FILE && tfd->node) {
            vfs_close((vfs_node_t *)tfd->node);
        }
        tfd->in_use = 0;
        tfd->ref_count = 0;
        tfd->node = NULL;
        tfd->offset = 0;
        tfd->flags = 0;
        tfd->private_data = NULL;
    }
    task_fd_reclaim_unused(task);
}

#define FORK_MIN_FREE_PAGES 64

pid_t task_fork(registers_t *parent_regs) {
    uint64_t free_pages;
    uint64_t needed_pages;
    uint64_t *child_user_pages;
    uint64_t child_user_pages_count;
    uint64_t child_pd;
    uint8_t* kernel_stack_base;
    uint64_t i;
    int parent_cap;
    task_t* child;
    registers_t *child_frame;

    if (!current_task || !current_task->is_user) {
        printf("task_fork: can only fork user tasks\n");
        return -1;
    }

    free_pages = pfa_count_free();
    needed_pages = current_task->user_pages_count + FORK_MIN_FREE_PAGES;
    if (free_pages < needed_pages) {
        printf("task_fork: insufficient memory (free=%u, need~%u)\n", free_pages, needed_pages);
        return -1;
    }

    child_user_pages = NULL;
    child_user_pages_count = 0;

    child_pd = vmm_clone_pml4(current_task->pml4_phys, &child_user_pages, &child_user_pages_count);
    if (!child_pd) {
        printf("task_fork: failed to clone page directory\n");
        return -1;
    }

    DEBUG_TASK("task_fork: creating child task with cloned pd=0x%016lX\n", child_pd);

    child = (task_t*)kmalloc(sizeof(task_t));
    kernel_stack_base = kstack_alloc();
    if (!child || !kernel_stack_base) {
        printf("task_fork: allocation failed\n");
        if (child) kfree(child);
        if (kernel_stack_base) kstack_free(kernel_stack_base);
        if (child_user_pages) {
            kfree(child_user_pages);
        }
        vmm_free_pml4(child_pd);
        return -1;
    }
    memset(child, 0, sizeof(task_t));
    if (task_copy_cwd(child, current_task) != 0) {
        kfree(child);
        kstack_free(kernel_stack_base);
        if (child_user_pages) kfree(child_user_pages);
        vmm_free_pml4(child_pd);
        return -KERR_ENOMEM;
    }
    if (task_init_fpu_state(child) != 0) {
        printf("task_fork: fpu allocation failed\n");
        task_free_cwd(child);
        kfree(child);
        kstack_free(kernel_stack_base);
        if (child_user_pages) {
            kfree(child_user_pages);
        }
        vmm_free_pml4(child_pd);
        return -1;
    }
    fpu_save_area(current_task->fpu_state);
    memcpy(child->fpu_state, current_task->fpu_state, FPU_STATE_SIZE);

    child->id = next_task_id;
    child->pid = next_task_id;
    next_task_id++;

    if (creds_copy_task(current_task, child) != 0) {
        task_free_fpu_state(child);
        task_free_cwd(child);
        kfree(child);
        kstack_free(kernel_stack_base);
        if (child_user_pages) kfree(child_user_pages);
        vmm_free_pml4(child_pd);
        return -KERR_ENOMEM;
    }
    signals_init_task(child);

    child->ppid = current_task->pid;
    child->pgid = current_task->pgid ? current_task->pgid : current_task->pid;
    child->sid = current_task->sid ? current_task->sid : current_task->pid;

    child->start_tick = tick_count;
    child->state = TASK_READY;
    child->next = NULL;
    child->cr3 = child_pd;
    child->time_slice = SCHED_DEFAULT_TIMESLICE;
    child->base_time_slice = SCHED_DEFAULT_TIMESLICE;
    child->stack_base = NULL;
    child->stack_size = 0;
    child->kernel_stack_base = kernel_stack_base;
    child->kernel_stack_size = KSTACK_USABLE_SIZE;
    child->wake_tick = 0;
    child->sleep_next = NULL;
    child->in_sleep_queue = 0;
    child->in_wait_queue = 0;
    child->wait_next = NULL;
    child->waiting_queue = NULL;
    child->join_target = NULL;
    waitq_init(&child->join_waiters);
    child->join_refs = 0;
    child->exit_code = 0;
    child->is_user = true;
    child->syscall_frame = NULL;
    child->pml4_phys = child_pd;
    child->user_pages = child_user_pages;
    child->user_pages_count = child_user_pages_count;
    child->file_map_count = current_task->file_map_count;
    child->file_map_capacity = 0;
    child->file_maps = NULL;
    if (child->file_map_count > 0) {
        if (task_ensure_file_map_capacity(child, child->file_map_count) != 0) {
            task_free_fpu_state(child);
            task_free_cwd(child);
            kfree(child);
            kstack_free(kernel_stack_base);
            if (child_user_pages)
                kfree(child_user_pages);
            vmm_free_pml4(child_pd);
            return -KERR_ENOMEM;
        }
    }
    for (i = 0; i < (uint64_t)child->file_map_count; i++) {
        child->file_maps[i] = current_task->file_maps[i];
        if (child->file_maps[i].node) {
            vfs_open(child->file_maps[i].node, 0);
        }
    }
    child->user_brk = current_task->user_brk;
    child->user_brk_start = current_task->user_brk_start;
    child->console_id = current_task->console_id;
    child->tls_base = current_task->tls_base;
    child->tls_limit = current_task->tls_limit;
    child->waiting_for_any_child = 0;
    memcpy(child->name, current_task->name, sizeof(child->name));
    parent_cap = current_task->fds_capacity;
    if (parent_cap < TASK_INIT_FDS) parent_cap = TASK_INIT_FDS;
    child->fds = (task_fd_t *)kmalloc(parent_cap * sizeof(task_fd_t));
    if (!child->fds) {
        task_clear_file_mappings(child);
        task_free_fpu_state(child);
        task_free_cwd(child);
        kfree(child);
        kstack_free(kernel_stack_base);
        if (child_user_pages)
            kfree(child_user_pages);
        vmm_free_pml4(child_pd);
        return -KERR_ENOMEM;
    }
    child->fds_capacity = parent_cap;
    memset(child->fds, 0, parent_cap * sizeof(task_fd_t));
    if (current_task->fds && current_task->fds_capacity > 0)
        memcpy(child->fds, current_task->fds, current_task->fds_capacity * sizeof(task_fd_t));
    for (i = 0; i < (uint64_t)child->fds_capacity; i++) {
        if (child->fds[i].in_use && child->fds[i].ref_count > 0) {
            child->fds[i].ref_count++;
        }
        if (child->fds[i].in_use && child->fds[i].type == FD_TYPE_FILE && child->fds[i].node) {
            vfs_open((vfs_node_t *)child->fds[i].node, child->fds[i].flags);
        }
        if (child->fds[i].in_use && (child->fds[i].type == FD_TYPE_PIPE_R || child->fds[i].type == FD_TYPE_PIPE_W) && child->fds[i].private_data) {
            pipe_t *p = (pipe_t *)child->fds[i].private_data;
            if (child->fds[i].type == FD_TYPE_PIPE_R) p->readers++;
            else p->writers++;
        }
    }
    child->envp = NULL;
    child->envc = 0;

    child_frame = (registers_t *)((uint8_t *)kernel_stack_base + KSTACK_USABLE_SIZE - sizeof(registers_t));
    memcpy(child_frame, parent_regs, sizeof(registers_t));
    child_frame->rax = 0;
    child_frame->int_no = 0;
    child_frame->err_code = 0;

    child->regs.rsp = (uint64_t)child_frame;
    child->regs.rip = child_frame->rip;
    child->regs.rflags = child_frame->rflags;
    child->regs.cs = child_frame->cs;
    child->regs.ds = child->regs.es = child_frame->ds;

    lock_scheduler();
    child->all_next = all_tasks_head;
    all_tasks_head = child;
    add_task_to_runqueue(child);
    unlock_scheduler();

    DEBUG_TASK("task_fork: parent pid=%d, child pid=%d\n", current_task->pid, child->pid);

    return child->pid;
}

int task_exec(const uint8_t *bin_start, uint64_t bin_size, registers_t *regs) {
    uint64_t free_pages;
    uint64_t needed_estimate;
    uint8_t *kernel_bin;
    int elf_valid;
    uint64_t old_pd;
    uint64_t *old_user_pages;
    uint64_t old_user_pages_count;
    uint64_t stack_top;
    uint64_t stack_size;
    uint64_t new_pd;
    uint64_t *elf_pages;
    uint64_t elf_page_count;
    int load_result;
    uint64_t stack_page_count;
    uint64_t *stack_pages;
    uint64_t total_pages;
    uint64_t sp;
    uint64_t zero;
    const char *prog_name;
    int prog_len;
    uint64_t prog_addr;
    uint8_t random_bytes[16];
    uint64_t random_addr;
    uint64_t argc_val;
    elf_info_t elf_info;

    if (!current_task || !current_task->is_user) {
        task_error("task_exec: can only exec in user tasks\n");
        return -1;
    }
    if (!bin_start || bin_size == 0) {
        task_error("task_exec: invalid binary\n");
        return -1;
    }

    free_pages = pfa_count_free();
    needed_estimate = 20 + (bin_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (free_pages < needed_estimate) {
        task_error("task_exec: insufficient memory (free=%u need=%u)\n", free_pages, needed_estimate);
        return -1;
    }

    kernel_bin = (uint8_t *)kmalloc(bin_size);
    if (!kernel_bin) {
        task_error("task_exec: failed to allocate kernel buffer (%u bytes)\n", bin_size);
        return -1;
    }
    memcpy(kernel_bin, bin_start, bin_size);

    elf_valid = elf_validate(kernel_bin, bin_size);
    if (elf_valid != 0) {
        task_error("task_exec: ELF validation failed (%d)\n", elf_valid);
        kfree(kernel_bin);
        return -1;
    }

    DEBUG_TASK("task_exec: replacing task %d with ELF binary (%u bytes)\n", current_task->pid, bin_size);

    task_fd_close_cloexec(current_task);

    old_pd = current_task->pml4_phys;
    old_user_pages = current_task->user_pages;
    old_user_pages_count = current_task->user_pages_count;
    task_clear_file_mappings(current_task);

    stack_top = USER_STACK_TOP;
    stack_size = task_initial_stack_size(0, NULL, 0, NULL, "program", 11);

    new_pd = vmm_create_pml4();
    if (!new_pd) {
        task_error("task_exec: failed to create page directory\n");
        kfree(kernel_bin);
        return -1;
    }

    elf_pages = NULL;
    elf_page_count = 0;

    load_result = elf_load_to_pd(new_pd, kernel_bin, bin_size, &elf_info, &elf_pages, &elf_page_count);
    if (load_result != 0) {
        task_error("task_exec: ELF loading failed (%d)\n", load_result);
        if (elf_pages) kfree(elf_pages);
        vmm_free_pml4(new_pd);
        kfree(kernel_bin);
        return -1;
    }

    kfree(kernel_bin);

    stack_page_count = 0;
    stack_pages = vmm_map_range_in_pml4_tracked(new_pd, stack_top - USER_STACK_GAP - stack_size, stack_size, 0x7, &stack_page_count);
    if (!stack_pages) {
        task_error("task_exec: failed to map stack\n");
        if (elf_pages) kfree(elf_pages);
        vmm_free_pml4(new_pd);
        return -1;
    }

    current_task->user_brk = (elf_info.bss_end + 0xFFF) & ~0xFFFu;
    current_task->user_brk_start = current_task->user_brk;

    total_pages = elf_page_count + stack_page_count;
    if (total_pages == 0 || total_pages > 65536) {
        task_error("task_exec: suspicious total_pages=%u\n", total_pages);
        kfree(elf_pages);
        kfree(stack_pages);
        vmm_free_pml4(new_pd);
        return -1;
    }

    current_task->user_pages = (uint64_t *)kmalloc(total_pages * sizeof(uint64_t));
    if (current_task->user_pages) {
        memcpy(current_task->user_pages, elf_pages, elf_page_count * sizeof(uint64_t));
        memcpy(current_task->user_pages + elf_page_count, stack_pages, stack_page_count * sizeof(uint64_t));
        current_task->user_pages_count = total_pages;
    } else {
        current_task->user_pages_count = 0;
    }

    kfree(elf_pages);
    kfree(stack_pages);

    current_task->tls_base = 0;
    current_task->tls_limit = 0;
    task_write_fs_base(0);
    current_task->stack_size = stack_size;
    task_reset_fpu_state(current_task);

    sp = stack_top - USER_STACK_GAP - 16;
    zero = 0;
    
    prog_name = "program";
    prog_len = 0;
    while (prog_name[prog_len]) prog_len++;
    sp -= (prog_len + 1 + 7) & ~7;
    prog_addr = sp;
    vmm_copy_to_pml4(new_pd, sp, prog_name, prog_len + 1);
    
    rng_fill(random_bytes, sizeof(random_bytes));
    sp -= 16;
    random_addr = sp;
    vmm_copy_to_pml4(new_pd, sp, random_bytes, 16);
    
    sp = sp & ~0xF;
    
#define AT_NULL         0
#define AT_PHDR         3
#define AT_PHENT        4
#define AT_PHNUM        5
#define AT_PAGESZ       6
#define AT_ENTRY        9
#define AT_UID          11
#define AT_EUID         12
#define AT_GID          13
#define AT_EGID         14
#define AT_RANDOM       25

#define PUSH_AUXV_PD(pd, type, val) do { \
    uint64_t _t = (type), _v = (val); \
    sp -= 8; vmm_copy_to_pml4(pd, sp, &_v, sizeof(uint64_t)); \
    sp -= 8; vmm_copy_to_pml4(pd, sp, &_t, sizeof(uint64_t)); \
} while(0)

    PUSH_AUXV_PD(new_pd, AT_NULL, 0);
    PUSH_AUXV_PD(new_pd, AT_RANDOM, random_addr);
    PUSH_AUXV_PD(new_pd, AT_EGID, current_task->egid);
    PUSH_AUXV_PD(new_pd, AT_GID, current_task->gid);
    PUSH_AUXV_PD(new_pd, AT_EUID, current_task->euid);
    PUSH_AUXV_PD(new_pd, AT_UID, current_task->uid);
    PUSH_AUXV_PD(new_pd, AT_PAGESZ, 4096);
    PUSH_AUXV_PD(new_pd, AT_ENTRY, elf_info.entry_point);
    PUSH_AUXV_PD(new_pd, AT_PHNUM, elf_info.phnum);
    PUSH_AUXV_PD(new_pd, AT_PHENT, elf_info.phent);
    PUSH_AUXV_PD(new_pd, AT_PHDR, elf_info.phdr_vaddr);

#undef PUSH_AUXV_PD
#undef AT_NULL
#undef AT_PHDR
#undef AT_PHENT
#undef AT_PHNUM
#undef AT_PAGESZ
#undef AT_ENTRY
#undef AT_UID
#undef AT_EUID
#undef AT_GID
#undef AT_EGID
#undef AT_RANDOM
    
    sp -= 8;
    vmm_copy_to_pml4(new_pd, sp, &zero, sizeof(uint64_t));
    
    sp -= 8;
    vmm_copy_to_pml4(new_pd, sp, &prog_addr, sizeof(uint64_t));
    
    sp -= 8;
    argc_val = 1;
    vmm_copy_to_pml4(new_pd, sp, &argc_val, sizeof(uint64_t));

    {
        uint64_t final_entry;
        uint64_t final_sp;
        
        final_entry = elf_info.entry_point;
        final_sp = sp;

        current_task->pml4_phys = new_pd;
        current_task->cr3 = new_pd;
        regs->entry_cr3 = new_pd;
        regs->return_cr3 = new_pd;
        regs->saved_entry_cr3 = new_pd;

        regs->ds = 0x23;
        regs->es = 0x23;
        regs->ss = 0x23;
        regs->cs = 0x1B;
        regs->rax = 0;
        regs->rbx = 0;
        regs->rcx = 0;
        regs->rdx = 0;
        regs->rsi = 0;
        regs->rdi = 0;
        regs->rbp = 0;
        regs->r8 = 0;
        regs->r9 = 0;
        regs->r10 = 0;
        regs->r11 = 0;
        regs->r12 = 0;
        regs->r13 = 0;
        regs->r14 = 0;
        regs->r15 = 0;
        regs->rflags = 0x202;
        regs->rsp = final_sp;
        regs->int_no = 0;
        regs->err_code = 0;
        regs->rip = final_entry;

        current_task->regs.rip = final_entry;
        current_task->regs.rsp = final_sp;
        current_task->regs.rbp = 0;
        current_task->regs.rax = 0;
        current_task->regs.rbx = 0;
        current_task->regs.rcx = 0;
        current_task->regs.rdx = 0;
        current_task->regs.rsi = 0;
        current_task->regs.rdi = 0;
        current_task->regs.cs = 0x1B;
        current_task->regs.ds = 0x23;
        current_task->regs.es = 0x23;
        current_task->regs.ss = 0x23;
        current_task->regs.rflags = 0x202;
        current_task->regs.int_no = 0;
        current_task->regs.err_code = 0;
        current_task->regs.rsp = (uint64_t)regs;
        current_task->regs.entry_cr3 = new_pd;
        current_task->regs.return_cr3 = new_pd;
        current_task->regs.saved_entry_cr3 = new_pd;

        task_discard_exec_old(current_task);
        current_task->exec_old_pml4 = old_pd;
        current_task->exec_old_pages = old_user_pages;
        current_task->exec_old_pages_count = old_user_pages_count;

        DEBUG_TASK("task_exec: new ELF entry at 0x%016lX, stack at 0x%016lX new_pd=0x%016lX (CR3 switch deferred)\n", 
                 final_entry, final_sp, new_pd);
    }

    return 0;
}

static int task_exec_with_args_common(vfs_node_t *bin_node, const uint8_t *bin_start, uint64_t bin_size, registers_t *regs,
                                      int argc, char **argv, int envc, char **envp) {
    uint64_t free_pages;
    uint64_t needed_estimate;
    uint8_t *kernel_bin;
    char **k_argv;
    char **k_envp;
    int i, j, len;
    int elf_valid;
    uint64_t old_pd;
    uint64_t *old_user_pages;
    uint64_t old_user_pages_count;
    uint64_t stack_top;
    uint64_t stack_size;
    uint64_t new_pd;
    uint64_t *elf_pages;
    uint64_t elf_page_count;
    int load_result;
    uint64_t stack_page_count;
    uint64_t *stack_pages;
    uint64_t total_pages;
    uint64_t sp;
    uint64_t *envp_ptrs;
    uint64_t *argv_ptrs;
    uint8_t random_bytes[16];
    uint64_t random_addr;
    uint64_t entry_to_use;
    uint64_t stack_ptr;
    uint64_t check_stack_base;
    uint64_t check_stack_top;
    uint64_t final_entry;
    uint64_t final_sp;
    const char *base;
    int n;
    elf_info_t elf_info;
    int use_node;
    int held_node;
    uint64_t tbl_stack[64];
    uint64_t *tbl_buf;
    int tbl_idx;
    uint64_t tbl_bytes;
    int tbl_cap;
    int tbl_heap;

    __asm__ volatile ("mov %%rsp, %0" : "=r"(stack_ptr));
    if (current_task && current_task->kernel_stack_base) {
        check_stack_base = (uint64_t)current_task->kernel_stack_base;
        check_stack_top = check_stack_base + current_task->kernel_stack_size;
        if (stack_ptr < check_stack_base || stack_ptr > check_stack_top) {
            DEBUG_TASK("task_exec_with_args: STACK OVERFLOW rsp=0x%016lX base=0x%016lX top=0x%016lX\n",
                     stack_ptr, check_stack_base, check_stack_top);
        }
    }

    if (!current_task || !current_task->is_user) {
        task_error("task_exec_with_args: can only exec in user tasks\n");
        if (bin_start) kfree((void *)bin_start);
        return -KERR_EPERM;
    }
    use_node = (bin_node != NULL) ? 1 : 0;
    held_node = 0;
    if (use_node) {
        bin_size = bin_node->length;
    }
    if ((!use_node && !bin_start) || bin_size == 0) {
        task_error("task_exec_with_args: invalid binary\n");
        if (bin_start) kfree((void *)bin_start);
        return -KERR_ENOEXEC;
    }

    free_pages = pfa_count_free();
    needed_estimate = 20 + (bin_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (free_pages < needed_estimate) {
        task_error("task_exec_with_args: insufficient memory\n");
        kfree((void *)bin_start);
        return -KERR_ENOMEM;
    }

    kernel_bin = (uint8_t *)bin_start;

    k_argv = NULL;
    k_envp = NULL;

    if (argc > 0 && argv) {
            k_argv = (char **)kmalloc((argc + 1) * sizeof(char *));
            if (!k_argv) {
                if (kernel_bin) kfree(kernel_bin);
                return -KERR_ENOMEM;
            }
            for (i = 0; i < argc; i++) {
                len = strlen(argv[i]);
                k_argv[i] = (char *)kmalloc(len + 1);
                if (!k_argv[i]) {
                    for (j = 0; j < i; j++) kfree(k_argv[j]);
                    kfree(k_argv);
                    if (kernel_bin) kfree(kernel_bin);
                    return -KERR_ENOMEM;
                }
                memcpy(k_argv[i], argv[i], len + 1);
            }
            k_argv[argc] = NULL;
        }

        if (envc > 0 && envp) {
            k_envp = (char **)kmalloc((envc + 1) * sizeof(char *));
            if (!k_envp) {
                if (k_argv) {
                    for (i = 0; i < argc; i++) kfree(k_argv[i]);
                    kfree(k_argv);
                }
                if (kernel_bin) kfree(kernel_bin);
                return -KERR_ENOMEM;
            }
            for (i = 0; i < envc; i++) {
                len = strlen(envp[i]);
                k_envp[i] = (char *)kmalloc(len + 1);
                if (!k_envp[i]) {
                    for (j = 0; j < i; j++) kfree(k_envp[j]);
                    kfree(k_envp);
                    if (k_argv) {
                        for (j = 0; j < argc; j++) kfree(k_argv[j]);
                        kfree(k_argv);
                    }
                    if (kernel_bin) kfree(kernel_bin);
                    return -KERR_ENOMEM;
                }
                memcpy(k_envp[i], envp[i], len + 1);
            }
            k_envp[envc] = NULL;
        }

    elf_valid = 0;
    if (!use_node) {
        elf_valid = elf_validate(kernel_bin, bin_size);
    }
    if (elf_valid != 0) {
        task_error("task_exec_with_args: ELF validation failed code=%d size=%llu magic=%02x%02x%02x%02x type=%04x\n",
                   elf_valid, (unsigned long long)bin_size,
                   kernel_bin[0], kernel_bin[1], kernel_bin[2], kernel_bin[3],
                   (unsigned)((const Elf64_Ehdr *)kernel_bin)->e_type);
        if (k_argv) {
            for (i = 0; i < argc; i++) kfree(k_argv[i]);
            kfree(k_argv);
        }
        if (k_envp) {
            for (i = 0; i < envc; i++) kfree(k_envp[i]);
            kfree(k_envp);
        }
        if (kernel_bin) kfree(kernel_bin);
        return -KERR_ENOEXEC;
    }

    stack_top = USER_STACK_TOP;
    stack_size = task_initial_stack_size(argc, k_argv, envc, k_envp, "program", 11);
    if (stack_size > USER_STACK_SIZE) {
        task_error("task_exec_with_args: initial stack too large\n");
        if (k_argv) {
            for (i = 0; i < argc; i++) kfree(k_argv[i]);
            kfree(k_argv);
        }
        if (k_envp) {
            for (i = 0; i < envc; i++) kfree(k_envp[i]);
            kfree(k_envp);
        }
        if (kernel_bin) kfree(kernel_bin);
        return -KERR_ENOMEM;
    }

    task_fd_close_cloexec(current_task);

    old_pd = current_task->pml4_phys;
    old_user_pages = current_task->user_pages;
    old_user_pages_count = current_task->user_pages_count;
    task_clear_file_mappings(current_task);

    new_pd = vmm_create_pml4();
    if (!new_pd) {
        task_error("task_exec_with_args: failed to create page directory\n");
        if (k_argv) {
            for (i = 0; i < argc; i++) kfree(k_argv[i]);
            kfree(k_argv);
        }
        if (k_envp) {
            for (i = 0; i < envc; i++) kfree(k_envp[i]);
            kfree(k_envp);
        }
        if (kernel_bin) kfree(kernel_bin);
        return -KERR_ENOMEM;
    }

    if (use_node) {
        vfs_open(bin_node, 0);
        held_node = 1;
    }

    elf_pages = NULL;
    elf_page_count = 0;
    if (use_node) {
        load_result = elf_load_node_to_pd(new_pd, bin_node, &elf_info, &elf_pages, &elf_page_count);
    } else {
        load_result = elf_load_to_pd(new_pd, kernel_bin, bin_size, &elf_info, &elf_pages, &elf_page_count);
    }
    if (held_node) {
        vfs_close(bin_node);
        held_node = 0;
    }
    if (load_result != 0) {
        task_error("task_exec_with_args: ELF loading failed code=%d\n", load_result);
        task_clear_file_mappings(current_task);
        if (elf_pages) kfree(elf_pages);
        vmm_free_pml4(new_pd);
        if (k_argv) {
            for (i = 0; i < argc; i++) kfree(k_argv[i]);
            kfree(k_argv);
        }
        if (k_envp) {
            for (i = 0; i < envc; i++) kfree(k_envp[i]);
            kfree(k_envp);
        }
        if (kernel_bin) kfree(kernel_bin);
        return -KERR_ENOEXEC;
    }

    if (!use_node) {
        uint64_t entry_phys;
        uint64_t entry_page_addr;

        entry_page_addr = elf_info.entry_point & ~0xFFFu;
        entry_phys = vmm_get_phys_in_pml4(new_pd, entry_page_addr);
        if (entry_phys == 0) {
            DEBUG_TASK("task_exec_with_args: ERROR: entry page not mapped after elf_load\n");
        }
    }
    if (kernel_bin) kfree(kernel_bin);

    stack_page_count = 0;
    stack_pages = vmm_map_range_in_pml4_tracked(new_pd, stack_top - USER_STACK_GAP - stack_size, stack_size, 0x7, &stack_page_count);
    if (!stack_pages) {
        task_error("task_exec_with_args: failed to map stack\n");
        task_clear_file_mappings(current_task);
        if (elf_pages) kfree(elf_pages);
        vmm_free_pml4(new_pd);
        if (k_argv) {
            for (i = 0; i < argc; i++) kfree(k_argv[i]);
            kfree(k_argv);
        }
        if (k_envp) {
            for (i = 0; i < envc; i++) kfree(k_envp[i]);
            kfree(k_envp);
        }
        return -KERR_ENOMEM;
    }

    current_task->user_brk = (elf_info.bss_end + 0xFFF) & ~0xFFFu;
    current_task->user_brk_start = current_task->user_brk;

    total_pages = elf_page_count + stack_page_count;
    if (total_pages == 0 || total_pages > 65536) {
        task_error("task_exec_with_args: suspicious total_pages=%u\n", total_pages);
        task_clear_file_mappings(current_task);
        kfree(elf_pages);
        kfree(stack_pages);
        if (k_argv) {
            for (i = 0; i < argc; i++) kfree(k_argv[i]);
            kfree(k_argv);
        }
        if (k_envp) {
            for (i = 0; i < envc; i++) kfree(k_envp[i]);
            kfree(k_envp);
        }
        vmm_free_pml4(new_pd);
        return -KERR_ENOEXEC;
    }

    current_task->user_pages = (uint64_t *)kmalloc(total_pages * sizeof(uint64_t));
    if (current_task->user_pages) {
        if (elf_page_count > 0 && elf_pages) {
            memcpy(current_task->user_pages, elf_pages, elf_page_count * sizeof(uint64_t));
        }
        memcpy(current_task->user_pages + elf_page_count, stack_pages, stack_page_count * sizeof(uint64_t));
        current_task->user_pages_count = total_pages;
    } else {
        current_task->user_pages_count = 0;
    }

    kfree(elf_pages);
    kfree(stack_pages);
    current_task->stack_size = stack_size;

    sp = stack_top - USER_STACK_GAP - 16;

    envp_ptrs = NULL;
    argv_ptrs = NULL;
    tbl_buf = NULL;
    tbl_idx = 0;
    tbl_bytes = 0;
    tbl_cap = argc + envc + 32;
    tbl_heap = 0;

    if (envc > 0 && k_envp) {
        envp_ptrs = (uint64_t *)kmalloc((envc + 1) * sizeof(uint64_t));
        if (!envp_ptrs) {
            task_clear_file_mappings(current_task);
            if (current_task->user_pages) {
                kfree(current_task->user_pages);
            }
            current_task->user_pages = old_user_pages;
            current_task->user_pages_count = old_user_pages_count;
            vmm_free_pml4(new_pd);
            if (k_argv) {
                for (i = 0; i < argc; i++) kfree(k_argv[i]);
                kfree(k_argv);
            }
            if (k_envp) {
                for (i = 0; i < envc; i++) kfree(k_envp[i]);
                kfree(k_envp);
            }
            return -KERR_ENOMEM;
        }
        for (i = envc - 1; i >= 0; i--) {
            len = 0;
            while (k_envp[i][len]) len++;
            sp -= (len + 1 + 7) & ~7;
            vmm_copy_to_pml4(new_pd, sp, k_envp[i], len + 1);
            envp_ptrs[i] = sp;
        }
        envp_ptrs[envc] = 0;
    }

    if (argc > 0 && k_argv) {
        argv_ptrs = (uint64_t *)kmalloc((argc + 1) * sizeof(uint64_t));
        if (!argv_ptrs) {
            task_clear_file_mappings(current_task);
            if (current_task->user_pages) {
                kfree(current_task->user_pages);
            }
            current_task->user_pages = old_user_pages;
            current_task->user_pages_count = old_user_pages_count;
            if (envp_ptrs) kfree(envp_ptrs);
            vmm_free_pml4(new_pd);
            if (k_argv) {
                for (i = 0; i < argc; i++) kfree(k_argv[i]);
                kfree(k_argv);
            }
            if (k_envp) {
                for (i = 0; i < envc; i++) kfree(k_envp[i]);
                kfree(k_envp);
            }
            return -KERR_ENOMEM;
        }
        for (i = argc - 1; i >= 0; i--) {
            len = 0;
            while (k_argv[i][len]) len++;
            sp -= (len + 1 + 7) & ~7;
            vmm_copy_to_pml4(new_pd, sp, k_argv[i], len + 1);
            argv_ptrs[i] = sp;
        }
        argv_ptrs[argc] = 0;
    }

    rng_fill(random_bytes, sizeof(random_bytes));
    sp -= 16;
    random_addr = sp;
    vmm_copy_to_pml4(new_pd, sp, random_bytes, 16);

    sp = sp & ~0xF;

#define AT_NULL         0
#define AT_PHDR         3
#define AT_PHENT        4
#define AT_PHNUM        5
#define AT_PAGESZ       6
#define AT_ENTRY        9
#define AT_UID          11
#define AT_EUID         12
#define AT_GID          13
#define AT_EGID         14
#define AT_RANDOM       25

    if (tbl_cap <= (int)(sizeof(tbl_stack) / sizeof(tbl_stack[0]))) {
        tbl_buf = tbl_stack;
    } else {
        tbl_buf = (uint64_t *)kmalloc((uint64_t)tbl_cap * sizeof(uint64_t));
        tbl_heap = 1;
    }
    if (!tbl_buf) {
        task_clear_file_mappings(current_task);
        if (current_task->user_pages) {
            kfree(current_task->user_pages);
        }
        current_task->user_pages = old_user_pages;
        current_task->user_pages_count = old_user_pages_count;
        if (argv_ptrs) kfree(argv_ptrs);
        if (envp_ptrs) kfree(envp_ptrs);
        vmm_free_pml4(new_pd);
        if (k_argv) {
            for (i = 0; i < argc; i++) kfree(k_argv[i]);
            kfree(k_argv);
        }
        if (k_envp) {
            for (i = 0; i < envc; i++) kfree(k_envp[i]);
            kfree(k_envp);
        }
        return -KERR_ENOMEM;
    }

    tbl_buf[tbl_idx++] = (uint64_t)argc;

    if (argv_ptrs) {
        for (i = 0; i <= argc; i++)
            tbl_buf[tbl_idx++] = argv_ptrs[i];
    } else {
        tbl_buf[tbl_idx++] = 0;
    }

    if (envp_ptrs) {
        for (i = 0; i <= envc; i++)
            tbl_buf[tbl_idx++] = envp_ptrs[i];
    } else {
        tbl_buf[tbl_idx++] = 0;
    }

    if (elf_info.phdr_vaddr != 0 && elf_info.phnum != 0) {
        tbl_buf[tbl_idx++] = AT_PHDR;
        tbl_buf[tbl_idx++] = elf_info.phdr_vaddr;
    }
    tbl_buf[tbl_idx++] = AT_PHENT;
    tbl_buf[tbl_idx++] = elf_info.phent;
    tbl_buf[tbl_idx++] = AT_PHNUM;
    tbl_buf[tbl_idx++] = elf_info.phnum;
    tbl_buf[tbl_idx++] = AT_ENTRY;
    tbl_buf[tbl_idx++] = elf_info.entry_point;
    tbl_buf[tbl_idx++] = AT_PAGESZ;
    tbl_buf[tbl_idx++] = 4096;
    tbl_buf[tbl_idx++] = AT_UID;
    tbl_buf[tbl_idx++] = current_task->uid;
    tbl_buf[tbl_idx++] = AT_EUID;
    tbl_buf[tbl_idx++] = current_task->euid;
    tbl_buf[tbl_idx++] = AT_GID;
    tbl_buf[tbl_idx++] = current_task->gid;
    tbl_buf[tbl_idx++] = AT_EGID;
    tbl_buf[tbl_idx++] = current_task->egid;
    tbl_buf[tbl_idx++] = AT_RANDOM;
    tbl_buf[tbl_idx++] = random_addr;
    tbl_buf[tbl_idx++] = AT_NULL;
    tbl_buf[tbl_idx++] = 0;

    tbl_bytes = (uint64_t)tbl_idx * 8;
    sp -= tbl_bytes;
    vmm_copy_to_pml4(new_pd, sp, tbl_buf, tbl_bytes);
    if (tbl_heap) kfree(tbl_buf);

#undef AT_NULL
#undef AT_PHDR
#undef AT_PHENT
#undef AT_PHNUM
#undef AT_PAGESZ
#undef AT_ENTRY
#undef AT_UID
#undef AT_EUID
#undef AT_GID
#undef AT_EGID
#undef AT_RANDOM

    if (argv_ptrs) kfree(argv_ptrs);
    if (envp_ptrs) kfree(envp_ptrs);

    if (k_argv && k_argv[0]) {
        base = k_argv[0];
        for (i = 0; k_argv[0][i]; i++) {
            if (k_argv[0][i] == '/')
                base = &k_argv[0][i + 1];
        }
        for (n = 0; n < 15 && base[n]; n++)
            current_task->name[n] = base[n];
        current_task->name[n] = '\0';
    }

    if (k_argv) {
        for (i = 0; i < argc; i++) kfree(k_argv[i]);
        kfree(k_argv);
    }
    if (k_envp) {
        for (i = 0; i < envc; i++) kfree(k_envp[i]);
        kfree(k_envp);
    }

    entry_to_use = elf_info.entry_point;

    final_entry = entry_to_use;
    final_sp = sp;

    regs->ds = 0x23;
    regs->es = 0x23;
    regs->ss = 0x23;
    regs->cs = 0x1B;
    regs->rax = 0;
    regs->rbx = 0;
    regs->rcx = 0;
    regs->rdx = 0;
    regs->rsi = 0;
    regs->rdi = 0;
    regs->rbp = 0;
    regs->r8 = 0;
    regs->r9 = 0;
    regs->r10 = 0;
    regs->r11 = 0;
    regs->r12 = 0;
    regs->r13 = 0;
    regs->r14 = 0;
    regs->r15 = 0;
    regs->rflags = 0x202;
    regs->rsp = final_sp;
    regs->int_no = 0;
    regs->err_code = 0;
    regs->rip = final_entry;

    {
        extern void task_reset_signals_on_exec(void);
        task_reset_signals_on_exec();
    }

    current_task->pml4_phys = new_pd;
    current_task->cr3 = new_pd;
    regs->entry_cr3 = new_pd;
    regs->return_cr3 = new_pd;
    regs->saved_entry_cr3 = new_pd;
    current_task->regs.rip = final_entry;
    current_task->regs.rsp = final_sp;
    current_task->regs.rbp = 0;
    current_task->regs.rax = 0;
    current_task->regs.rbx = 0;
    current_task->regs.rcx = 0;
    current_task->regs.rdx = 0;
    current_task->regs.rsi = 0;
    current_task->regs.rdi = 0;
    current_task->regs.cs = 0x1B;
    current_task->regs.ds = 0x23;
    current_task->regs.es = 0x23;
    current_task->regs.ss = 0x23;
    current_task->regs.rflags = 0x202;
    current_task->regs.int_no = 0;
    current_task->regs.err_code = 0;
    current_task->regs.rsp = (uint64_t)regs;
    current_task->regs.entry_cr3 = new_pd;
    current_task->regs.return_cr3 = new_pd;
    current_task->regs.saved_entry_cr3 = new_pd;

    task_discard_exec_old(current_task);
    current_task->exec_old_pml4 = old_pd;
    current_task->exec_old_pages = old_user_pages;
    current_task->exec_old_pages_count = old_user_pages_count;

    current_task->tls_base = 0;
    current_task->tls_limit = 0;
    task_write_fs_base(0);

    DEBUG_TASK("task_exec_with_args: entry=0x%016lX rsp=0x%016lX new_pd=0x%016lX\n", 
             final_entry, final_sp, new_pd);

    if (!use_node) {
        uint64_t entry_page;
        uint64_t phys;

        entry_page = final_entry & ~0xFFFu;
        phys = vmm_get_phys_in_pml4(new_pd, entry_page);
        
        if (phys == 0) {
            DEBUG_TASK("task_exec_with_args: FATAL: entry page not mapped in new_pd\n");
            if (current_task->user_pages) {
                kfree(current_task->user_pages);
            }
            vmm_free_pml4(new_pd);
            current_task->user_pages = old_user_pages;
            current_task->user_pages_count = old_user_pages_count;
            current_task->pml4_phys = old_pd;
            current_task->cr3 = old_pd;
            regs->entry_cr3 = old_pd;
            regs->return_cr3 = old_pd;
            regs->saved_entry_cr3 = old_pd;
            current_task->regs.entry_cr3 = old_pd;
            current_task->regs.return_cr3 = old_pd;
            current_task->regs.saved_entry_cr3 = old_pd;
            current_task->exec_old_pml4 = 0;
            current_task->exec_old_pages = NULL;
            current_task->exec_old_pages_count = 0;
            return -KERR_ENOEXEC;
        }
    }

    return 0;
}

int task_exec_with_args(const uint8_t *bin_start, uint64_t bin_size, registers_t *regs,
                        int argc, char **argv, int envc, char **envp) {
    return task_exec_with_args_common(NULL, bin_start, bin_size, regs, argc, argv, envc, envp);
}

int task_exec_node_with_args(vfs_node_t *node, registers_t *regs,
                             int argc, char **argv, int envc, char **envp) {
    return task_exec_with_args_common(node, NULL, 0, regs, argc, argv, envc, envp);
}

pid_t task_create_thread(void (*entry)(void)) {
    uint64_t thread_stack_size;
    uint64_t thread_stack_base;
    uint64_t thread_stack_top;
    uint64_t stack_page_count;
    uint64_t *stack_pages;
    uint64_t *stack_ptr;
    task_t *new_task;

    if (!current_task || !current_task->is_user) {
        return -1;
    }
    
    new_task = (task_t *)kmalloc(sizeof(task_t));
    if (!new_task) return -1;
    
    memset(new_task, 0, sizeof(task_t));
    if (task_copy_cwd(new_task, current_task) != 0) {
        kfree(new_task);
        return -1;
    }
    if (task_init_fpu_state(new_task) != 0) {
        task_free_cwd(new_task);
        kfree(new_task);
        return -1;
    }
    
    new_task->kernel_stack_base = kstack_alloc();
    if (!new_task->kernel_stack_base) {
        task_free_fpu_state(new_task);
        task_free_cwd(new_task);
        kfree(new_task);
        return -1;
    }
    new_task->kernel_stack_size = KSTACK_USABLE_SIZE;
    
    new_task->id = next_task_id++;
    new_task->pid = new_task->id;
    signals_init_task(new_task);
    new_task->state = TASK_READY;
    new_task->is_user = true;
    new_task->time_slice = SCHED_DEFAULT_TIMESLICE;
    new_task->base_time_slice = SCHED_DEFAULT_TIMESLICE;
    
    new_task->pml4_phys = current_task->pml4_phys;
    new_task->cr3 = current_task->cr3;
    new_task->user_pages = NULL;
    new_task->user_pages_count = 0;
    new_task->file_map_count = 0;
    new_task->user_brk = current_task->user_brk;
    new_task->user_brk_start = current_task->user_brk_start;
    new_task->console_id = current_task->console_id;
    
    thread_stack_size = 0x2000;
    thread_stack_base = (current_task->user_brk + 0xFFF) & ~0xFFF;
    thread_stack_top = thread_stack_base + thread_stack_size;
    
    stack_page_count = 0;
    stack_pages = vmm_map_range_in_pml4_tracked(current_task->pml4_phys, thread_stack_base, thread_stack_size, 0x7, &stack_page_count);
    if (!stack_pages) {
        kstack_free(new_task->kernel_stack_base);
        task_free_fpu_state(new_task);
        task_free_cwd(new_task);
        kfree(new_task);
        return -1;
    }
    
    new_task->user_pages = stack_pages;
    new_task->user_pages_count = stack_page_count;
    
    current_task->user_brk = thread_stack_top + 0x1000;
    
    stack_ptr = (uint64_t *)(new_task->kernel_stack_base + KSTACK_USABLE_SIZE);
    
    stack_ptr--;
    *stack_ptr = 0x23;
    stack_ptr--;
    *stack_ptr = thread_stack_top - 16;
    stack_ptr--;
    *stack_ptr = 0x202;
    stack_ptr--;
    *stack_ptr = 0x1B;
    stack_ptr--;
    *stack_ptr = (uint64_t)entry;
    
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    
    stack_ptr--;
    *stack_ptr = 0x23;
    stack_ptr--;
    *stack_ptr = 0x23;
    stack_ptr--;
    *stack_ptr = new_task->cr3;
    stack_ptr--;
    *stack_ptr = new_task->cr3;
    
    new_task->regs.rsp = (uint64_t)stack_ptr;

    new_task->regs.rip = (uint64_t)entry;
    new_task->regs.cs = 0x1B;
    new_task->regs.ds = new_task->regs.es = new_task->regs.ss = 0x23;
    new_task->regs.rflags = 0x202;
    
    lock_scheduler();
    new_task->all_next = all_tasks_head;
    all_tasks_head = new_task;
    add_task_to_runqueue(new_task);
    unlock_scheduler();
    
    return new_task->pid;
}

pid_t task_create_thread_with_arg(void *(*entry)(void *), void *arg) {
    uint64_t thread_stack_size;
    uint64_t thread_stack_base;
    uint64_t thread_stack_top;
    uint64_t stack_page_count;
    uint64_t *stack_pages;
    uint64_t zero_val;
    uint64_t arg_val;
    uint64_t *stack_ptr;
    task_t *new_task;

    if (!current_task || !current_task->is_user) {
        return -1;
    }
    
    new_task = (task_t *)kmalloc(sizeof(task_t));
    if (!new_task) return -1;
    
    memset(new_task, 0, sizeof(task_t));
    if (task_copy_cwd(new_task, current_task) != 0) {
        kfree(new_task);
        return -1;
    }
    if (task_init_fpu_state(new_task) != 0) {
        task_free_cwd(new_task);
        kfree(new_task);
        return -1;
    }
    
    new_task->kernel_stack_base = kstack_alloc();
    if (!new_task->kernel_stack_base) {
        task_free_fpu_state(new_task);
        task_free_cwd(new_task);
        kfree(new_task);
        return -1;
    }
    new_task->kernel_stack_size = KSTACK_USABLE_SIZE;
    
    new_task->id = next_task_id++;
    new_task->pid = new_task->id;
    signals_init_task(new_task);
    new_task->state = TASK_READY;
    new_task->is_user = true;
    new_task->time_slice = SCHED_DEFAULT_TIMESLICE;
    new_task->base_time_slice = SCHED_DEFAULT_TIMESLICE;
    
    new_task->pml4_phys = current_task->pml4_phys;
    new_task->cr3 = current_task->cr3;
    new_task->user_pages = NULL;
    new_task->user_pages_count = 0;
    new_task->file_map_count = 0;
    new_task->user_brk = current_task->user_brk;
    new_task->user_brk_start = current_task->user_brk_start;
    new_task->console_id = current_task->console_id;
    
    thread_stack_size = 0x2000;
    thread_stack_base = (current_task->user_brk + 0xFFF) & ~0xFFF;
    thread_stack_top = thread_stack_base + thread_stack_size;
    
    stack_page_count = 0;
    stack_pages = vmm_map_range_in_pml4_tracked(current_task->pml4_phys, thread_stack_base, thread_stack_size, 0x7, &stack_page_count);
    if (!stack_pages) {
        kstack_free(new_task->kernel_stack_base);
        task_free_fpu_state(new_task);
        task_free_cwd(new_task);
        kfree(new_task);
        return -1;
    }
    
    new_task->user_pages = stack_pages;
    new_task->user_pages_count = stack_page_count;
    
    current_task->user_brk = thread_stack_top + 0x1000;
    
    zero_val = 0;
    arg_val = (uint64_t)arg;
    vmm_copy_to_pml4(current_task->pml4_phys, thread_stack_top - 16, &zero_val, 8);
    vmm_copy_to_pml4(current_task->pml4_phys, thread_stack_top - 8, &arg_val, 8);
    
    stack_ptr = (uint64_t *)(new_task->kernel_stack_base + KSTACK_USABLE_SIZE);
    
    stack_ptr--;
    *stack_ptr = 0x23;
    stack_ptr--;
    *stack_ptr = thread_stack_top - 16;
    stack_ptr--;
    *stack_ptr = 0x202;
    stack_ptr--;
    *stack_ptr = 0x1B;
    stack_ptr--;
    *stack_ptr = (uint64_t)entry;
    
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    stack_ptr--;
    *stack_ptr = 0;
    
    stack_ptr--;
    *stack_ptr = 0x23;
    stack_ptr--;
    *stack_ptr = 0x23;
    stack_ptr--;
    *stack_ptr = new_task->cr3;
    stack_ptr--;
    *stack_ptr = new_task->cr3;
    
    new_task->regs.rsp = (uint64_t)stack_ptr;

    new_task->regs.rip = (uint64_t)entry;
    new_task->regs.cs = 0x1B;
    new_task->regs.ds = new_task->regs.es = new_task->regs.ss = 0x23;
    new_task->regs.rflags = 0x202;
    
    lock_scheduler();
    new_task->all_next = all_tasks_head;
    all_tasks_head = new_task;
    add_task_to_runqueue(new_task);
    unlock_scheduler();
    
    return new_task->pid;
}

bool task_is_kernel_pid(int32_t pid) {
    return pid < 0;
}

void task_set_vring(task_t *task, uint8_t vring_minor) {
    uint64_t stack_base;
    uint64_t stack_end;
    uint64_t task_stack_base;
    uint64_t task_stack_end;
    vring_t *ring;

    if (!task) return;
    task->vring_minor = vring_minor;
    task->is_kernel_task = (vring_minor != 0);

    if (vring_minor != 0 && !task->is_user && task->kernel_stack_base && task->kernel_stack_size) {
        stack_base = (uint64_t)task->kernel_stack_base;
        stack_end = stack_base + task->kernel_stack_size;
        vring_add_region(vring_minor, stack_base, stack_end, VRING_PERM_READ | VRING_PERM_WRITE);
    }

    if (vring_minor != 0 && !task->is_user && task->stack_base && task->stack_size) {
        task_stack_base = (uint64_t)task->stack_base;
        task_stack_end = task_stack_base + task->stack_size;
        vring_add_region(vring_minor, task_stack_base, task_stack_end, VRING_PERM_READ | VRING_PERM_WRITE);
    }

    if (vring_minor != 0 && !task->is_user) {
        ring = vring_get(vring_minor);
        if (ring && ring->vring_pml4) {
            task->pml4_phys = ring->vring_pml4;
        }
    }
}

static int cached_proc_count = 1;

void task_update_cached_stats(void) {
    int count;
    task_t *t;
    count = 0;

    if (!ready_queue_head) {
        cached_proc_count = 1;
        return;
    }
    t = ready_queue_head;
    do {
        count++;
        t = t->next;
    } while (t && t != ready_queue_head);
    if (count < 1) count = 1;
    cached_proc_count = count;
}

void task_get_cached_stats(int *proc_count, int *unused1, int *unused2, pid_t *unused3) {
    if (proc_count) *proc_count = cached_proc_count;
    (void)unused1;
    (void)unused2;
    (void)unused3;
}
