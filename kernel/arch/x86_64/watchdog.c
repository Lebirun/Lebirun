#include <kernel/watchdog.h>
#include <kernel/pit.h>
#include <kernel/task.h>
#include <kernel/panic.h>
#include <kernel/common.h>
#include <stdint.h>
#include <stdbool.h>

static volatile uint64_t wdt_last_kick = 0;
static volatile uint64_t wdt_last_sched_tick = 0;
static volatile uint64_t wdt_prev_tick_count = 0;
static volatile uint64_t wdt_stall_strikes = 0;
static int wdt_handle = -1;

extern volatile uint64_t tick_count;
extern task_t *all_tasks_head;

void watchdog_kick(void) {
    wdt_last_kick = tick_count;
}

uint64_t watchdog_get_last_kick(void) {
    return wdt_last_kick;
}

static void watchdog_print_task_state(task_t *t) {
    const char *state_name;

    switch (t->state) {
    case TASK_READY:   state_name = "READY";   break;
    case TASK_RUNNING: state_name = "RUNNING"; break;
    case TASK_BLOCKED: state_name = "BLOCKED"; break;
    case TASK_STOPPED: state_name = "STOPPED"; break;
    case TASK_DEAD:    state_name = "DEAD";    break;
    default:           state_name = "UNKNOWN"; break;
    }
    printf("  PID=%d name=%s state=%s console=%d is_user=%d\n",
           (int)t->pid, t->name[0] ? t->name : "(none)",
           state_name, t->console_id, t->is_user);
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
    uint64_t elapsed;
    int limit;
    (void)ticks;

    if (tick_count != wdt_prev_tick_count) {
        wdt_last_sched_tick = tick_count;
        wdt_prev_tick_count = tick_count;
        wdt_stall_strikes = 0;
    }

    elapsed = tick_count - wdt_last_sched_tick;
    if (elapsed > WATCHDOG_SCHED_TIMEOUT) {
        wdt_stall_strikes++;
        printf("WATCHDOG: scheduler stall detected (%u ms, strike %u/%u)\n",
               elapsed, wdt_stall_strikes, WATCHDOG_MAX_STRIKES);
        watchdog_dump_tasks();
        if (wdt_stall_strikes < WATCHDOG_MAX_STRIKES) {
            if (current_task && current_task->pid > 1 && current_task->is_user) {
                printf("WATCHDOG: killing stalled task PID=%d (%s)\n",
                       (int)current_task->pid,
                       current_task->name[0] ? current_task->name : "(none)");
                task_kill(current_task, 137);
            }
            wdt_last_sched_tick = tick_count;
        } else {
            kernel_panic_msg("WATCHDOG: scheduler stall unrecoverable (%u ms, %u strikes)",
                             elapsed, wdt_stall_strikes);
        }
    }

    t = all_tasks_head;
    limit = 2048;
    while (t && limit > 0) {
        if (t->pid == 1) {
            if (t->state == TASK_DEAD) {
                kernel_panic_msg("WATCHDOG: init (PID 1) has exited");
            }
            return;
        }
        t = t->all_next;
        limit--;
    }

    if (tick_count > 10000) {
        kernel_panic_msg("WATCHDOG: init (PID 1) not found");
    }
}

void watchdog_init(void) {
    uint64_t interval_ticks;

    wdt_last_kick = tick_count;
    wdt_last_sched_tick = tick_count;
    wdt_prev_tick_count = tick_count;
    wdt_stall_strikes = 0;

    interval_ticks = pit_ms_to_ticks(WATCHDOG_INTERVAL_MS);
    if (interval_ticks == 0)
        interval_ticks = 5000;

    wdt_handle = pit_register_callback(watchdog_callback, interval_ticks, false);
    if (wdt_handle < 0) {
        printf("WATCHDOG: failed to register timer callback\n");
        return;
    }

    printf("WATCHDOG: initialized, interval=%ums, sched_timeout=%ums, max_strikes=%u\n",
           WATCHDOG_INTERVAL_MS, WATCHDOG_SCHED_TIMEOUT, WATCHDOG_MAX_STRIKES);
}
