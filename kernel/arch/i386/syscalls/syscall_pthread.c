#include "syscall_defs.h"

extern task_t *current_task;
extern void *syscall_table[];

#define MAX_THREADS 64
#define PTHREAD_STACK_SIZE 0x4000

typedef struct {
    int in_use;
    pid_t tid;
    void *stack;
    uint32_t stack_size;
    void *retval;
    int detached;
    int exited;
    wait_queue_t join_waitq;
} thread_info_t;

static thread_info_t threads[MAX_THREADS];
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

#define MAX_MUTEXES 128
#define MAX_CONDS 64

static pthread_mutex_internal_t mutexes[MAX_MUTEXES];
static pthread_cond_internal_t conds[MAX_CONDS];

static void init_pthread(void) {
    if (pthread_initialized) return;
    pthread_initialized = 1;
    memset(threads, 0, sizeof(threads));
    memset(mutexes, 0, sizeof(mutexes));
    memset(conds, 0, sizeof(conds));
    for (int i = 0; i < MAX_MUTEXES; i++) {
        waitq_init(&mutexes[i].waitq);
    }
    for (int i = 0; i < MAX_CONDS; i++) {
        waitq_init(&conds[i].waitq);
    }
}

static int sys_pthread_create(int thread_ptr, const char *start_routine_ptr, int arg_ptr) {
    init_pthread();
    
    uint32_t thread_addr = (uint32_t)thread_ptr;
    uint32_t func_addr = (uint32_t)(uintptr_t)start_routine_ptr;
    uint32_t arg_addr = (uint32_t)arg_ptr;
    
    if (!thread_addr || thread_addr >= 0xC0000000 || thread_addr < 0x1000) return -EFAULT;
    if (!func_addr || func_addr >= 0xC0000000 || func_addr < 0x1000) return -EFAULT;
    
    int slot = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!threads[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -EAGAIN;
    
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
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].in_use && threads[i].tid == tid) {
            threads[i].retval = (void *)retval_ptr;
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
    
    for (int i = 0; i < MAX_THREADS; i++) {
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
        uint32_t addr = (uint32_t)(uintptr_t)retval_ptr;
        if (addr && addr < 0xC0000000 && addr >= 0x1000) {
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
    
    for (int i = 0; i < MAX_THREADS; i++) {
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
    
    uint32_t addr = (uint32_t)mutex_ptr;
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    
    int slot = -1;
    for (int i = 0; i < MAX_MUTEXES; i++) {
        if (!mutexes[i].locked && mutexes[i].owner == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -ENOMEM;
    
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
    
    uint32_t addr = (uint32_t)mutex_ptr;
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    
    int slot = *(int *)addr;
    if (slot < 0 || slot >= MAX_MUTEXES) return -EINVAL;
    
    if (mutexes[slot].locked) return -EBUSY;
    
    mutexes[slot].owner = 0;
    return 0;
}

static int sys_pthread_mutex_lock(int mutex_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    init_pthread();
    
    uint32_t addr = (uint32_t)mutex_ptr;
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    
    int slot = *(int *)addr;
    if (slot < 0 || slot >= MAX_MUTEXES) return -EINVAL;
    
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
    
    uint32_t addr = (uint32_t)mutex_ptr;
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    
    int slot = *(int *)addr;
    if (slot < 0 || slot >= MAX_MUTEXES) return -EINVAL;
    
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
    
    uint32_t addr = (uint32_t)mutex_ptr;
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    
    int slot = *(int *)addr;
    if (slot < 0 || slot >= MAX_MUTEXES) return -EINVAL;
    
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
    
    uint32_t addr = (uint32_t)cond_ptr;
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    
    int slot = -1;
    for (int i = 0; i < MAX_CONDS; i++) {
        if (!conds[i].signaled) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -ENOMEM;
    
    waitq_init(&conds[slot].waitq);
    conds[slot].signaled = 0;
    
    *(int *)addr = slot;
    return 0;
}

static int sys_pthread_cond_destroy(int cond_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    
    uint32_t addr = (uint32_t)cond_ptr;
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    
    int slot = *(int *)addr;
    if (slot < 0 || slot >= MAX_CONDS) return -EINVAL;
    
    return 0;
}

static int sys_pthread_cond_wait(int cond_ptr, const char *mutex_ptr, int unused) {
    (void)unused;
    init_pthread();
    
    uint32_t cond_addr = (uint32_t)cond_ptr;
    uint32_t mutex_addr = (uint32_t)(uintptr_t)mutex_ptr;
    
    if (!cond_addr || cond_addr >= 0xC0000000 || cond_addr < 0x1000) return -EFAULT;
    if (!mutex_addr || mutex_addr >= 0xC0000000 || mutex_addr < 0x1000) return -EFAULT;
    
    int cond_slot = *(int *)cond_addr;
    int mutex_slot = *(int *)mutex_addr;
    
    if (cond_slot < 0 || cond_slot >= MAX_CONDS) return -EINVAL;
    if (mutex_slot < 0 || mutex_slot >= MAX_MUTEXES) return -EINVAL;
    
    sys_pthread_mutex_unlock((int)mutex_addr, NULL, 0);
    
    waitq_wait(&conds[cond_slot].waitq);
    
    sys_pthread_mutex_lock((int)mutex_addr, NULL, 0);
    
    return 0;
}

static int sys_pthread_cond_signal(int cond_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    
    uint32_t addr = (uint32_t)cond_ptr;
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    
    int slot = *(int *)addr;
    if (slot < 0 || slot >= MAX_CONDS) return -EINVAL;
    
    waitq_wake_one(&conds[slot].waitq);
    return 0;
}

static int sys_pthread_cond_broadcast(int cond_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    
    uint32_t addr = (uint32_t)cond_ptr;
    if (!addr || addr >= 0xC0000000 || addr < 0x1000) return -EFAULT;
    
    int slot = *(int *)addr;
    if (slot < 0 || slot >= MAX_CONDS) return -EINVAL;
    
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
