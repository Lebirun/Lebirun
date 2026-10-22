#include "syscalls/syscall_defs.h"
#include <lebirun/debug.h>
#include <lebirun/creds.h>
#include <lebirun/smp.h>
#include <sys/ptrace.h>

#if CONFIG_KERNEL_PTRACE
typedef struct task_trace {
    task_t *task;
    pid_t tracer;
    pid_t parent;
    registers_t *frame;
    int stop_signal;
    int reported;
    int pending_stop;
    int resume_signal;
    int skip_signal;
    int busy;
    int kill_pending;
    uint64_t kill_code;
    struct task_trace *next;
} task_trace_t;

static task_trace_t *task_traces;

static task_trace_t *task_trace_find(task_t *task) {
    task_trace_t *trace;

    trace = task_traces;
    while (trace && trace->task != task) trace = trace->next;
    return trace;
}

static void task_trace_remove_locked(task_trace_t *trace) {
    task_trace_t **link;

    link = &task_traces;
    while (*link && *link != trace) link = &(*link)->next;
    if (*link) {
        __atomic_store_n(link, trace->next, __ATOMIC_RELEASE);
        kfree(trace);
    }
}

#endif

void task_debug_registers(task_t *task, registers_t *frame,
                           debug_registers_t *registers) {
    memset(registers, 0, sizeof(*registers));
    registers->r15 = frame->r15;
    registers->r14 = frame->r14;
    registers->r13 = frame->r13;
    registers->r12 = frame->r12;
    registers->rbp = frame->rbp;
    registers->rbx = frame->rbx;
    registers->r11 = frame->r11;
    registers->r10 = frame->r10;
    registers->r9 = frame->r9;
    registers->r8 = frame->r8;
    registers->rax = frame->rax;
    registers->rcx = frame->rcx;
    registers->rdx = frame->rdx;
    registers->rsi = frame->rsi;
    registers->rdi = frame->rdi;
    registers->orig_rax = UINT64_MAX;
    registers->rip = frame->rip;
    registers->cs = frame->cs;
    registers->eflags = frame->rflags;
    registers->rsp = frame->rsp;
    registers->ss = frame->ss;
    registers->fs_base = task->tls_base;
    registers->ds = frame->ds;
    registers->es = frame->es;
}

#if CONFIG_KERNEL_PTRACE
static int task_debug_set_registers(task_t *task, registers_t *frame,
                                     const debug_registers_t *registers) {
    if (registers->rip < PAGE_SIZE ||
        registers->rip >= 0x0000800000000000ULL ||
        registers->rsp >= 0x0000800000000000ULL ||
        registers->fs_base >= 0x0000800000000000ULL ||
        registers->gs_base || registers->fs || registers->gs ||
        registers->cs != USER_CS || registers->ss != USER_DS ||
        (registers->ds && registers->ds != USER_DS) ||
        (registers->es && registers->es != USER_DS)) return -EIO;
    frame->r15 = registers->r15;
    frame->r14 = registers->r14;
    frame->r13 = registers->r13;
    frame->r12 = registers->r12;
    frame->rbp = registers->rbp;
    frame->rbx = registers->rbx;
    frame->r11 = registers->r11;
    frame->r10 = registers->r10;
    frame->r9 = registers->r9;
    frame->r8 = registers->r8;
    frame->rax = registers->rax;
    frame->rcx = registers->rcx;
    frame->rdx = registers->rdx;
    frame->rsi = registers->rsi;
    frame->rdi = registers->rdi;
    frame->rip = registers->rip;
    frame->rflags = (frame->rflags & ~0xDD5ULL) |
                    (registers->eflags & 0xDD5ULL) | 0x202ULL;
    frame->rsp = registers->rsp;
    frame->ds = registers->ds;
    frame->es = registers->es;
    task->tls_base = registers->fs_base;
    return 0;
}

int task_is_traced(task_t *task) {
    int traced;
    task_trace_t *trace;

    if (!__atomic_load_n(&task_traces, __ATOMIC_ACQUIRE)) return 0;
    lock_scheduler();
    trace = task_trace_find(task);
    traced = trace && trace->tracer != 0;
    unlock_scheduler();
    return traced;
}

