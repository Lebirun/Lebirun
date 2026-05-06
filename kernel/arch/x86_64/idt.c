#include <string.h>
#include <stdint.h>
#include <lebirun/tty.h>
#include <lebirun/debug.h>
#include <lebirun/keyboard.h>
#include <lebirun/console.h>
#include <lebirun/task.h>
#include <lebirun/syscall.h>
#include <lebirun/io.h>
#include <lebirun/mem_map.h>
#include <lebirun/registers.h>
#include <lebirun/idt.h>
#include <lebirun/vring.h>
#include <lebirun/panic.h>
#include <lebirun/smp.h>
#include <lebirun/pit.h>
#include <lebirun/common.h>
#include <lebirun/kstack.h>

extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);
extern void isr32(void);
extern void isr33(void);
extern void isr34(void);
extern void isr35(void);
extern void isr36(void);
extern void isr37(void);
extern void isr38(void);
extern void isr39(void);
extern void isr40(void);
extern void isr41(void);
extern void isr42(void);
extern void isr43(void);
extern void isr44(void);
extern void isr45(void);
extern void isr46(void);
extern void isr47(void);
extern void isr48(void);
extern void isr49(void);
extern void isr128(void);
extern void isr255(void);

volatile uint64_t tick_count = 0;
volatile uint64_t cpu_user_ticks = 0;
volatile uint64_t cpu_system_ticks = 0;
volatile uint64_t cpu_idle_ticks = 0;

#define MAX_IRQ_HANDLERS 16
static irq_handler_t irq_handlers[MAX_IRQ_HANDLERS] = {0};

void irq_register_handler(uint8_t irq, irq_handler_t handler) {
    if (irq < MAX_IRQ_HANDLERS) {
        irq_handlers[irq] = handler;
    }
}

void irq_unregister_handler(uint8_t irq) {
    if (irq < MAX_IRQ_HANDLERS) {
        irq_handlers[irq] = NULL;
    }
}

void irq_unmask(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    if (irq < 8) {
        port = 0x21;
    } else {
        port = 0xA1;
        irq -= 8;
    }
    value = inb(port) & ~(1 << irq);
    outb(port, value);
}

void irq_mask(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    if (irq < 8) {
        port = 0x21;
    } else {
        port = 0xA1;
        irq -= 8;
    }
    value = inb(port) | (1 << irq);
    outb(port, value);
}

static int is_usermode_exception(registers_t *regs) {
    if ((regs->cs & 0x3) == 3) return 1;
    if (current_task && current_task->is_user) {
        if (regs->rip < KERNEL_VMA) return 1;
    }
    return 0;
}

