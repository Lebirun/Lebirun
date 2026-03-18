#include "syscall_defs.h"

extern task_t *current_task;
extern void *syscall_table[];

#define THREAD_INIT_COUNT 16
#define PTHREAD_STACK_SIZE 0x4000

typedef struct {
    int in_use;
    pid_t tid;
    void *stack;
    uint64_t stack_size;
    void *retval;
    int detached;
    int exited;
    wait_queue_t join_waitq;
} thread_info_t;

static thread_info_t *threads = NULL;
static int thread_capacity = 0;
static int pthread_initialized = 0;

typedef struct {
    int pshared;
    int type;
    int protocol;
    int prioceiling;
    int robust;
} mutexattr_t;

typedef struct {
    int locked;
    pid_t owner;
    int type;
    int recursive_count;
    wait_queue_t waitq;
} pthread_mutex_internal_t;

typedef struct {
    wait_queue_t waitq;
    int signaled;
} pthread_cond_internal_t;

#define MUTEX_INIT_COUNT 32
#define COND_INIT_COUNT 16

static pthread_mutex_internal_t *mutexes = NULL;
static int mutex_capacity = 0;
static pthread_cond_internal_t *conds = NULL;
static int cond_capacity = 0;

static int thread_grow(void) {
    int new_cap;
    thread_info_t *new_arr;
    int i;

    new_cap = thread_capacity ? thread_capacity * 2 : THREAD_INIT_COUNT;
    new_arr = (thread_info_t *)krealloc(threads, new_cap * sizeof(thread_info_t));
    if (!new_arr) return -1;
    for (i = thread_capacity; i < new_cap; i++) {
        memset(&new_arr[i], 0, sizeof(thread_info_t));
    }
    threads = new_arr;
    thread_capacity = new_cap;
    return 0;
}

static int mutex_grow(void) {
    int new_cap;
    pthread_mutex_internal_t *new_arr;
    int i;

    new_cap = mutex_capacity ? mutex_capacity * 2 : MUTEX_INIT_COUNT;
    new_arr = (pthread_mutex_internal_t *)krealloc(mutexes, new_cap * sizeof(pthread_mutex_internal_t));
    if (!new_arr) return -1;
    for (i = mutex_capacity; i < new_cap; i++) {
        memset(&new_arr[i], 0, sizeof(pthread_mutex_internal_t));
        waitq_init(&new_arr[i].waitq);
    }
    mutexes = new_arr;
    mutex_capacity = new_cap;
    return 0;
}

static int cond_grow(void) {
    int new_cap;
    pthread_cond_internal_t *new_arr;
    int i;

    new_cap = cond_capacity ? cond_capacity * 2 : COND_INIT_COUNT;
    new_arr = (pthread_cond_internal_t *)krealloc(conds, new_cap * sizeof(pthread_cond_internal_t));
    if (!new_arr) return -1;
    for (i = cond_capacity; i < new_cap; i++) {
        memset(&new_arr[i], 0, sizeof(pthread_cond_internal_t));
        waitq_init(&new_arr[i].waitq);
    }
    conds = new_arr;
    cond_capacity = new_cap;
    return 0;
}

static void init_pthread(void) {
    if (pthread_initialized) return;
    pthread_initialized = 1;
    thread_grow();
    mutex_grow();
    cond_grow();
}

