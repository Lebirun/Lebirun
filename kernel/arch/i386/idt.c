#include <string.h>
#include <stdint.h>
#include <kernel/tty.h>
#include <kernel/debug.h>
#include <kernel/keyboard.h>
#include <kernel/console.h>
#include <kernel/task.h>
#include <kernel/syscall.h>
#include <kernel/io.h>
#include <kernel/mem_map.h>
#include <kernel/registers.h>
#include <kernel/idt.h>
#include <kernel/vring.h>
#include <kernel/panic.h>

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
extern void isr128(void);

volatile uint32_t tick_count = 0;

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
        if (regs->eip < 0xC0000000) return 1;
    }
    return 0;
}

registers_t* interrupt_handler(registers_t* regs)
{
    uint32_t fault_addr;
    uint32_t orig_cr3;
    uint32_t kernel_cr3;
    uint8_t access_type;
    uint8_t irq;
    int sig;

    if (regs->int_no < 32) {
        if (regs->int_no == 14) {
            __asm__ ("movl %%cr2, %0" : "=r" (fault_addr));
            
            if (!(regs->err_code & 0x4)) {
                if (demand_page_fault_handler(fault_addr, regs->err_code)) {
                    return regs;
                }
            }

            if (!(regs->err_code & 0x4) && !(regs->err_code & 0x1) && fault_addr >= 0xC0000000) {
                extern uint32_t pae_enabled;
                extern uint64_t boot_pd_high[];
                extern void pae_sync_kernel_mappings(void);
                if (pae_enabled) {
                    uint32_t pde_idx;
                    pde_idx = (fault_addr >> 21) & 0x1FF;
                    if (boot_pd_high[pde_idx] & 1) {
                        pae_sync_kernel_mappings();
                        __asm__ volatile("invlpg (%0)" : : "r"(fault_addr) : "memory");
                        return regs;
                    }
                }
            }

            {
                extern int kstack_page_fault_handler(uint32_t fault_addr);
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
                    vring_panic_forbidden(current_kproc->vring_minor, fault_addr, access_type);
                }
            }
            
            if (current_task && current_task->vring_minor != 0 && !current_task->is_user) {
                if (!vring_check_access(current_task->vring_minor, fault_addr, PAGE_SIZE, access_type)) {
                    vring_panic_forbidden(current_task->vring_minor, fault_addr, access_type);
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
            printf("[KERNEL] User exception %d at EIP=0x%08X sig=%d\n",
                   regs->int_no, regs->eip, sig);
            
            task_exit_deferred(128 + sig);
            return schedule_from_irq(regs);
        }
        
        if (regs->int_no == 14 && (regs->err_code & 0x4) && current_task && current_task->is_user) {
            uint32_t actual_cr3;
            uint32_t expected_pd;
            uint32_t entry_phys;
            uint32_t fault_page;
            uint32_t phys;
            uint32_t stack_floor;
            uint32_t new_phys;
            uint32_t mapped_phys;
            uint32_t *new_user_pages;
            __asm__ ("movl %%cr2, %0" : "=r" (fault_addr));
            __asm__ ("movl %%cr3, %0" : "=r" (actual_cr3));
            expected_pd = current_task->pd_phys;
            printf("User PF at 0x%08X EIP=0x%08X err=0x%X\n", fault_addr, regs->eip, regs->err_code);
            printf("  actual_cr3=0x%08X expected_pd=0x%08X exec_completed=%d\n", 
                   actual_cr3, expected_pd, current_task->exec_completed);
            
            if (actual_cr3 != expected_pd) {
                printf("  CR3 MISMATCH! Switching CR3 now...\n");
                __asm__ volatile ("mov %0, %%cr3" : : "r"(expected_pd) : "memory");
                
                entry_phys = vmm_get_phys_in_pd(expected_pd, regs->eip & ~0xFFF);
                printf("  After CR3 switch: entry_phys=0x%08X\n", entry_phys);
                if (entry_phys) {

                    printf("  Retrying faulting instruction...\n");
                    return regs;
                }
            }
            
            {
                fault_page = fault_addr & ~0xFFFu;
                phys = vmm_get_phys_in_pd(current_task->pd_phys, fault_page);
                printf("  pd=0x%08X page_phys=0x%08X\n", current_task->pd_phys, phys);
                if (phys == 0) {
                    stack_floor = 0x00700000u;
                    if (fault_addr >= stack_floor && fault_addr < 0x00800000u) {
                        new_phys = pfa_alloc();
                        if (new_phys != 0) {
                            pmm_zero_page_phys(new_phys);
                            vmm_map_page_in_pd(current_task->pd_phys, fault_page, new_phys, 0x7);
                            mapped_phys = vmm_get_phys_in_pd(current_task->pd_phys, fault_page);
                            if (mapped_phys != 0) {
                                new_user_pages = (uint32_t *)krealloc(current_task->user_pages, (current_task->user_pages_count + 1) * sizeof(uint32_t));
                                if (new_user_pages) {
                                    current_task->user_pages = new_user_pages;
                                    current_task->user_pages[current_task->user_pages_count] = new_phys;
                                    current_task->user_pages_count++;
                                }
                                printf("  grew user stack: page=0x%08X phys=0x%08X\n", fault_page, new_phys);
                                return regs;
                            }
                            pfa_free(new_phys);
                        }
                    }
                }
            }

            task_exit_deferred(139);
            return schedule_from_irq(regs);
        }

        if (regs->int_no == 14 && !(regs->err_code & 0x4) && current_task && current_task->is_user && current_task->syscall_frame) {
            __asm__ ("movl %%cr2, %0" : "=r" (fault_addr));
            if (fault_addr < 0xC0000000) {
                printf("User syscall PF at 0x%08X\n", fault_addr);
                task_exit_deferred(139);
                return schedule_from_irq(regs);
            }
        }
        
        terminal_writestring(">>> INT 0x");
        print_hex(regs->int_no);
        terminal_writestring(" EIP=0x");
        print_hex(regs->eip);
        terminal_writestring(" <<<\n");
    }

    if (regs->int_no < 32) {
        kernel_panic("CPU Exception", regs);
    } else {
        if (regs->int_no == 48) {
            return schedule_from_irq(regs);
        } else if (regs->int_no == 128) {
            uint32_t old_cr3;
            uint32_t new_cr3;
            uint32_t *old_pages;
            uint32_t old_pd;
            
            do_syscall(regs);
            
            if (current_task && current_task->exec_completed) {
                DEBUG_IDT("IDT: exec completed, preparing to switch CR3\n");
                DEBUG_IDT("IDT: exec regs: eip=0x%08X useresp=0x%08X cs=0x%X ss=0x%X\n",
                       regs->eip, regs->useresp, regs->cs, regs->ss);
                
                if (regs->useresp < 0x1000 || regs->useresp >= 0xC0000000) {
                    DEBUG_IDT("IDT: CRITICAL: useresp=0x%08X is invalid!\n", regs->useresp);
                    __asm__ volatile ("cli; hlt");
                }
                
                old_cr3 = 0;
                __asm__ volatile ("mov %%cr3, %0" : "=r"(old_cr3));
                new_cr3 = current_task->pd_phys;
                
                if (new_cr3 && old_cr3 != new_cr3) {
                    __asm__ volatile ("mov %0, %%cr3" : : "r"(new_cr3) : "memory");
                    __asm__ volatile (
                        "movl %%cr3, %%eax\n\t"
                        "movl %%eax, %%cr3\n\t"
                        ::: "eax", "memory"
                    );
                    DEBUG_IDT("IDT: exec completed, switched CR3 from 0x%08X to 0x%08X\n", old_cr3, new_cr3);
                }
                if (current_task) {
                    current_task->cr3 = new_cr3;
                }
                
                old_pages = current_task->exec_old_pages;
                old_pd = current_task->exec_old_pd;
                
                if (old_pages) {
                    kfree(old_pages);
                }
                
                if (old_pd) {
                    vmm_free_page_directory(old_pd);
                }
                
                current_task->exec_old_pages = NULL;
                current_task->exec_old_pages_count = 0;
                current_task->exec_old_pd = 0;
                current_task->exec_completed = 0;
            }
            
            if (current_task && current_task->state == TASK_DEAD) {
                return schedule_from_irq(regs);
            }
            if ((regs->cs & 0x3) == 0x3) {
                if (regs->eip < 0x1000 || regs->eip >= 0xC0000000) {
                    printf("IDT: CRITICAL: syscall return eip=0x%08X is invalid!\n", regs->eip);
                    printf("IDT: regs=%p cs=0x%X useresp=0x%08X ss=0x%X eflags=0x%08X\n",
                           regs, regs->cs, regs->useresp, regs->ss, regs->eflags);
                    printf("IDT: eax=0x%X ebx=0x%X ecx=0x%X edx=0x%X\n",
                           regs->eax, regs->ebx, regs->ecx, regs->edx);
                    __asm__ volatile ("cli; hlt");
                }

                if (regs->useresp < 0x1000 || regs->useresp >= 0xC0000000) {
                    printf("IDT: CRITICAL: useresp=0x%08X invalid for user return!\n", regs->useresp);
                    printf("IDT: regs=%p eip=0x%08X cs=0x%X ss=0x%X\n",
                           regs, regs->eip, regs->cs, regs->ss);
                    __asm__ volatile ("cli; hlt");
                }
            }
            return regs;
        }

        irq = regs->int_no - 32;
        if (irq >= 8) outb(0xA0, 0x20);
        outb(0x20, 0x20);
        
        if (irq == 0) {
            tick_count++;

            wake_sleeping_tasks();
            reap_dead_tasks();
            extern void fb_tick(void);
            __asm__ volatile ("mov %%cr3, %0" : "=r"(orig_cr3));
            kernel_cr3 = vmm_get_kernel_cr3();
            if (kernel_cr3 && orig_cr3 != kernel_cr3) {
                __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
            }

            fb_tick();
            extern void net_tick(void);
            net_tick();

            extern void kprint_poll(uint32_t max_items);
            kprint_poll(128);

            if (kernel_cr3 && orig_cr3 != kernel_cr3) {
                __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
            }

            regs = schedule_from_irq(regs);

            if (current_task && current_task->is_user && (regs->cs & 0x3) == 0x3) {
                signal_deliver_pending(regs);
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
    outb(0x64, 0x20);
    uint8_t cmd_byte = inb(0x60);
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
    uint16_t base_lo;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_hi;
} __attribute__((packed)) idt[256] __attribute__((section(".data")));

static struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) idtp = { sizeof(idt)-1, 0 };

void idt_set_gate(uint8_t n, uintptr_t handler)
{
    uint16_t lo;
    uint16_t hi;
    
    lo = handler & 0xFFFF;
    hi = (handler >> 16) & 0xFFFF;
    
    idt[n].base_lo = lo;
    idt[n].base_hi = hi;
    idt[n].sel     = 0x08;
    idt[n].always0 = 0;
    idt[n].flags   = 0x8E;
    
    if (idt[n].base_lo != lo || idt[n].base_hi != hi) {
        outb(0x3F8, 'I');
        outb(0x3F8, 'D');
        outb(0x3F8, 'T');
        outb(0x3F8, 'C');
        outb(0x3F8, '\n');
    }
}

void idt_set_gate_flags(uint8_t n, uintptr_t handler, uint8_t flags)
{
    idt[n].base_lo = handler & 0xFFFF;
    idt[n].base_hi = (handler >> 16) & 0xFFFF;
    idt[n].sel     = 0x08;
    idt[n].always0 = 0;
    idt[n].flags   = flags;
}

void idt_init(void)
{
    uint8_t i;
    
    idtp.base = (uint32_t)&idt[0];

    memset(&idt, 0, sizeof(idt));

    idt_set_gate(0,  (uint32_t)isr0);
    idt_set_gate(1,  (uint32_t)isr1);
    idt_set_gate(2,  (uint32_t)isr2);
    idt_set_gate(3,  (uint32_t)isr3);
    idt_set_gate(4,  (uint32_t)isr4);
    idt_set_gate(5,  (uint32_t)isr5);
    idt_set_gate(6,  (uint32_t)isr6);
    idt_set_gate(7,  (uint32_t)isr7);
    idt_set_gate(8,  (uint32_t)isr8);
    idt_set_gate(9,  (uint32_t)isr9);
    idt_set_gate(10, (uint32_t)isr10);
    idt_set_gate(11, (uint32_t)isr11);
    idt_set_gate(12, (uint32_t)isr12);
    idt_set_gate(13, (uint32_t)isr13);
    idt_set_gate(14, (uint32_t)isr14);
    idt_set_gate(15, (uint32_t)isr15);
    idt_set_gate(16, (uint32_t)isr16);
    idt_set_gate(17, (uint32_t)isr17);
    idt_set_gate(18, (uint32_t)isr18);
    idt_set_gate(19, (uint32_t)isr19);

    idt_set_gate(20, (uint32_t)isr20);
    idt_set_gate(21, (uint32_t)isr21);
    idt_set_gate(22, (uint32_t)isr22);
    idt_set_gate(23, (uint32_t)isr23);
    idt_set_gate(24, (uint32_t)isr24);
    idt_set_gate(25, (uint32_t)isr25);
    idt_set_gate(26, (uint32_t)isr26);
    idt_set_gate(27, (uint32_t)isr27);
    idt_set_gate(28, (uint32_t)isr28);
    idt_set_gate(29, (uint32_t)isr29);
    idt_set_gate(30, (uint32_t)isr30);
    idt_set_gate(31, (uint32_t)isr31);
    idt_set_gate(32, (uint32_t)isr32);
    idt_set_gate(33, (uint32_t)isr33);

    idt_set_gate(34, (uint32_t)isr34);
    idt_set_gate(35, (uint32_t)isr35);
    idt_set_gate(36, (uint32_t)isr36);
    idt_set_gate(37, (uint32_t)isr37);
    idt_set_gate(38, (uint32_t)isr38);
    idt_set_gate(39, (uint32_t)isr39);
    idt_set_gate(40, (uint32_t)isr40);
    idt_set_gate(41, (uint32_t)isr41);
    idt_set_gate(42, (uint32_t)isr42);
    idt_set_gate(43, (uint32_t)isr43);
    idt_set_gate(44, (uint32_t)isr44);
    idt_set_gate(45, (uint32_t)isr45);
    idt_set_gate(46, (uint32_t)isr46);
    idt_set_gate(47, (uint32_t)isr47);
    idt_set_gate(48, (uint32_t)isr48);

    idt_set_gate_flags(128, (uint32_t)isr128, 0xEE);

    for (i = 0; i < 49; i++) {
        if (i == 48) break;
        if (idt[i].base_lo == 0 && idt[i].base_hi == 0) {
            outb(0x3F8, 'N');
            outb(0x3F8, 'U');
            outb(0x3F8, 'L');
            outb(0x3F8, 'L');
            outb(0x3F8, '\n');
        }
    }

    __asm__ volatile ("lidt %0" : : "m"(idtp) : "memory");

    outb(0x3F8, 'I');
    outb(0x3F8, 'D');
    outb(0x3F8, 'T');
    outb(0x3F8, '\n');
}
