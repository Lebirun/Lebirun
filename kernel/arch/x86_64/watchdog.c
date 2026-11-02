#include <lebirun/watchdog.h>
#include <lebirun/pit.h>
#include <lebirun/task.h>
#include <lebirun/panic.h>
#include <lebirun/common.h>
#include <lebirun/smp.h>
#include <lebirun/vfs.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

static volatile uint64_t wdt_last_kick = 0;
static uint64_t wdt_delivered_ticks;
static volatile uint64_t wdt_stall_strikes = 0;
static volatile int wdt_disabled = 0;
static volatile int wdt_init_pid = 0;
static registers_t *wdt_regs;

extern task_t *all_tasks_head;
extern char _kernel_text_start[];
extern char _kernel_text_end[];
extern void serial_write_direct(const char *buf, size_t len);

void watchdog_tick(registers_t *regs) {
    wdt_regs = regs;
    __atomic_add_fetch(&wdt_delivered_ticks, 1, __ATOMIC_RELAXED);
}

static void wdt_log(const char *fmt, ...) {
    char buf[192];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    if (n > 0) serial_write_direct(buf, (size_t)n);
}

void watchdog_kick(void) {
    wdt_last_kick = __atomic_load_n(&wdt_delivered_ticks, __ATOMIC_RELAXED);
}

void watchdog_disable(void) {
    wdt_disabled = 1;
}

void KERNEL_INIT watchdog_set_init_pid(int pid) {
    wdt_init_pid = pid;
}

static void watchdog_print_task_state(task_t *t) {
    const char *state_name;
    registers_t *frame;

    switch (t->state) {
    case TASK_READY:   state_name = "READY";   break;
    case TASK_RUNNING: state_name = "RUNNING"; break;
    case TASK_BLOCKED: state_name = "BLOCKED"; break;
    case TASK_STOPPED: state_name = "STOPPED"; break;
    case TASK_DEAD:    state_name = "DEAD";    break;
    default:           state_name = "UNKNOWN"; break;
    }
    frame = t->syscall_frame;
    wdt_log("  PID=%d name=%s state=%s syscall=%lu rip=0x%lX min=%lu maj=%lu stage=%u lookup=0x%lX\n",
            (int)t->pid, t->name[0] ? t->name : "(none)",
            state_name,
            frame ? frame->rax : UINT64_MAX,
            frame ? frame->rip : t->regs.rip,
            t->minor_faults, t->major_faults,
            t->kernel_stage,
            (uint64_t)(uintptr_t)__atomic_load_n(&t->vfs_lookup_node, __ATOMIC_ACQUIRE));
}

static void watchdog_dump_tasks(void) {
    task_t *t;
    int count;

    t = all_tasks_head;
    count = 0;
    wdt_log("WATCHDOG: active tasks:\n");
    while (t && count < 32) {
        watchdog_print_task_state(t);
        t = t->all_next;
        count++;
    }
    if (t) wdt_log("  ... (more tasks)\n");
}

static void watchdog_dump_cpus(void) {
    cpu_info_t *c;
    task_t *t;
    int i;

    for (i = 0; cpus && i < cpu_count; i++) {
        c = &cpus[i];
        t = c->running_task;
        wdt_log("  CPU%d apic=%lu active=%d running=%d (%s) tick_stopped=%d lock=%d tlb=%lu/%lu\n",
                i, c->lapic_id, c->active, t ? (int)t->pid : -1,
                t && t->name[0] ? t->name : "-", c->tick_stopped,
                c->scheduler_lock_depth, c->tlb_request, c->tlb_ack);
    }
}

static void watchdog_dump_frame(void) {
    registers_t *r = wdt_regs;
    task_t *t = current_task;
    uint64_t *p;
    uint64_t *top;
    int n = 0;

    if (!r) return;
    wdt_log("WATCHDOG: interrupted rip=0x%lX cs=0x%lX rsp=0x%lX rflags=0x%lX pid=%d (%s) syscall=%lu\n",
            r->rip, r->cs, r->rsp, r->rflags, t ? (int)t->pid : -1,
            t && t->name[0] ? t->name : "-",
            t && t->syscall_frame ? t->syscall_frame->rax : UINT64_MAX);
    if (r->cs & 3) return;
    p = (uint64_t *)(uintptr_t)r->rsp;
    top = (uint64_t *)(((uintptr_t)p + 0x1000) & ~(uintptr_t)0xFFF);
    if (t && t->kernel_stack_base && (uint8_t *)p >= t->kernel_stack_base &&
        (uint8_t *)p < t->kernel_stack_base + t->kernel_stack_size)
        top = (uint64_t *)(t->kernel_stack_base + t->kernel_stack_size);
    wdt_log("WATCHDOG: stack text addrs:");
    for (; p < top && n < 24; p++) {
        if (*p >= (uint64_t)(uintptr_t)_kernel_text_start &&
            *p < (uint64_t)(uintptr_t)_kernel_text_end) {
            wdt_log(" 0x%lX", *p);
            n++;
        }
    }
    wdt_log("\n");
}

static void watchdog_callback(uint64_t ticks) {
    task_t *t;
    cpu_info_t *cpu;
    uint64_t elapsed;
    uint64_t flags;
    int lookup_complete;
    (void)ticks;

    if (wdt_disabled) return;

    elapsed = pit_ticks_to_ms(
        __atomic_load_n(&wdt_delivered_ticks, __ATOMIC_RELAXED) - wdt_last_kick);
    if (elapsed <= WATCHDOG_SCHED_TIMEOUT) {
        wdt_stall_strikes = 0;
    }

    if (elapsed > WATCHDOG_SCHED_TIMEOUT) {
        __asm__ volatile ("pushf; pop %0" : "=r"(flags) : : "memory");
        cpu = smp_this_cpu();
        wdt_stall_strikes++;
        wdt_log("WATCHDOG: scheduler stall detected (%u ms, strike %u/%u, lock=%d, irq=%d)\n",
                elapsed, wdt_stall_strikes, WATCHDOG_MAX_STRIKES,
                cpu ? cpu->scheduler_lock_depth : -1,
                (flags & (1ULL << 9)) != 0);
        watchdog_dump_frame();
        watchdog_dump_cpus();
        watchdog_dump_tasks();
        if (wdt_stall_strikes < WATCHDOG_MAX_STRIKES) {
            if (current_task && current_task->pid > 1 && current_task->is_user) {
                wdt_log("WATCHDOG: killing stalled task PID=%d (%s)\n",
                        (int)current_task->pid,
                        current_task->name[0] ? current_task->name : "(none)");
                task_kill(current_task, 137);
            }
            watchdog_kick();
            printf("WATCHDOG: scheduler stall detected (%u ms, strike %u/%u)\n",
                   elapsed, wdt_stall_strikes, WATCHDOG_MAX_STRIKES);
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

    watchdog_kick();
    wdt_stall_strikes = 0;
    wdt_init_pid = 0;
    interval_ticks = pit_ms_to_ticks(WATCHDOG_INTERVAL_MS);
    if (interval_ticks == 0)
        interval_ticks = WATCHDOG_INTERVAL_MS;

    handle = pit_register_callback(watchdog_callback, interval_ticks, false);
    if (handle < 0) {
        KERNEL_INIT_LOG("WATCHDOG: failed to register timer callback\n");
        return;
    }

}