int task_debug_busy(task_t *task) {
    task_trace_t *trace;
    int busy;

    if (!__atomic_load_n(&task_traces, __ATOMIC_ACQUIRE)) return 0;
    lock_scheduler();
    trace = task_trace_find(task);
    busy = trace && trace->busy;
    unlock_scheduler();
    return busy;
}

int task_debug_defer_kill(task_t *task, uint64_t exit_code) {
    task_trace_t *trace;
    int deferred;

    if (!__atomic_load_n(&task_traces, __ATOMIC_ACQUIRE)) return 0;
    deferred = 0;
    lock_scheduler();
    trace = task_traces;
    while (trace) {
        if (trace->tracer == task->pid && trace->busy) {
            trace->kill_pending = 1;
            trace->kill_code = exit_code;
            deferred = 1;
            break;
        }
        trace = trace->next;
    }
    unlock_scheduler();
    return deferred;
}

int task_trace_signal(registers_t *frame, int signal) {
    task_trace_t *trace;
    task_t *tracer;
    int consumed;

    if (!__atomic_load_n(&task_traces, __ATOMIC_ACQUIRE) ||
        !current_task || !frame || (frame->cs & 3) != 3 || signal == 9)
        return 0;
    lock_scheduler();
    trace = task_trace_find(current_task);
    if (trace && !trace->tracer) {
        consumed = signal == 5 && frame->int_no == 1;
        frame->rflags &= ~0x100ULL;
        task_trace_remove_locked(trace);
        unlock_scheduler();
        return consumed;
    }
    if (!trace || trace->skip_signal == signal) {
        if (trace) trace->skip_signal = 0;
        unlock_scheduler();
        return 0;
    }
    trace->frame = frame;
    trace->stop_signal = signal;
    trace->reported = 0;
    trace->pending_stop = 0;
    frame->rflags &= ~0x100ULL;
    current_task->state = TASK_STOPPED;
    tracer = task_find(trace->tracer);
    if (tracer) {
        wake_task(tracer);
        deliver_signal_to_task(tracer, 17);
    }
    unlock_scheduler();
    return 1;
}

int task_trace_resume(registers_t *frame) {
    task_trace_t *trace;
    int pending;
    int signal;

    if (!__atomic_load_n(&task_traces, __ATOMIC_ACQUIRE) ||
        !current_task || !frame || (frame->cs & 3) != 3) return 0;
    lock_scheduler();
    trace = task_trace_find(current_task);
    if (trace && !trace->tracer) {
        frame->rflags &= ~0x100ULL;
        task_trace_remove_locked(trace);
        unlock_scheduler();
        return 0;
    }
    pending = trace ? trace->pending_stop : 0;
    signal = trace ? trace->resume_signal : 0;
    if (trace) {
        trace->resume_signal = 0;
        if (signal) trace->skip_signal = signal;
    }
    unlock_scheduler();
    if (pending) return task_trace_signal(frame, pending);
    if (signal) deliver_signal_to_task(current_task, signal);
    return 0;
}

int task_trace_stop_pending(task_t *task) {
    task_trace_t *trace;
    int pending;

    if (!__atomic_load_n(&task_traces, __ATOMIC_ACQUIRE)) return 0;
    lock_scheduler();
    trace = task_trace_find(task);
    pending = trace && current_task && trace->tracer == current_task->pid &&
              task->state == TASK_STOPPED && !trace->reported;
    unlock_scheduler();
    return pending;
}

int task_trace_take_stop(task_t *task) {
    task_trace_t *trace;
    int signal;

    if (!__atomic_load_n(&task_traces, __ATOMIC_ACQUIRE)) return 0;
    lock_scheduler();
    trace = task_trace_find(task);
    signal = 0;
    if (trace && current_task && trace->tracer == current_task->pid &&
        task->state == TASK_STOPPED && !trace->reported) {
        signal = trace->stop_signal;
        trace->reported = 1;
    }
    unlock_scheduler();
    return signal;
}

void task_trace_exec(void) {
    task_trace_t *trace;

    if (!__atomic_load_n(&task_traces, __ATOMIC_ACQUIRE)) return;
    lock_scheduler();
    trace = task_trace_find(current_task);
    if (trace && trace->tracer) trace->pending_stop = 5;
    unlock_scheduler();
}

