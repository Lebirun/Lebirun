#ifndef TASK_H
#define TASK_H

#include <kernel/common.h>
#include <kernel/registers.h>
#include <stdint.h>

#define TASK_MAX_FDS 64

#define FD_TYPE_FILE   0
#define FD_TYPE_PIPE_R 1
#define FD_TYPE_PIPE_W 2
#define FD_TYPE_STDIN  3
#define FD_TYPE_STDOUT 4
#define FD_TYPE_STDERR 5

typedef enum {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_DEAD
} task_state_t;

typedef struct wait_queue {
    struct task* head;
} wait_queue_t;

typedef struct {
    int in_use;
    int type;
    void *node;
    uint32_t offset;
    uint32_t flags;
    int ref_count;
    void *private_data;
} task_fd_t;

typedef struct task {
    uint32_t id;
    pid_t pid; 
    task_state_t state;
    struct task *next;
    struct task *sleep_next;
    int in_sleep_queue;
    int in_wait_queue;
    struct task *wait_next;
    wait_queue_t *waiting_queue;
    struct task *join_target;
    wait_queue_t join_waiters;
    int join_refs;
    uint32_t exit_code;
    registers_t regs;
    uint32_t cr3;
    int time_slice;
    int base_time_slice;
    uint8_t *stack_base;
    uint32_t stack_size;
    uint8_t *kernel_stack_base;
    uint32_t kernel_stack_size;
    uint32_t wake_tick;
    bool is_user;
    uint32_t user_brk;
    uint32_t mmap_next_addr;
    registers_t *syscall_frame;
    uint32_t pd_phys;
    uint32_t *user_pages;
    uint32_t user_pages_count;
    int console_id;
    uint32_t tls_base;
    uint32_t tls_limit;
    char cwd[256];
    char **envp;
    int envc;
    task_fd_t fds[TASK_MAX_FDS];
    
    uint32_t uid;
    uint32_t gid;
    uint32_t euid;
    uint32_t egid;
    uint32_t suid;
    uint32_t sgid;
    uint32_t fsuid;
    uint32_t fsgid;
    uint32_t groups[32];
    int ngroups;
    
    pid_t pgid;
    pid_t sid;
    pid_t ppid;
    int waiting_for_any_child;
    
    uint32_t sig_pending;
    uint32_t sig_blocked;
    struct {
        void (*handler)(int);
        uint32_t flags;
        uint32_t mask;
    } sigactions[32];
    void *sig_altstack;
    size_t sig_altstack_size;
    
    int *clear_child_tid;
    void *robust_list;
    size_t robust_list_len;
    char name[16];
    
    uint8_t vring_minor;
    bool is_kernel_task;
} task_t;

extern task_t* current_task;
extern task_t* ready_queue_head;

pid_t getpid(void);
task_t* task_find(pid_t pid);

int task_has_child_of(pid_t parent_pid, pid_t pgid_filter);
task_t* task_find_dead_child_of(pid_t parent_pid, pid_t pgid_filter);

void init_tasks(void);
task_t* create_task(void (*entry)(void), task_state_t initial_state, bool user_mode);
task_t* create_task_with_cr3(void (*entry)(void), task_state_t initial_state, bool user_mode, uint32_t cr3);
void schedule(void);
registers_t* schedule_from_irq(registers_t* regs);
extern void save_context(void);
extern void switch_to(task_t* next);
void lock_scheduler(void);
void unlock_scheduler(void);
void add_task_to_runqueue(task_t* new_task);

void yield(void);
void block_current(void);
void wake_task(task_t* task);
void task_kill(task_t* task, uint32_t exit_code);
void sleep_ticks(uint32_t ticks);
void wake_sleeping_tasks(void);
void reap_dead_tasks(void);
void task_exit(uint32_t exit_code);
void task_exit_deferred(uint32_t exit_code);
void sleep_ms(uint32_t ms);
int task_join(task_t* task, uint32_t* exit_code);

void waitq_init(wait_queue_t* q);
void waitq_add(wait_queue_t* q, task_t* t);
task_t* waitq_pop(wait_queue_t* q);
void waitq_wake_all(wait_queue_t* q);
void waitq_wake_one(wait_queue_t* q);
void waitq_wait(wait_queue_t* q);
void waitq_remove(wait_queue_t* q, task_t* t);

void task_free_user_memory(task_t* t);

void set_syscall_frame(registers_t *frame);
void clear_syscall_frame(void);

void task_init_fds(task_t *task);
int task_fd_alloc(task_t *task);
void task_fd_free(task_t *task, int fd);
task_fd_t *task_fd_get(task_t *task, int fd);
void task_fd_close_all(task_t *task);

pid_t task_fork(registers_t *parent_regs);
int task_exec(const uint8_t *bin_start, uint32_t bin_size, registers_t *regs);
int task_exec_with_args(const uint8_t *bin_start, uint32_t bin_size, registers_t *regs,
                        int argc, char **argv, int envc, char **envp);
pid_t task_create_thread(void (*entry)(void));
pid_t task_create_thread_with_arg(void *(*entry)(void *), void *arg);

bool task_is_kernel_pid(int32_t pid);
void task_set_vring(task_t *task, uint8_t vring_minor);

#endif