static int sys_pthread_create(int thread_ptr, const char *start_routine_ptr, int arg_ptr) {
    init_pthread();
    
    uint64_t thread_addr = (uint64_t)thread_ptr;
    uint64_t func_addr = (uint64_t)(uintptr_t)start_routine_ptr;
    uint64_t arg_addr = (uint64_t)arg_ptr;
    
    if (!thread_addr || thread_addr >= KERNEL_VMA || thread_addr < 0x1000) return -EFAULT;
    if (!func_addr || func_addr >= KERNEL_VMA || func_addr < 0x1000) return -EFAULT;
    
    int slot = -1;
    for (int i = 0; i < thread_capacity; i++) {
        if (!threads[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (thread_grow() < 0) return -EAGAIN;
        slot = thread_capacity / 2;
    }
    
    pid_t new_tid = task_create_thread_with_arg((void *(*)(void *))func_addr, (void *)arg_addr);
    if (new_tid == (pid_t)-1) return -EAGAIN;
    
    threads[slot].in_use = 1;
    threads[slot].tid = new_tid;
    threads[slot].stack = NULL;
    threads[slot].stack_size = PTHREAD_STACK_SIZE;
    threads[slot].retval = NULL;
    threads[slot].detached = 0;
    threads[slot].exited = 0;
    waitq_init(&threads[slot].join_waitq);
    
    *(pid_t *)thread_addr = new_tid;
    
    return 0;
}

static int sys_pthread_exit(int retval_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    
    if (!current_task) return -ESRCH;
    
    pid_t tid = current_task->pid;
    for (int i = 0; i < thread_capacity; i++) {
        if (threads[i].in_use && threads[i].tid == tid) {
            threads[i].retval = (void *)(uintptr_t)retval_ptr;
            threads[i].exited = 1;
            waitq_wake_all(&threads[i].join_waitq);
            break;
        }
    }
    
    task_exit_deferred(0);
    schedule();
    for (;;) asm volatile ("hlt");
    return 0;
}

static int sys_pthread_join(int thread, const char *retval_ptr, int unused) {
    (void)unused;
    init_pthread();
    
    pid_t tid = (pid_t)thread;
    int slot = -1;
    
    for (int i = 0; i < thread_capacity; i++) {
        if (threads[i].in_use && threads[i].tid == tid) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) return -ESRCH;
    if (threads[slot].detached) return -EINVAL;
    
    while (!threads[slot].exited) {
        waitq_wait(&threads[slot].join_waitq);
    }
    
    if (retval_ptr) {
        uint64_t addr = (uint64_t)(uintptr_t)retval_ptr;
        if (addr && addr < KERNEL_VMA && addr >= 0x1000) {
            *(void **)addr = threads[slot].retval;
        }
    }
    
    threads[slot].in_use = 0;
    return 0;
}

static int sys_pthread_detach(int thread, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    init_pthread();
    
    pid_t tid = (pid_t)thread;
    
    for (int i = 0; i < thread_capacity; i++) {
        if (threads[i].in_use && threads[i].tid == tid) {
            threads[i].detached = 1;
            return 0;
        }
    }
    
    return -ESRCH;
}

static int sys_pthread_self(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    return current_task ? (int)current_task->pid : 0;
}

static int sys_pthread_mutex_init(int mutex_ptr, const char *attr_ptr, int unused) {
    (void)attr_ptr; (void)unused;
    init_pthread();
    
    uint64_t addr = (uint64_t)mutex_ptr;
    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    
    int slot = -1;
    for (int i = 0; i < mutex_capacity; i++) {
        if (!mutexes[i].locked && mutexes[i].owner == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (mutex_grow() < 0) return -ENOMEM;
        slot = mutex_capacity / 2;
    }
    
    mutexes[slot].locked = 0;
    mutexes[slot].owner = 0;
    mutexes[slot].type = 0;
    mutexes[slot].recursive_count = 0;
    waitq_init(&mutexes[slot].waitq);
    
    *(int *)addr = slot;
    return 0;
}

static int sys_pthread_mutex_destroy(int mutex_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    
    uint64_t addr = (uint64_t)mutex_ptr;
    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    
    int slot = *(int *)addr;
    if (slot < 0 || slot >= mutex_capacity) return -EINVAL;
    
    if (mutexes[slot].locked) return -EBUSY;
    
    mutexes[slot].owner = 0;
    return 0;
}

static int sys_pthread_mutex_lock(int mutex_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    init_pthread();
    
    uint64_t addr = (uint64_t)mutex_ptr;
    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    
    int slot = *(int *)addr;
    if (slot < 0 || slot >= mutex_capacity) return -EINVAL;
    
    pid_t me = current_task ? current_task->pid : 0;
    
    while (mutexes[slot].locked && mutexes[slot].owner != me) {
        waitq_wait(&mutexes[slot].waitq);
    }
    
    if (mutexes[slot].locked && mutexes[slot].owner == me) {
        mutexes[slot].recursive_count++;
    } else {
        mutexes[slot].locked = 1;
        mutexes[slot].owner = me;
        mutexes[slot].recursive_count = 1;
    }
    
    return 0;
}

static int sys_pthread_mutex_trylock(int mutex_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    init_pthread();
    
    uint64_t addr = (uint64_t)mutex_ptr;
    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    
    int slot = *(int *)addr;
    if (slot < 0 || slot >= mutex_capacity) return -EINVAL;
    
    pid_t me = current_task ? current_task->pid : 0;
    
    if (mutexes[slot].locked && mutexes[slot].owner != me) {
        return -EBUSY;
    }
    
    if (mutexes[slot].locked && mutexes[slot].owner == me) {
        mutexes[slot].recursive_count++;
    } else {
        mutexes[slot].locked = 1;
        mutexes[slot].owner = me;
        mutexes[slot].recursive_count = 1;
    }
    
    return 0;
}

static int sys_pthread_mutex_unlock(int mutex_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    
    uint64_t addr = (uint64_t)mutex_ptr;
    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    
    int slot = *(int *)addr;
    if (slot < 0 || slot >= mutex_capacity) return -EINVAL;
    
    pid_t me = current_task ? current_task->pid : 0;
    
    if (!mutexes[slot].locked || mutexes[slot].owner != me) {
        return -EPERM;
    }
    
    mutexes[slot].recursive_count--;
    if (mutexes[slot].recursive_count == 0) {
        mutexes[slot].locked = 0;
        mutexes[slot].owner = 0;
        waitq_wake_one(&mutexes[slot].waitq);
    }
    
    return 0;
}

static int sys_pthread_cond_init(int cond_ptr, const char *attr_ptr, int unused) {
    (void)attr_ptr; (void)unused;
    init_pthread();
    
    uint64_t addr = (uint64_t)cond_ptr;
    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    
    int slot = -1;
    for (int i = 0; i < cond_capacity; i++) {
        if (!conds[i].signaled) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (cond_grow() < 0) return -ENOMEM;
        slot = cond_capacity / 2;
    }
    
    waitq_init(&conds[slot].waitq);
    conds[slot].signaled = 0;
    
    *(int *)addr = slot;
    return 0;
}

static int sys_pthread_cond_destroy(int cond_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    
    uint64_t addr = (uint64_t)cond_ptr;
    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    
    int slot = *(int *)addr;
    if (slot < 0 || slot >= cond_capacity) return -EINVAL;
    
    return 0;
}

static int sys_pthread_cond_wait(int cond_ptr, const char *mutex_ptr, int unused) {
    (void)unused;
    init_pthread();
    
    uint64_t cond_addr = (uint64_t)cond_ptr;
    uint64_t mutex_addr = (uint64_t)(uintptr_t)mutex_ptr;
    
    if (!cond_addr || cond_addr >= KERNEL_VMA || cond_addr < 0x1000) return -EFAULT;
    if (!mutex_addr || mutex_addr >= KERNEL_VMA || mutex_addr < 0x1000) return -EFAULT;
    
    int cond_slot = *(int *)cond_addr;
    int mutex_slot = *(int *)mutex_addr;
    
    if (cond_slot < 0 || cond_slot >= cond_capacity) return -EINVAL;
    if (mutex_slot < 0 || mutex_slot >= mutex_capacity) return -EINVAL;
    
    sys_pthread_mutex_unlock((int)mutex_addr, NULL, 0);
    
    waitq_wait(&conds[cond_slot].waitq);
    
    sys_pthread_mutex_lock((int)mutex_addr, NULL, 0);
    
    return 0;
}

static int sys_pthread_cond_signal(int cond_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    
    uint64_t addr = (uint64_t)cond_ptr;
    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    
    int slot = *(int *)addr;
    if (slot < 0 || slot >= cond_capacity) return -EINVAL;
    
    waitq_wake_one(&conds[slot].waitq);
    return 0;
}

static int sys_pthread_cond_broadcast(int cond_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    
    uint64_t addr = (uint64_t)cond_ptr;
    if (!addr || addr >= KERNEL_VMA || addr < 0x1000) return -EFAULT;
    
    int slot = *(int *)addr;
    if (slot < 0 || slot >= cond_capacity) return -EINVAL;
    
    waitq_wake_all(&conds[slot].waitq);
    return 0;
}

#define SYSCALL_PTHREAD_CREATE 223
#define SYSCALL_PTHREAD_EXIT 224
#define SYSCALL_PTHREAD_JOIN 225
#define SYSCALL_PTHREAD_DETACH 226
#define SYSCALL_PTHREAD_SELF 227
#define SYSCALL_PTHREAD_MUTEX_INIT 228
#define SYSCALL_PTHREAD_MUTEX_DESTROY 229
#define SYSCALL_PTHREAD_MUTEX_LOCK 230
#define SYSCALL_PTHREAD_MUTEX_TRYLOCK 231
#define SYSCALL_PTHREAD_MUTEX_UNLOCK 232
#define SYSCALL_PTHREAD_COND_INIT 233
#define SYSCALL_PTHREAD_COND_DESTROY 234
#define SYSCALL_PTHREAD_COND_WAIT 235
#define SYSCALL_PTHREAD_COND_SIGNAL 236
#define SYSCALL_PTHREAD_COND_BROADCAST 237

void syscalls_pthread_init(void) {
    init_pthread();
    syscall_table[SYSCALL_PTHREAD_CREATE] = sys_pthread_create;
    syscall_table[SYSCALL_PTHREAD_EXIT] = sys_pthread_exit;
    syscall_table[SYSCALL_PTHREAD_JOIN] = sys_pthread_join;
    syscall_table[SYSCALL_PTHREAD_DETACH] = sys_pthread_detach;
    syscall_table[SYSCALL_PTHREAD_SELF] = sys_pthread_self;
    syscall_table[SYSCALL_PTHREAD_MUTEX_INIT] = sys_pthread_mutex_init;
    syscall_table[SYSCALL_PTHREAD_MUTEX_DESTROY] = sys_pthread_mutex_destroy;
    syscall_table[SYSCALL_PTHREAD_MUTEX_LOCK] = sys_pthread_mutex_lock;
    syscall_table[SYSCALL_PTHREAD_MUTEX_TRYLOCK] = sys_pthread_mutex_trylock;
    syscall_table[SYSCALL_PTHREAD_MUTEX_UNLOCK] = sys_pthread_mutex_unlock;
    syscall_table[SYSCALL_PTHREAD_COND_INIT] = sys_pthread_cond_init;
    syscall_table[SYSCALL_PTHREAD_COND_DESTROY] = sys_pthread_cond_destroy;
    syscall_table[SYSCALL_PTHREAD_COND_WAIT] = sys_pthread_cond_wait;
    syscall_table[SYSCALL_PTHREAD_COND_SIGNAL] = sys_pthread_cond_signal;
    syscall_table[SYSCALL_PTHREAD_COND_BROADCAST] = sys_pthread_cond_broadcast;
}
