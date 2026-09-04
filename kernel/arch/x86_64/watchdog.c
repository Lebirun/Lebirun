#include <lebirun/watchdog.h>
#include <lebirun/pit.h>
#include <lebirun/task.h>
#include <lebirun/panic.h>
#include <lebirun/common.h>
#include <lebirun/smp.h>
#include <lebirun/vfs.h>
#include <stdint.h>
#include <stdbool.h>

static volatile uint64_t wdt_last_kick = 0;
static volatile uint64_t wdt_stall_strikes = 0;
static volatile int wdt_disabled = 0;
static volatile int wdt_init_pid = 0;

extern volatile uint64_t tick_count;
extern task_t *all_tasks_head;

void watchdog_kick(void) {
    wdt_last_kick = tick_count;
}

void watchdog_disable(void) {
    wdt_disabled = 1;
}

void KERNEL_INIT watchdog_set_init_pid(int pid) {
    wdt_init_pid = pid;
}

static void watchdog_print_task_state(task_t *t) {
    const char *state_name;
    const char *lookup_name;
    registers_t *frame;
    vfs_node_t *lookup;

    switch (t->state) {
    case TASK_READY:   state_name = "READY";   break;
    case TASK_RUNNING: state_name = "RUNNING"; break;
    case TASK_BLOCKED: state_name = "BLOCKED"; break;
    case TASK_STOPPED: state_name = "STOPPED"; break;
    case TASK_DEAD:    state_name = "DEAD";    break;
    default:           state_name = "UNKNOWN"; break;
    }
    frame = t->syscall_frame;
    lookup = __atomic_load_n(&t->vfs_lookup_node, __ATOMIC_ACQUIRE);
    lookup_name = lookup ? vfs_node_name(lookup) : "-";
    printf("  PID=%d name=%s state=%s syscall=%lu rip=0x%lX min=%lu maj=%lu stage=%u lookup=%s\n",
           (int)t->pid, t->name[0] ? t->name : "(none)",
           state_name,
           frame ? frame->rax : UINT64_MAX,
           frame ? frame->rip : t->regs.rip,
           t->minor_faults, t->major_faults,
           t->kernel_stage,
           lookup_name ? lookup_name : "-");
}

static void watchdog_dump_tasks(void) {
    task_t *t;
    int count;

    t = all_tasks_head;
    count = 0;
    printf("WATCHDOG: active tasks:\n");
    while (t && count < 32) {
        watchdog_print_task_state(t);
        t = t->all_next;
        count++;
    }
    if (t) {
        printf("  ... (more tasks)\n");
    }
}

static void watchdog_callback(uint64_t ticks) {
    task_t *t;
    cpu_info_t *cpu;
    uint64_t elapsed;
    uint64_t flags;
    int lookup_complete;
    (void)ticks;

    if (wdt_disabled) return;

    elapsed = tick_count - wdt_last_kick;
    if (elapsed <= WATCHDOG_SCHED_TIMEOUT) {
        wdt_stall_strikes = 0;
    }

    if (elapsed > WATCHDOG_SCHED_TIMEOUT) {
        __asm__ volatile ("pushf; pop %0" : "=r"(flags) : : "memory");
        cpu = smp_this_cpu();
        wdt_stall_strikes++;
        printf("WATCHDOG: scheduler stall detected (%u ms, strike %u/%u, lock=%d, irq=%d)\n",
               elapsed, wdt_stall_strikes, WATCHDOG_MAX_STRIKES,
               cpu ? cpu->scheduler_lock_depth : -1,
               (flags & (1ULL << 9)) != 0);
        watchdog_dump_tasks();
        if (wdt_stall_strikes < WATCHDOG_MAX_STRIKES) {
            if (current_task && current_task->pid > 1 && current_task->is_user) {
                printf("WATCHDOG: killing stalled task PID=%d (%s)\n",
                       (int)current_task->pid,
                       current_task->name[0] ? current_task->name : "(none)");
                task_kill(current_task, 137);
            }
            wdt_last_kick = tick_count;
        } else {
            kernel_panic_msg("WATCHDOG: scheduler stall unrecoverable (%u ms, %u strikes)",
                             elapsed, wdt_stall_strikes);
        }
    }

    if (wdt_init_pid <= 0) return;

    lookup_complete = task_find_from_irq((pid_t)wdt_init_pid, &t);
    if (!lookup_complete) return;
    if (t) {
        if (t->state == TASK_DEAD) {
            kernel_panic_msg("WATCHDOG: init (PID 1) has exited");
        }
        return;
    }

    kernel_panic_msg("WATCHDOG: init (PID %d) not found", wdt_init_pid);
}

void KERNEL_INIT watchdog_init(void) {
    uint64_t interval_ticks;
    int handle;

    wdt_last_kick = tick_count;
    wdt_stall_strikes = 0;
    wdt_init_pid = 0;
    interval_ticks = pit_ms_to_ticks(WATCHDOG_INTERVAL_MS);
    if (interval_ticks == 0)
        interval_ticks = WATCHDOG_INTERVAL_MS;

    handle = pit_register_callback(watchdog_callback, interval_ticks, false);
    if (handle < 0) {
        printf("WATCHDOG: failed to register timer callback\n");
        return;
    }

}