registers_t* interrupt_handler(registers_t* regs)
{
    uint64_t fault_addr;
    uint64_t orig_cr3;
    uint64_t kernel_cr3;
    uint8_t access_type;
    uint8_t irq;
    int sig;
    task_t *fault_task;

    if (regs->int_no < 32) {
        if (regs->int_no == 14) {
            __asm__ ("movq %%cr2, %0" : "=r" (fault_addr));
            access_type = 0;
            if (regs->err_code & 0x2) access_type |= VRING_PERM_WRITE;
            else access_type |= VRING_PERM_READ;
            if (regs->err_code & 0x10) access_type |= VRING_PERM_EXEC;

            if (current_task && current_task->vring_minor != 0 && !current_task->is_user) {
                if (!vring_check_access(current_task->vring_minor, fault_addr, PAGE_SIZE, access_type)) {
                    vring_handle_violation(current_task->vring_minor, fault_addr, access_type);
                    return schedule_from_irq(regs);
                }
            }

            fault_task = task_find_by_pml4(regs->entry_cr3);
            if (fault_task && fault_task->vring_minor != 0 && !fault_task->is_user) {
                if (!vring_check_access(fault_task->vring_minor, fault_addr, PAGE_SIZE, access_type)) {
                    current_task = fault_task;
                    smp_this_cpu()->current_task = fault_task;
                    vring_handle_violation(fault_task->vring_minor, fault_addr, access_type);
                    return schedule_from_irq(regs);
                }
            }
            
            if (!(regs->err_code & 0x4)) {
                if (demand_page_fault_handler(fault_addr, regs->err_code)) {
                    return regs;
                }
            }

            {
                extern int kstack_page_fault_handler(uint64_t fault_addr);
                if (kstack_page_fault_handler(fault_addr)) {
                    return regs;
                }
            }
            
            access_type = 0;
            if (regs->err_code & 0x2) access_type |= VRING_PERM_WRITE;
            else access_type |= VRING_PERM_READ;
            if (regs->err_code & 0x10) access_type |= VRING_PERM_EXEC;
            
            if (current_kproc && current_kproc->vring_minor != 0) {
                if (!vring_check_access(current_kproc->vring_minor, fault_addr, PAGE_SIZE, access_type)) {
                    vring_handle_violation(current_kproc->vring_minor, fault_addr, access_type);
                    return schedule_from_irq(regs);
                }
            }
            
            if (current_task && current_task->vring_minor != 0 && !current_task->is_user) {
                if (!vring_check_access(current_task->vring_minor, fault_addr, PAGE_SIZE, access_type)) {
                    vring_handle_violation(current_task->vring_minor, fault_addr, access_type);
                    return schedule_from_irq(regs);
                }
            }

            fault_task = task_find_by_pml4(regs->entry_cr3);
            if (fault_task && fault_task->vring_minor != 0 && !fault_task->is_user) {
                if (!vring_check_access(fault_task->vring_minor, fault_addr, PAGE_SIZE, access_type)) {
                    current_task = fault_task;
                    smp_this_cpu()->current_task = fault_task;
                    vring_handle_violation(fault_task->vring_minor, fault_addr, access_type);
                    return schedule_from_irq(regs);
                }
            }
        }

        if (is_usermode_exception(regs) && current_task && current_task->is_user && regs->int_no != 14) {
            sig = 0;
            switch (regs->int_no) {
                case 0:  sig = 8;  break;
                case 4:  sig = 8;  break;
                case 5:  sig = 5;  break;
                case 6:  sig = 4;  break;
                case 7:  sig = 8;  break;
                case 8:  sig = 6;  break;
                case 10: sig = 11; break;
                case 11: sig = 11; break;
                case 12: sig = 11; break;
                case 13: sig = 11; break;
                case 16: sig = 8;  break;
                case 19: sig = 8;  break;
                default: sig = 6;  break;
            }
            printf("[KERNEL] User exception %d at RIP=0x%016lX sig=%d\n",
                   regs->int_no, regs->rip, sig);
            
            task_exit_deferred(128 + sig);
            return schedule_from_irq(regs);
        }
        
        if (regs->int_no == 14 && (regs->err_code & 0x7) == 0x7 && current_task && current_task->is_user) {
            int cow_result;
            __asm__ ("movq %%cr2, %0" : "=r" (fault_addr));
            cow_result = cow_handle_fault(fault_addr, current_task->pml4_phys);
            if (cow_result == 1) {
                return regs;
            }
            if (task_handle_file_write_fault(current_task, fault_addr)) {
                return regs;
            }
        }

        if (regs->int_no == 14 && (regs->err_code & 0x4) && current_task && current_task->is_user) {
            uint64_t actual_cr3;
            uint64_t expected_pd;
            uint64_t entry_phys;
            uint64_t fault_page;
            uint64_t phys;
            uint64_t stack_floor;
            uint64_t new_phys;
            uint64_t mapped_phys;
            uint64_t *new_user_pages;
            __asm__ ("movq %%cr2, %0" : "=r" (fault_addr));
            actual_cr3 = regs->entry_cr3;
            if (!actual_cr3) __asm__ ("movq %%cr3, %0" : "=r" (actual_cr3));
            expected_pd = current_task->pml4_phys;
            
            if (actual_cr3 != expected_pd) {
                regs->return_cr3 = expected_pd;
                entry_phys = vmm_get_phys_in_pml4(expected_pd, regs->rip & ~0xFFF);
                if (entry_phys) {
                    return regs;
                }
            }
            
            {
                fault_page = fault_addr & ~0xFFFu;
                phys = vmm_get_phys_in_pml4(current_task->pml4_phys, fault_page);
                if (phys == 0) {
                    if (task_handle_file_page_fault(current_task, fault_addr)) {
                        return regs;
                    }
                    stack_floor = 0x00700000u;
                    if (fault_addr >= stack_floor && fault_addr < 0x00800000u) {
                        new_phys = pfa_alloc();
                        if (new_phys != 0) {
                            pmm_zero_page_phys(new_phys);
                            vmm_map_page_in_pml4(current_task->pml4_phys, fault_page, new_phys, 0x7);
                            mapped_phys = vmm_get_phys_in_pml4(current_task->pml4_phys, fault_page);
                            if (mapped_phys != 0) {
                                new_user_pages = (uint64_t *)krealloc(current_task->user_pages, (current_task->user_pages_count + 1) * sizeof(uint64_t));
                                if (new_user_pages) {
                                    current_task->user_pages = new_user_pages;
                                    current_task->user_pages[current_task->user_pages_count] = new_phys;
                                    current_task->user_pages_count++;
                                    current_task->stack_size += PAGE_SIZE;
                                }
                                return regs;
                            }
                            pfa_free(new_phys);
                        }
                    }
                }
            }

            printf("SEGV addr=0x%lX rip=0x%lX err=0x%lX\n", fault_addr, regs->rip, regs->err_code);
            printf("  RAX=0x%lX RBX=0x%lX RCX=0x%lX RDX=0x%lX\n", regs->rax, regs->rbx, regs->rcx, regs->rdx);
            printf("  RSI=0x%lX RDI=0x%lX RSP=0x%lX RBP=0x%lX\n", regs->rsi, regs->rdi, regs->rsp, regs->rbp);
            printf("  R8=0x%lX R9=0x%lX R10=0x%lX R11=0x%lX\n", regs->r8, regs->r9, regs->r10, regs->r11);
            printf("  R12=0x%lX R13=0x%lX R14=0x%lX R15=0x%lX\n", regs->r12, regs->r13, regs->r14, regs->r15);
            if (current_task && regs->rsp >= 0x1000 && regs->rsp < KERNEL_VMA) {
                extern void temp_map_raw(uint64_t, uint64_t);
                extern void temp_unmap_raw(uint64_t);
                uint64_t sp_page;
                uint64_t sp_phys;
                sp_page = regs->rsp & ~0xFFFULL;
                sp_phys = vmm_get_phys_in_pml4(current_task->pml4_phys, sp_page);
                if (sp_phys) {
                    uint64_t tv;
                    uint64_t *kp;
                    int si;
                    uint64_t soff;
                    tv = TEMP_SLOT(1);
                    temp_map_raw(tv, sp_phys);
                    soff = regs->rsp - sp_page;
                    kp = (uint64_t *)(tv + soff);
                    printf("  Stack:");
                    for (si = 0; si < 16 && (soff + si * 8) < 4096; si++) {
                        printf(" 0x%lX", kp[si]);
                    }
                    printf("\n");
                    temp_unmap_raw(tv);
                }
            }
            task_exit_deferred(139);
            return schedule_from_irq(regs);
        }

        if (regs->int_no == 14 && !(regs->err_code & 0x4) && (regs->err_code & 0x3) == 0x3 && current_task && current_task->is_user && current_task->syscall_frame) {
            int sc_cow_result;
            uint64_t sc_cow_addr;
            __asm__ ("movq %%cr2, %0" : "=r" (sc_cow_addr));
            if (sc_cow_addr < KERNEL_VMA) {
                sc_cow_result = cow_handle_fault(sc_cow_addr, current_task->pml4_phys);
                if (sc_cow_result == 1) {
                    return regs;
                }
            }
        }

        if (regs->int_no == 14 && !(regs->err_code & 0x4) && current_task && current_task->is_user && current_task->syscall_frame) {
            uint64_t sc_fault_addr;
            uint64_t sc_fault_page;
            uint64_t sc_phys;
            uint64_t sc_actual_cr3;
            uint64_t sc_expected_pd;
            uint64_t sc_new_phys;
            uint64_t sc_mapped_phys;
            uint64_t *sc_new_user_pages;

            __asm__ ("movq %%cr2, %0" : "=r" (sc_fault_addr));
            if (sc_fault_addr < KERNEL_VMA) {
                sc_fault_page = sc_fault_addr & ~0xFFFu;
                sc_expected_pd = current_task->pml4_phys;
                sc_actual_cr3 = regs->entry_cr3;
                if (!sc_actual_cr3) __asm__ volatile ("mov %%cr3, %0" : "=r"(sc_actual_cr3));

                sc_phys = vmm_get_phys_in_pml4(sc_expected_pd, sc_fault_page);

                if (sc_phys != 0) {
                    if (sc_actual_cr3 != sc_expected_pd) {
                        regs->return_cr3 = sc_expected_pd;
                    } else {
                        __asm__ volatile ("invlpg (%0)" : : "r"(sc_fault_page) : "memory");
                    }
                    return regs;
                }

                if (task_handle_file_page_fault(current_task, sc_fault_addr)) {
                    if (sc_actual_cr3 != sc_expected_pd) {
                        regs->return_cr3 = sc_expected_pd;
                    } else {
                        __asm__ volatile ("invlpg (%0)" : : "r"(sc_fault_page) : "memory");
                    }
                    return regs;
                }

                if (sc_fault_addr >= 0x00700000u && sc_fault_addr < 0x00800000u) {
                    sc_new_phys = pfa_alloc();
                    if (sc_new_phys != 0) {
                        pmm_zero_page_phys(sc_new_phys);
                        vmm_map_page_in_pml4(sc_expected_pd, sc_fault_page, sc_new_phys, 0x7);
                        sc_mapped_phys = vmm_get_phys_in_pml4(sc_expected_pd, sc_fault_page);
                        if (sc_mapped_phys != 0) {
                            sc_new_user_pages = (uint64_t *)krealloc(current_task->user_pages, (current_task->user_pages_count + 1) * sizeof(uint64_t));
                            if (sc_new_user_pages) {
                                current_task->user_pages = sc_new_user_pages;
                                current_task->user_pages[current_task->user_pages_count] = sc_new_phys;
                                current_task->user_pages_count++;
                                current_task->stack_size += PAGE_SIZE;
                            }
                            if (sc_actual_cr3 != sc_expected_pd) {
                                regs->return_cr3 = sc_expected_pd;
                            } else {
                                __asm__ volatile ("invlpg (%0)" : : "r"(sc_fault_page) : "memory");
                            }
                            return regs;
                        }
                        pfa_free(sc_new_phys);
                    }
                }

                if (sc_fault_addr >= current_task->user_brk && sc_fault_addr < 0x40000000u) {
                    sc_new_phys = pfa_alloc();
                    if (sc_new_phys != 0) {
                        pmm_zero_page_phys(sc_new_phys);
                        vmm_map_page_in_pml4(sc_expected_pd, sc_fault_page, sc_new_phys, 0x7);
                        sc_mapped_phys = vmm_get_phys_in_pml4(sc_expected_pd, sc_fault_page);
                        if (sc_mapped_phys != 0) {
                            sc_new_user_pages = (uint64_t *)krealloc(current_task->user_pages, (current_task->user_pages_count + 1) * sizeof(uint64_t));
                            if (sc_new_user_pages) {
                                current_task->user_pages = sc_new_user_pages;
                                current_task->user_pages[current_task->user_pages_count] = sc_new_phys;
                                current_task->user_pages_count++;
                            }
                            if (sc_actual_cr3 != sc_expected_pd) {
                                regs->return_cr3 = sc_expected_pd;
                            } else {
                                __asm__ volatile ("invlpg (%0)" : : "r"(sc_fault_page) : "memory");
                            }
                            return regs;
                        }
                        pfa_free(sc_new_phys);
                    }
                }

                if (sc_fault_addr >= 0x1000u && sc_fault_addr < current_task->user_brk) {
                    sc_new_phys = pfa_alloc();
                    if (sc_new_phys != 0) {
                        pmm_zero_page_phys(sc_new_phys);
                        vmm_map_page_in_pml4(sc_expected_pd, sc_fault_page, sc_new_phys, 0x7);
                        sc_mapped_phys = vmm_get_phys_in_pml4(sc_expected_pd, sc_fault_page);
                        if (sc_mapped_phys != 0) {
                            sc_new_user_pages = (uint64_t *)krealloc(current_task->user_pages, (current_task->user_pages_count + 1) * sizeof(uint64_t));
                            if (sc_new_user_pages) {
                                current_task->user_pages = sc_new_user_pages;
                                current_task->user_pages[current_task->user_pages_count] = sc_new_phys;
                                current_task->user_pages_count++;
                            }
                            if (sc_actual_cr3 != sc_expected_pd) {
                                regs->return_cr3 = sc_expected_pd;
                            } else {
                                __asm__ volatile ("invlpg (%0)" : : "r"(sc_fault_page) : "memory");
                            }
                            return regs;
                        }
                        pfa_free(sc_new_phys);
                    }
                }

                printf("SEGV addr=0x%lX rip=0x%lX err=0x%lX\n", sc_fault_addr, regs->rip, regs->err_code);
                task_exit_deferred(139);
                return schedule_from_irq(regs);
            }
        }
        
        terminal_writestring(">>> INT 0x");
        print_hex(regs->int_no);
        terminal_writestring(" RIP=0x");
        print_hex(regs->rip);
        terminal_writestring(" <<<\n");
    }

    if (regs->int_no < 32) {
        kernel_panic("CPU Exception", regs);
    } else {
        if (regs->int_no == 255) {
            return regs;
        }
        if (regs->int_no == 49) {
            __asm__ volatile (
                "movq %%cr3, %%rax\n\t"
                "movq %%rax, %%cr3\n\t"
                ::: "rax", "memory"
            );
            lapic_eoi();
            return regs;
        }
        if (regs->int_no == 48) {
            return schedule_from_irq(regs);
        } else if (regs->int_no == 128) {
            uint64_t old_cr3;
            uint64_t new_cr3;
            int did_exec;

            did_exec = 0;

            do_syscall(regs);

            __asm__ volatile ("cli" ::: "memory");

            if (current_task && current_task->exec_completed) {
                did_exec = 1;
            }

            if (did_exec) {
                DEBUG_IDT("IDT: exec completed, preparing to switch CR3\n");
                DEBUG_IDT("IDT: exec regs: rip=0x%016lX rsp=0x%016lX cs=0x%lX ss=0x%lX\n",
                       regs->rip, regs->rsp, regs->cs, regs->ss);
                
                if (regs->rsp < 0x1000 || regs->rsp >= KERNEL_VMA) {
                    DEBUG_IDT("IDT: CRITICAL: rsp=0x%016lX is invalid!\n", regs->rsp);
                    __asm__ volatile ("cli; hlt");
                }
                
                old_cr3 = 0;
                __asm__ volatile ("mov %%cr3, %0" : "=r"(old_cr3));
                new_cr3 = current_task->pml4_phys;
                
                if (new_cr3 && old_cr3 != new_cr3) {
                    {
                        extern void temp_map_raw(uint64_t, uint64_t);
                        extern void temp_unmap_raw(uint64_t);
                        extern uint64_t boot_pdpt_high[] __attribute__((aligned(4096)));
                        uint64_t ev;
                        uint64_t *ep;
                        uint64_t e511;
                        uint64_t ewant;
                        ev = TEMP_SLOT(0);
                        ewant = ((uint64_t)(uintptr_t)boot_pdpt_high & ~0xFFFULL) | 3;
                        temp_map_raw(ev, new_cr3);
                        ep = (uint64_t *)ev;
                        e511 = ep[511];
                        temp_unmap_raw(ev);
                        if ((e511 & 0xFFFFFFFFF000ULL) != (ewant & 0xFFFFFFFFF000ULL)) {
                            printf("EXEC-CR3: PML4[511] BAD pid=%d cr3=0x%lX got=0x%lX want=0x%lX\n",
                                   current_task->id, new_cr3, e511, ewant);
                            __asm__ volatile ("cli; hlt");
                        }
                    }
                    __asm__ volatile ("mov %0, %%cr3" : : "r"(new_cr3) : "memory");
                    __asm__ volatile (
                        "movq %%cr3, %%rax\n\t"
                        "movq %%rax, %%cr3\n\t"
                        ::: "rax", "memory"
                    );
                    DEBUG_IDT("IDT: exec completed, switched CR3 from 0x%016lX to 0x%016lX\n", old_cr3, new_cr3);
                }
                if (current_task) {
                    current_task->cr3 = new_cr3;
                }
                
                exec_cleanup_enqueue(current_task->exec_old_pml4,
                                     current_task->exec_old_pages,
                                     current_task->exec_old_pages_count);
                
                current_task->exec_old_pages = NULL;
                current_task->exec_old_pages_count = 0;
                current_task->exec_old_pml4 = 0;
                current_task->exec_completed = 0;
            }
            
            if (current_task && current_task->state == TASK_DEAD) {
                return schedule_from_irq(regs);
            }

            if (current_task && current_task->is_user && (regs->cs & 0x3) == 0x3) {
                signal_deliver_pending(regs);
                if (current_task && (current_task->state == TASK_DEAD || current_task->state == TASK_BLOCKED)) {
                    return schedule_from_irq(regs);
                }
            }

            return regs;
        }

        irq = regs->int_no - 32;
        if (lapic_base) {
            lapic_eoi();
        } else {
            if (irq >= 8) outb(0xA0, 0x20);
            outb(0x20, 0x20);
        }
        
        if (irq == 0) {
            __sync_fetch_and_add(&tick_count, 1);

            if (current_task) {
                if (current_task->id == 0 && !current_task->is_user) {
                    __sync_fetch_and_add(&cpu_idle_ticks, 1);
                } else if (regs->cs & 0x3) {
                    current_task->utime++;
                    __sync_fetch_and_add(&cpu_user_ticks, 1);
                } else {
                    current_task->stime++;
                    __sync_fetch_and_add(&cpu_system_ticks, 1);
                }
            } else {
                __sync_fetch_and_add(&cpu_idle_ticks, 1);
            }

            if ((tick_count & 0x7F) == 0) {
                extern void task_update_cached_stats(void);
                task_update_cached_stats();
            }

            wake_sleeping_tasks();
            reap_request();
            exec_drain_request();

            extern void fb_tick(void);
            __asm__ volatile ("mov %%cr3, %0" : "=r"(orig_cr3));
            kernel_cr3 = vmm_get_kernel_cr3();
            if (kernel_cr3 && orig_cr3 != kernel_cr3) {
                __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
            }

            fb_tick();
            extern void net_tick(void);
            net_tick();

            extern void kprint_poll(uint64_t max_items);
            kprint_poll(64);

            pit_process_callbacks();

            if (kernel_cr3 && orig_cr3 != kernel_cr3) {
                __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
            }

            regs = schedule_from_irq(regs);

            keyboard_process_sigint();

            if (current_task && current_task->is_user && (regs->cs & 0x3) == 0x3) {
                signal_deliver_pending(regs);
                if (current_task && (current_task->state == TASK_DEAD || current_task->state == TASK_BLOCKED)) {
                    regs = schedule_from_irq(regs);
                }
            }

        } else if (irq == 1) {
            __asm__ volatile ("mov %%cr3, %0" : "=r"(orig_cr3));
            kernel_cr3 = vmm_get_kernel_cr3();
            if (kernel_cr3 && orig_cr3 != kernel_cr3) {
                __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
            }
            keyboard_handler(regs);
            if (kernel_cr3 && orig_cr3 != kernel_cr3) {
                __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
            }
        } else if (irq < MAX_IRQ_HANDLERS && irq_handlers[irq]) {
            __asm__ volatile ("mov %%cr3, %0" : "=r"(orig_cr3));
            kernel_cr3 = vmm_get_kernel_cr3();
            if (kernel_cr3 && orig_cr3 != kernel_cr3) {
                __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
            }
            irq_handlers[irq](regs);
            if (kernel_cr3 && orig_cr3 != kernel_cr3) {
                __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
            }
        }

        if (0) {
            terminal_writestring("IRQ ");
            print_hex(irq);
            terminal_writestring(" handled\n");
        }
    }

    return regs;
}

