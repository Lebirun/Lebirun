#ifndef TASK_PRIV_H
#define TASK_PRIV_H

#include <lebirun/task.h>
#include <lebirun/debug.h>
#include <lebirun/pipe.h>
#include <lebirun/pty.h>
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
#include <lebirun/squashfs.h>
#include <lebirun/fs/ext4/ext4.h>
#include <lebirun/rng.h>
#include <lebirun/panic.h>
#include <lebirun/uaccess.h>
#include <lebirun/watchdog.h>
#include <lebirun/evdev.h>
#include <lebirun/mutex.h>
#include <lebirun/seccomp.h>
#include <lebirun/pit.h>
#include <lebirun/timekeeping.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

extern void temp_map_raw(uint64_t temp_virt, uint64_t phys_addr);
extern void temp_unmap_raw(uint64_t temp_virt);
extern char _kernel_text_start[];
extern char _kernel_text_end[];
extern void sysfs_reclaim_unused(void);
extern void event_descriptors_close_task(pid_t pid);
extern void event_descriptors_close_cloexec(pid_t pid);
extern void shm_close_vm(task_vm_t *vm);
extern int shm_fork_vm(task_vm_t *parent_vm, task_vm_t *child_vm);
extern void socket_close_task(task_t *task);
extern void socket_close_cloexec(task_t *task);
extern void socket_retain_task_fd(task_fd_t *descriptor);
extern void file_locks_release_process(pid_t pid);
extern void file_locks_release_process_node(pid_t pid, vfs_node_t *node,
                                            int release_flock);
extern void syscall_robust_list_exit(task_t *task);
extern void copy_file_range_release_task(void *owner);

#define TASK_ANON_RECLAIM_TEMP TEMP_SLOT(6)

#define KERR_EPERM   1
#define KERR_EIO     5
#define KERR_ENOEXEC 8
#define KERR_ECHILD 10
#define KERR_EINTR   4
#define KERR_EAGAIN  11
#define KERR_EFAULT  14
#define KERR_ENOMEM  12
#define KERR_EINVAL  22
#define KERR_ETIMEDOUT 110

#define TASK_WAIT_QUEUE_NORMAL 1
#define TASK_WAIT_QUEUE_FUTEX  2
#define TASK_DEFERRED_REAP 1
#define TASK_DEFERRED_EXEC_DRAIN 2
#define TASK_INIT_PID 1
#define TASK_SIGCHLD 17
#define MEMORY_PRESSURE_REQUESTED 1

#define SCHED_DEFAULT_TIMESLICE 3
#define TASK_SCHED_OTHER 0
#define TASK_SCHED_FIFO 1
#define TASK_SCHED_RR 2
#define TASK_SCHED_DEADLINE 6
extern int cpu_count;
extern volatile uint64_t tick_count;
extern volatile uint64_t task_context_switches;
extern void switch_to_asm(uint64_t* old_rsp, uint64_t new_rsp);
extern void posix_timers_release_task(task_t *task);
extern void task_timer_check(void);

#define USER_STACK_SIZE 0x10000u
#define USER_STACK_INITIAL_MIN 0x1000u
#define USER_STACK_GAP  0x1000u
#define USER_STACK_INIT_ESP (USER_STACK_TOP - USER_STACK_GAP - 16u)
#define FPU_STATE_SIZE 512

extern spinlock_t sched_lock;
extern int scheduler_initialized;
extern task_t* bootstrap_task;
extern int idle_wake_pending;
extern task_t* dead_queue_head;
void task_free_cwd(task_t *task);
extern volatile uint32_t task_deferred_pending;
extern task_vm_t task_vm_none;
task_vm_t *task_vm_alloc(void);
void task_vm_share(task_t *child, task_t *parent);
uint64_t task_allocate_id(void);
int task_ptr_valid(task_t *t);
int task_timeslice_for_nice(int nice_value);
int task_affinity_copy(task_t *dst, task_t *src);
void task_affinity_free(task_t *task);
void task_mark_ready_locked(task_t *task);
void task_kick_idle_cpus(void);
void remove_task_from_runqueue(task_t* task);
extern uint32_t fpu_state_size;
void fpu_save_area(uint8_t *area);
void fpu_restore_area(uint8_t *area);
void fpu_init_default_state(void);
int task_init_fpu_state(task_t *task);
void task_free_fpu_state(task_t *task);
void task_reset_fpu_state(task_t *task);
void task_write_fs_base(uint64_t base);
void sleepq_remove(task_t* t);
uint64_t sleepq_deadline_after_ticks(uint64_t ticks);
void sleepq_insert(task_t *task);
void task_remove_wait_locked(task_t *task);
void descriptor_ready_drain_irq_locked(void);
void descriptor_ready_try_drain_irq(void);
void task_vm_abandon(task_t *t);
void task_clear_file_mappings(task_t *t);
void task_release_file_mappings(task_file_map_t *maps, int count);
void task_exec_abort_vm(task_t *t, task_vm_t *old_vm);
int task_ensure_file_map_capacity(task_t *task, int needed);
uint64_t user_zero_page_get(void);
extern spinlock_t task_fd_array_lock;
uint64_t task_random_mmap_base(void);
uint64_t task_align_up(uint64_t v, uint64_t align);
int task_is_current_on_any_cpu(task_t *task);
int task_pml4_is_current_on_any_cpu(uint64_t pml4_phys);
int task_free_pml4_if_unowned(task_t *owner, uint64_t pd);
int task_free_exec_old_pml4_if_unowned(task_t *owner, uint64_t pd);
void exec_cleanup_stats(uint64_t *entries, uint64_t *pages);
uint64_t task_ksm_merge(uint64_t max_pages);

#endif