void task_debug_exit(task_t *task) {
    task_trace_t **link;
    task_trace_t *trace;

    if (!__atomic_load_n(&task_traces, __ATOMIC_ACQUIRE)) return;
    lock_scheduler();
    link = &task_traces;
    while (*link) {
        trace = *link;
        if (trace->tracer != task->pid) {
            link = &trace->next;
            continue;
        }
        trace->task->ppid = trace->parent;
        if (trace->task->state != TASK_STOPPED &&
            trace->task->state != TASK_DEAD) {
            trace->tracer = 0;
            trace->pending_stop = 0;
            trace->resume_signal = 0;
            trace->skip_signal = 0;
            link = &trace->next;
            continue;
        }
        if (trace->task->state == TASK_STOPPED) {
            if (trace->frame) trace->frame->rflags &= ~0x100ULL;
            trace->task->state = TASK_READY;
        }
        __atomic_store_n(link, trace->next, __ATOMIC_RELEASE);
        kfree(trace);
    }
    unlock_scheduler();
}

void task_debug_release(task_t *task) {
    task_trace_t *trace;

    if (!__atomic_load_n(&task_traces, __ATOMIC_ACQUIRE)) return;
    lock_scheduler();
    trace = task_trace_find(task);
    if (trace) task_trace_remove_locked(trace);
    unlock_scheduler();
}

static int task_debug_memory(task_trace_t *trace, uint64_t address,
                               uint64_t *word, int write) {
    task_t *task;
    task_t *other;
    uint64_t page;
    uint64_t physical;
    uint64_t original;
    uint64_t flags;
    uint64_t bytes;
    uint64_t offset;
    uint64_t copied;
    uint64_t temporary;
    uint64_t saved_flags;
    uint64_t kill_code;
    int result;

    task = trace->task;
    if (address < PAGE_SIZE || address > 0x0000800000000000ULL - sizeof(*word))
        return -EIO;
    other = all_tasks_head;
    while (other) {
        if (other != task && other->state != TASK_DEAD &&
            other->pml4_phys == task->pml4_phys) return -EBUSY;
        other = other->all_next;
    }
    trace->busy++;
    unlock_scheduler();
    result = 0;
    copied = 0;
    while (copied < sizeof(*word)) {
        page = (address + copied) & ~(PAGE_SIZE - 1);
        offset = (address + copied) & (PAGE_SIZE - 1);
        bytes = PAGE_SIZE - offset;
        if (bytes > sizeof(*word) - copied) bytes = sizeof(*word) - copied;
        physical = vmm_get_phys_in_pml4(task->pml4_phys, page);
        if (!physical && !task_handle_file_page_fault(task, page) &&
            !task_handle_anon_page_fault(task, page, 0)) {
            result = -EIO;
            break;
        }
        lock_scheduler();
        if (task->state != TASK_STOPPED || task->resources_released) {
            unlock_scheduler();
            result = -ESRCH;
            break;
        }
        original = vmm_get_phys_in_pml4(task->pml4_phys, page);
        flags = vmm_get_flags_in_pml4(task->pml4_phys, page);
        if (!original || !(flags & VMM_PTE_USER) ||
            (flags & VMM_PTE_SHARED)) {
            unlock_scheduler();
            result = -EIO;
            break;
        }
        if (write) {
            physical = pfa_alloc();
            if (!physical) {
                unlock_scheduler();
                result = -ENOMEM;
                break;
            }
            __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");
            temporary = TEMP_SLOT(6);
            vmm_temp_map_raw(temporary, original);
            pmm_copy_to_page_phys(physical, 0, (void *)temporary, PAGE_SIZE);
            vmm_temp_unmap_raw(temporary);
            __asm__ volatile ("push %0; popfq" :: "r"(saved_flags) : "memory", "cc");
            pmm_copy_to_page_phys(physical, offset,
                                  (uint8_t *)word + copied, bytes);
            if (vmm_map_page_in_pml4(task->pml4_phys, page, physical,
                    (flags | ((flags & VMM_PTE_COW) ? VMM_PTE_WRITE : 0)) &
                    ~(VMM_PTE_COW | VMM_PTE_NOFREE)) != 0) {
                pfa_free(physical);
                unlock_scheduler();
                result = -ENOMEM;
                break;
            }
            if (task_replace_user_page(task, original, physical) != 0) {
                vmm_map_page_in_pml4(task->pml4_phys, page, original, flags);
                pfa_free(physical);
                unlock_scheduler();
                result = -ENOMEM;
                break;
            }
            if (!(flags & VMM_PTE_NOFREE)) pfa_cow_release64(original);
        } else {
            vmm_read_from_pml4(task->pml4_phys, address + copied,
                               (uint8_t *)word + copied, bytes);
        }
        unlock_scheduler();
        copied += bytes;
    }
    lock_scheduler();
    trace->busy--;
    if (trace->kill_pending) {
        kill_code = trace->kill_code;
        trace->kill_pending = 0;
        unlock_scheduler();
        task_exit(kill_code);
        lock_scheduler();
    }
    return result;
}