void keyboard_disable(void) {
    uint8_t cmd_byte;

    outb(0x64, 0x20);
    cmd_byte = inb(0x60);
    cmd_byte &= ~0x01;
    outb(0x64, 0x60);
    outb(0x60, cmd_byte);
    terminal_writestring("Keyboard IRQ disabled.\n");
}

void pic_remap(void)
{
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);    
    outb(0xA1, 0x28);   
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0xFC);
    outb(0xA1, 0xFF);
}

static struct idt_entry {
    uint16_t offset_lo;
    uint16_t sel;
    uint8_t  ist;
    uint8_t  flags;
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t reserved;
} __attribute__((packed)) idt[256] __attribute__((section(".data")));

static struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idtp = { sizeof(idt)-1, 0 };

void idt_set_gate(uint8_t n, uintptr_t handler)
{
    idt[n].offset_lo  = handler & 0xFFFF;
    idt[n].offset_mid = (handler >> 16) & 0xFFFF;
    idt[n].offset_hi  = (handler >> 32) & 0xFFFFFFFF;
    idt[n].sel        = 0x08;
    idt[n].ist        = 0;
    idt[n].flags      = 0x8E;
    idt[n].reserved   = 0;
}

void idt_set_gate_flags(uint8_t n, uintptr_t handler, uint8_t flags)
{
    idt[n].offset_lo  = handler & 0xFFFF;
    idt[n].offset_mid = (handler >> 16) & 0xFFFF;
    idt[n].offset_hi  = (handler >> 32) & 0xFFFFFFFF;
    idt[n].sel        = 0x08;
    idt[n].ist        = 0;
    idt[n].flags      = flags;
    idt[n].reserved   = 0;
}

