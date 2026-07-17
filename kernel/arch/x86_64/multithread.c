#include <lebirun/multithread.h>
#include <lebirun/task.h>
#include <lebirun/mem_map.h>
#include <lebirun/mutex.h>
#include <string.h>

#define THREAD_INITIAL_CAPACITY 4

thread_t *current_thread = NULL;
thread_t *thread_ready_head = NULL;
static thread_t *threads;
static int thread_capacity;
static uint64_t next_thread_id = 1;
static mutex_t thread_lock;

static void thread_entry_wrapper(void);
static void do_context_switch(thread_t *from, thread_t *to);

static int thread_grow(void) {
    int new_cap;
    thread_t *new_arr;

    if (thread_capacity >= MAX_THREADS) return -1;
    new_cap = thread_capacity == 0 ? THREAD_INITIAL_CAPACITY : thread_capacity * 2;
    if (new_cap > MAX_THREADS) new_cap = MAX_THREADS;
    new_arr = (thread_t *)kmalloc(new_cap * sizeof(thread_t));
    if (!new_arr) return -1;
    memset(new_arr, 0, new_cap * sizeof(thread_t));
    if (threads && thread_capacity > 0) {
        memcpy(new_arr, threads, thread_capacity * sizeof(thread_t));
        kfree(threads);
    }
    threads = new_arr;
    thread_capacity = new_cap;
    return 0;
}

static thread_t *thread_alloc(void) {
    int i;
    int old_cap;

    for (i = 0; i < thread_capacity; i++) {
        if (threads[i].state == THREAD_DEAD) {
            memset(&threads[i], 0, sizeof(thread_t));
            threads[i].id = next_thread_id++;
            return &threads[i];
        }
    }
    old_cap = thread_capacity;
    if (thread_grow() < 0) return NULL;
    memset(&threads[old_cap], 0, sizeof(thread_t));
    threads[old_cap].id = next_thread_id++;
    return &threads[old_cap];
}

static void thread_free(thread_t *thread) {
    thread->state = THREAD_DEAD;
}

static void thread_add_to_ready(thread_t *thread) {
    thread->state = THREAD_READY;
    thread->next = thread_ready_head;
    thread_ready_head = thread;
}

static thread_t *thread_remove_from_ready(void) {
    thread_t *thread;

    if (!thread_ready_head) return NULL;
    thread = thread_ready_head;
    thread_ready_head = thread->next;
    thread->next = NULL;
    return thread;
}

void init_threads(void) {
    mutex_init(&thread_lock);
    current_thread = NULL;
    thread_ready_head = NULL;
    threads = NULL;
    thread_capacity = 0;
    thread_grow();
}

thread_t *create_thread(void (*entry)(void *arg), void *arg) {
    thread_t *thread;

    mutex_lock(&thread_lock);
    
    thread = thread_alloc();
    if (!thread) {
        mutex_unlock(&thread_lock);
        return NULL;
    }
    
    thread->entry = entry;
    thread->arg = arg;
    thread->state = THREAD_READY;
    thread->stack_size = THREAD_STACK_SIZE;
    thread->stack_base = (uint8_t *)kmalloc(thread->stack_size);
    if (!thread->stack_base) {
        thread_free(thread);
        mutex_unlock(&thread_lock);
        return NULL;
    }
    
    memset(&thread->regs, 0, sizeof(registers_t));
    thread->regs.rip = (uint64_t)thread_entry_wrapper;
    thread->regs.rsp = (uint64_t)(thread->stack_base + thread->stack_size);
    thread->regs.rflags = 0x202;
    
    thread_add_to_ready(thread);
    
    mutex_unlock(&thread_lock);
    return thread;
}

static void thread_entry_wrapper(void) {
    if (current_thread && current_thread->entry) {
        current_thread->entry(current_thread->arg);
    }
    thread_exit(0);
}

void schedule_thread(void) {
    thread_t *next;
    thread_t *prev;

    if (!current_thread) {
        current_thread = thread_remove_from_ready();
        if (current_thread) {
            current_thread->state = THREAD_RUNNING;
        }
        return;
    }
    
    if (current_thread->state == THREAD_RUNNING) {
        thread_add_to_ready(current_thread);
        current_thread->state = THREAD_READY;
    }
    
    next = thread_remove_from_ready();
    if (next) {
        prev = current_thread;
        current_thread = next;
        current_thread->state = THREAD_RUNNING;
        if (prev != current_thread) {
            do_context_switch(prev, current_thread);
        }
    }
}

registers_t *schedule_thread_from_irq(registers_t *regs) {
    if (current_thread) {
        memcpy(&current_thread->regs, regs, sizeof(registers_t));
    }
    
    schedule_thread();
    
    if (current_thread) {
        memcpy(regs, &current_thread->regs, sizeof(registers_t));
    }
    
    return regs;
}

void yield_thread(void) {
    schedule_thread();
}

void block_current_thread(void) {
    if (current_thread) {
        current_thread->state = THREAD_BLOCKED;
        schedule_thread();
    }
}

void wake_thread(thread_t *thread) {
    if (thread && thread->state == THREAD_BLOCKED) {
        thread_add_to_ready(thread);
    }
}

void thread_exit(int code) {
    if (current_thread) {
        current_thread->exit_code = code;
        current_thread->state = THREAD_DEAD;
        if (current_thread->stack_base) {
            kfree(current_thread->stack_base);
        }
        schedule_thread();
    }
}

int thread_join(thread_t *thread, int *exit_code) {
    while (thread->state != THREAD_DEAD) {
        yield_thread();
    }
    if (exit_code) {
        *exit_code = thread->exit_code;
    }
    thread_free(thread);
    return 0;
}

static void do_context_switch(thread_t *from, thread_t *to) {
    if (from == to) return;
    (void)from;
    (void)to;
}