int64_t task_ptrace(uint64_t request, uint64_t pid, uint64_t address,
                    uint64_t data, uint64_t unused1, uint64_t unused2) {
    task_trace_t *trace;
    task_trace_t *created;
    task_t *task;
    task_t *ancestor;
    debug_registers_t registers;
    uint64_t word;
    uint8_t fpregs[512];
    uint32_t mxcsr;
    int result;
    int regset;
    int get_regset;
    size_t regset_size;
    uint64_t iovec_address;
    struct {
        uint64_t base;
        uint64_t length;
    } iovec;

    (void)unused1;
    (void)unused2;
    if (!current_task) return -ESRCH;
    regset = request == PTRACE_GETREGSET || request == PTRACE_SETREGSET;
    get_regset = request == PTRACE_GETREGSET;
    iovec_address = data;
    regset_size = 0;
    if (regset) {
        if (address != 1 && address != 2) return -EINVAL;
        if (copy_from_user(&iovec, (void *)(uintptr_t)data,
                           sizeof(iovec)) < 0) return -EFAULT;
        regset_size = address == 1 ? sizeof(registers) : sizeof(fpregs);
        if (!get_regset && iovec.length < regset_size) return -EIO;
        if (get_regset && iovec.length < regset_size)
            regset_size = (size_t)iovec.length;
        data = iovec.base;
        request = address == 1 ?
                  (get_regset ? PTRACE_GETREGS : PTRACE_SETREGS) :
                  (get_regset ? PTRACE_GETFPREGS : PTRACE_SETFPREGS);
    }
    created = NULL;
    if (request == PTRACE_TRACEME || request == PTRACE_ATTACH) {
        created = kmalloc(sizeof(*created));
        if (!created) return -ENOMEM;
        memset(created, 0, sizeof(*created));
    }
    if (request == PTRACE_SETREGS &&
        copy_from_user(&registers, (void *)(uintptr_t)data,
                       sizeof(registers)) < 0) return -EFAULT;
    if (request == PTRACE_SETFPREGS &&
        copy_from_user(fpregs, (void *)(uintptr_t)data, sizeof(fpregs)) < 0)
        return -EFAULT;
retry:
    lock_scheduler();
    result = -ESRCH;
    if (current_task->state == TASK_DEAD) goto out;
    task = request == PTRACE_TRACEME ? current_task : task_find((pid_t)pid);
    if (!task || !task->is_user || task->pid <= 0 ||
        task->state == TASK_DEAD || task->resources_released) goto out;
    trace = task_trace_find(task);
    if (request == PTRACE_TRACEME || request == PTRACE_ATTACH) {
        result = -EPERM;
        if (trace) goto out;
        if (request == PTRACE_ATTACH) {
            if (task == current_task) goto out;
            if (!creds_has_capability(current_task, 19) &&
                (!creds_get_dumpable(task) ||
                 current_task->uid != task->uid ||
                 current_task->uid != task->euid ||
                 current_task->uid != task->suid ||
                 current_task->gid != task->gid ||
                 current_task->gid != task->egid ||
                 current_task->gid != task->sgid)) goto out;
            ancestor = current_task;
            while (ancestor && ancestor->ppid > 0) {
                if (ancestor->ppid == task->pid) goto out;
                ancestor = task_find(ancestor->ppid);
            }
        } else if (task->ppid <= 0 || !task_find(task->ppid)) {
            goto out;
        }
        created->task = task;
        created->tracer = request == PTRACE_TRACEME ? task->ppid :
                          current_task->pid;
        created->parent = task->ppid;
        created->pending_stop = request == PTRACE_ATTACH ? 19 : 0;
        created->next = task_traces;
        __atomic_store_n(&task_traces, created, __ATOMIC_RELEASE);
        task->ppid = created->tracer;
        created = NULL;
        if (request == PTRACE_ATTACH) wake_task(task);
        result = 0;
        goto out;
    }
    if (!trace || trace->tracer != current_task->pid ||
        task->state != TASK_STOPPED || !trace->frame) goto out;
    if (task->running_cpu >= 0) {
        unlock_scheduler();
        schedule();
        goto retry;
    }
    result = 0;
    switch (request) {
        case PTRACE_PEEKTEXT:
        case PTRACE_PEEKDATA:
            result = task_debug_memory(trace, address, &word, 0);
            break;
        case PTRACE_POKETEXT:
        case PTRACE_POKEDATA:
            word = data;
            result = task_debug_memory(trace, address, &word, 1);
            break;
        case PTRACE_PEEKUSER:
            if (address & 7 || address >= sizeof(registers)) {
                result = -EIO;
                break;
            }
            task_debug_registers(task, trace->frame, &registers);
            memcpy(&word, (uint8_t *)&registers + address, sizeof(word));
            break;
        case PTRACE_GETREGS:
            task_debug_registers(task, trace->frame, &registers);
            break;
        case PTRACE_SETREGS:
            result = task_debug_set_registers(task, trace->frame, &registers);
            break;
        case PTRACE_GETFPREGS:
            if (task->fpu_state) memcpy(fpregs, task->fpu_state, sizeof(fpregs));
            else memset(fpregs, 0, sizeof(fpregs));
            break;
        case PTRACE_SETFPREGS:
            memcpy(&mxcsr, fpregs + 24, sizeof(mxcsr));
            if (!task->fpu_state || (mxcsr & ~0xFFBFU)) {
                result = -EIO;
                break;
            }
            memcpy(task->fpu_state, fpregs, sizeof(fpregs));
            break;
        case PTRACE_CONT:
        case PTRACE_SINGLESTEP:
        case PTRACE_DETACH:
            if (data >= 64) {
                result = -EIO;
                break;
            }
            trace->resume_signal = (int)data;
            trace->frame->rflags &= ~0x100ULL;
            if (request == PTRACE_SINGLESTEP) trace->frame->rflags |= 0x100ULL;
            if (request == PTRACE_DETACH) {
                task->ppid = trace->parent;
                task_trace_remove_locked(trace);
                if (data) deliver_signal_to_task(task, (int)data);
            }
            task->state = TASK_READY;
            break;
        case PTRACE_KILL:
            task_kill(task, 137);
            break;
        case PTRACE_SETOPTIONS:
            if (data) result = -EINVAL;
            break;
        default:
            result = -EIO;
            break;
    }
out:
    unlock_scheduler();
    kfree(created);
    if (result) return result;
    if (regset) {
        if (get_regset && copy_to_user((void *)(uintptr_t)data,
                request == PTRACE_GETREGS ? (void *)&registers : (void *)fpregs,
                regset_size) < 0) return -EFAULT;
        iovec.length = regset_size;
        return copy_to_user((void *)(uintptr_t)iovec_address, &iovec,
                             sizeof(iovec)) < 0 ? -EFAULT : 0;
    }
    if (request >= PTRACE_PEEKTEXT && request <= PTRACE_PEEKUSER)
        return copy_to_user((void *)(uintptr_t)data, &word, sizeof(word)) < 0 ?
               -EFAULT : 0;
    if (request == PTRACE_GETREGS)
        return copy_to_user((void *)(uintptr_t)data, &registers,
                             sizeof(registers)) < 0 ? -EFAULT : 0;
    if (request == PTRACE_GETFPREGS)
        return copy_to_user((void *)(uintptr_t)data, fpregs, sizeof(fpregs)) < 0 ?
               -EFAULT : 0;
    return 0;
}

#endif

void task_user_exception(registers_t *frame, int signal) {
    uint64_t exit_code;

    if (task_trace_signal(frame, signal)) return;
    exit_code = 128 + signal;
    if (task_core_dump(frame, signal)) exit_code |= TASK_WAIT_CORE;
    task_exit_deferred(exit_code);
}
