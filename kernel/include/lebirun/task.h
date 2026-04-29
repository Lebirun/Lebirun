#ifndef TASK_H
#define TASK_H

#include <lebirun/common.h>
#include <lebirun/registers.h>
#include <stdint.h>

struct vfs_node;

#define TASK_INIT_FDS 16
#define TASK_MAX_FDS 1024
#define TASK_MAX_FILE_MAPS 16

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
    TASK_STOPPED,
    TASK_DEAD
} task_state_t;

typedef struct wait_queue {
    struct task* head;
} wait_queue_t;

typedef struct {
    int in_use;
    int type;
    void *node;
    uint64_t offset;
    uint64_t flags;
    int ref_count;
    void *private_data;
    uint8_t *read_buf;
    uint64_t read_buf_offset;
    uint32_t read_buf_len;
} task_fd_t;

typedef struct task {
    uint64_t id;
    pid_t pid;
    task_state_t state;
    struct task *next;
    struct task *all_next;
    struct task *sleep_next;
    int in_sleep_queue;
    int in_wait_queue;
    struct task *wait_next;
    wait_queue_t *waiting_queue;
    struct task *join_target;
    wait_queue_t join_waiters;
    int join_refs;
    uint64_t exit_code;
    registers_t regs;
    uint64_t cr3;
    int time_slice;
    int base_time_slice;
    uint8_t *stack_base;
    uint64_t stack_size;
    uint8_t *kernel_stack_base;
    uint64_t kernel_stack_size;
    uint64_t wake_tick;
    bool is_user;
    uint64_t user_brk;
    uint64_t mmap_next_addr;
    registers_t *syscall_frame;
    uint64_t pml4_phys;
    uint64_t *user_pages;
    uint64_t user_pages_count;
    struct {
        struct vfs_node *node;
        uint64_t vaddr;
        uint64_t memsz;
        uint64_t filesz;
        uint64_t offset;
        uint64_t flags;
    } file_maps[TASK_MAX_FILE_MAPS];
    int file_map_count;
    int console_id;
    uint64_t tls_base;
    uint64_t tls_limit;
    char cwd[128];
    char **envp;
    int envc;
    task_fd_t *fds;
    int fds_capacity;

    uint64_t uid;
    uint64_t gid;
    uint64_t euid;
    uint64_t egid;
    uint64_t suid;
    uint64_t sgid;
    uint64_t fsuid;
    uint64_t fsgid;
    uint64_t groups[16];
    int ngroups;

    pid_t pgid;
    pid_t sid;
    pid_t ppid;
    int waiting_for_any_child;

    uint64_t sig_pending;
    uint64_t sig_blocked;
    struct {
        void (*handler)(int);
        uint64_t flags;
        uint64_t mask;
    } sigactions[32];
    void *sig_altstack;
    size_t sig_altstack_size;
    void *signal_data;
    void *creds_data;

    int *clear_child_tid;
    void *robust_list;
    size_t robust_list_len;
    char name[16];

    uint64_t start_tick;
    uint64_t utime;
    uint64_t stime;

    uint8_t vring_minor;
    bool is_kernel_task;

    int exec_completed;
    int waited;

    uint64_t exec_old_pml4;
    uint64_t *exec_old_pages;
    uint64_t exec_old_pages_count;

    struct {
        struct { long tv_sec; long tv_usec; } it_interval;
        struct { long tv_sec; long tv_usec; } it_value;
    } itimers[3];
    uint64_t alarm_tick;
} task_t;

extern task_t* current_task;
extern task_t* ready_queue_head;
extern task_t* all_tasks_head;

pid_t getpid(void);
task_t* task_find(pid_t pid);

int task_has_child_of(pid_t parent_pid, pid_t pgid_filter);
task_t* task_find_dead_child_of(pid_t parent_pid, pid_t pgid_filter);

void init_tasks(void);
task_t* create_task(void (*entry)(void), task_state_t initial_state, bool user_mode);
task_t* create_task_with_cr3(void (*entry)(void), task_state_t initial_state, bool user_mode, uint64_t cr3);
task_t* create_kernel_task(void (*entry)(void), task_state_t initial_state);
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
void task_kill(task_t* task, uint64_t exit_code);
void sleep_ticks(uint64_t ticks);
void wake_sleeping_tasks(void);
void reap_dead_tasks(void);
void reap_request(void);
void exec_drain_request(void);
void task_deferred_work(void);
void task_exit(uint64_t exit_code);
void task_exit_deferred(uint64_t exit_code);
void sleep_ms(uint64_t ms);
int task_join(task_t* task, uint64_t* exit_code);

void waitq_init(wait_queue_t* q);
void waitq_add(wait_queue_t* q, task_t* t);
task_t* waitq_pop(wait_queue_t* q);
void waitq_wake_all(wait_queue_t* q);
void waitq_wake_one(wait_queue_t* q);
void waitq_wait(wait_queue_t* q);
void waitq_remove(wait_queue_t* q, task_t* t);

void task_free_user_memory(task_t* t);
int task_add_file_mapping(task_t *task, struct vfs_node *node, uint64_t vaddr,
                          uint64_t memsz, uint64_t filesz, uint64_t offset,
                          uint64_t flags);
int task_handle_file_page_fault(task_t *task, uint64_t fault_addr);
int task_handle_file_write_fault(task_t *task, uint64_t fault_addr);
void exec_page_cache_reclaim(uint64_t target_pages);
uint64_t exec_page_cache_get_pages(void);
uint64_t exec_page_cache_get_reclaimable_pages(void);

void set_syscall_frame(registers_t *frame);
void clear_syscall_frame(void);

void task_init_fds(task_t *task);
int task_fd_alloc(task_t *task);
void task_fd_free(task_t *task, int fd);
task_fd_t *task_fd_get(task_t *task, int fd);
void task_fd_close_all(task_t *task);
void task_fd_close_cloexec(task_t *task);

pid_t task_fork(registers_t *parent_regs);
int task_exec(const uint8_t *bin_start, uint64_t bin_size, registers_t *regs);
int task_exec_with_args(const uint8_t *bin_start, uint64_t bin_size, registers_t *regs,
                        int argc, char **argv, int envc, char **envp);
int task_exec_node_with_args(struct vfs_node *node, registers_t *regs,
                             int argc, char **argv, int envc, char **envp);
pid_t task_create_thread(void (*entry)(void));
pid_t task_create_thread_with_arg(void *(*entry)(void *), void *arg);

bool task_is_kernel_pid(int32_t pid);
void task_set_vring(task_t *task, uint8_t vring_minor);

int deliver_signal_to_task(task_t *target, int sig);
int collect_pids_in_pgrp(pid_t pgid, pid_t *out, int out_cap);
void signal_deliver_pending(registers_t *regs);
void signals_init_task(struct task *task);
int task_has_pending_signals(void);

void exec_cleanup_enqueue(uint64_t pml4, uint64_t *pages, uint64_t count);
void exec_cleanup_drain(void);

#endif