static void idt_set_gate_ist(uint8_t n, uintptr_t handler, uint8_t ist_index)
{
    idt[n].offset_lo  = handler & 0xFFFF;
    idt[n].offset_mid = (handler >> 16) & 0xFFFF;
    idt[n].offset_hi  = (handler >> 32) & 0xFFFFFFFF;
    idt[n].sel        = 0x08;
    idt[n].ist        = ist_index;
    idt[n].flags      = 0x8E;
    idt[n].reserved   = 0;
}

void idt_init(void)
{
    uint8_t i;
    
    idtp.base = (uint64_t)(uintptr_t)&idt[0];

    memset(&idt, 0, sizeof(idt));

    idt_set_gate(0,  (uintptr_t)isr0);
    idt_set_gate(1,  (uintptr_t)isr1);
    idt_set_gate_ist(2,  (uintptr_t)isr2, 2);
    idt_set_gate(3,  (uintptr_t)isr3);
    idt_set_gate(4,  (uintptr_t)isr4);
    idt_set_gate(5,  (uintptr_t)isr5);
    idt_set_gate(6,  (uintptr_t)isr6);
    idt_set_gate(7,  (uintptr_t)isr7);
    idt_set_gate_ist(8,  (uintptr_t)isr8, 1);
    idt_set_gate(9,  (uintptr_t)isr9);
    idt_set_gate(10, (uintptr_t)isr10);
    idt_set_gate(11, (uintptr_t)isr11);
    idt_set_gate(12, (uintptr_t)isr12);
    idt_set_gate(13, (uintptr_t)isr13);
    idt_set_gate_ist(14, (uintptr_t)isr14, 3);
    idt_set_gate(15, (uintptr_t)isr15);
    idt_set_gate(16, (uintptr_t)isr16);
    idt_set_gate(17, (uintptr_t)isr17);
    idt_set_gate(18, (uintptr_t)isr18);
    idt_set_gate(19, (uintptr_t)isr19);

    idt_set_gate(20, (uintptr_t)isr20);
    idt_set_gate(21, (uintptr_t)isr21);
    idt_set_gate(22, (uintptr_t)isr22);
    idt_set_gate(23, (uintptr_t)isr23);
    idt_set_gate(24, (uintptr_t)isr24);
    idt_set_gate(25, (uintptr_t)isr25);
    idt_set_gate(26, (uintptr_t)isr26);
    idt_set_gate(27, (uintptr_t)isr27);
    idt_set_gate(28, (uintptr_t)isr28);
    idt_set_gate(29, (uintptr_t)isr29);
    idt_set_gate(30, (uintptr_t)isr30);
    idt_set_gate(31, (uintptr_t)isr31);
    idt_set_gate(32, (uintptr_t)isr32);
    idt_set_gate(33, (uintptr_t)isr33);

    idt_set_gate(34, (uintptr_t)isr34);
    idt_set_gate(35, (uintptr_t)isr35);
    idt_set_gate(36, (uintptr_t)isr36);
    idt_set_gate(37, (uintptr_t)isr37);
    idt_set_gate(38, (uintptr_t)isr38);
    idt_set_gate(39, (uintptr_t)isr39);
    idt_set_gate(40, (uintptr_t)isr40);
    idt_set_gate(41, (uintptr_t)isr41);
    idt_set_gate(42, (uintptr_t)isr42);
    idt_set_gate(43, (uintptr_t)isr43);
    idt_set_gate(44, (uintptr_t)isr44);
    idt_set_gate(45, (uintptr_t)isr45);
    idt_set_gate(46, (uintptr_t)isr46);
    idt_set_gate(47, (uintptr_t)isr47);
    idt_set_gate(48, (uintptr_t)isr48);
    idt_set_gate(49, (uintptr_t)isr49);

    idt_set_gate_flags(128, (uintptr_t)isr128, 0xEF);

    idt_set_gate(255, (uintptr_t)isr255);

    for (i = 0; i < 48; i++) {
        if (idt[i].offset_lo == 0 && idt[i].offset_mid == 0 && idt[i].offset_hi == 0) {
            outb(0x3F8, 'N');
            outb(0x3F8, 'U');
            outb(0x3F8, 'L');
            outb(0x3F8, 'L');
            outb(0x3F8, '\n');
        }
    }

    __asm__ volatile ("lidt %0" : : "m"(idtp) : "memory");
}

void idt_load(void) {
    __asm__ volatile ("lidt %0" : : "m"(idtp) : "memory");
}
