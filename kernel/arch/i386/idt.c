#include <string.h>
#include <stdint.h>
#include <kernel/tty.h>
#include <kernel/debug.h>
#include <kernel/keyboard.h>
#include <kernel/task.h>
#include <kernel/syscall.h>
#include <kernel/io.h>
#include <kernel/mem_map.h>
#include <kernel/registers.h>
#include <kernel/idt.h>

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

static const char* exception_messages[] = {
    "Divide-by-zero",
    "Debug",
    "Non-maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Triple Fault"
};

registers_t* interrupt_handler(registers_t* regs)
{
    if (regs->int_no < 32) {
        if (regs->int_no == 14 && (regs->err_code & 0x4) && current_task && current_task->is_user) {
            uint32_t fault_addr;
            __asm__ ("movl %%cr2, %0" : "=r" (fault_addr));
            printf("User task %d page fault at 0x%08X (EIP=0x%08X) - killing task\n",
                   current_task->pid, fault_addr, regs->eip);
            printf("Regs: ESP=0x%08X EBP=0x%08X EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X\n",
                   regs->useresp, regs->ebp, regs->eax, regs->ebx, regs->ecx, regs->edx);
            printf("      ESI=0x%08X EDI=0x%08X\n", regs->esi, regs->edi);

            if (current_task && current_task->pd_phys) {
                uint32_t eip_pd_idx = regs->eip >> 22;
                uint32_t eip_pt_idx = (regs->eip >> 12) & 0x3FF;
                uint32_t temp_pd = 0xF7000000;
                uint32_t temp_pt = 0xF7001000;
                uint32_t temp_stack = 0xF7002000;

                vmm_temp_map_raw(temp_pd, current_task->pd_phys);
                uint32_t *user_pd = (uint32_t *)temp_pd;
                uint32_t pde = user_pd[eip_pd_idx];
                printf("User EIP PDE[%u]=0x%08X\n", eip_pd_idx, pde);
                if (pde & 1) {
                    uint32_t pt_phys = pde & ~0xFFF;
                    vmm_temp_map_raw(temp_pt, pt_phys);
                    uint32_t *user_pt = (uint32_t *)temp_pt;
                    uint32_t pte = user_pt[eip_pt_idx];
                    printf("User EIP PTE[%u]=0x%08X\n", eip_pt_idx, pte);

                    if (pte & 1) {
                        uint32_t page_phys = pte & ~0xFFF;
                        
                        uint32_t temp_code = 0xF7003000;
                        vmm_temp_map_raw(temp_code, page_phys);
                        uint32_t code_offset = regs->eip & 0xFFF;
                        printf("Instructions at EIP (0x%08X, offset 0x%X in page):\n", regs->eip, code_offset);
                        for (int i = -8; i < 16; i++) {
                            uint32_t off = (code_offset + i) & 0xFFF;
                            if (i == 0) printf("[");
                            printf("%02X", *((uint8_t*)(temp_code + off)));
                            if (i == 0) printf("]");
                            printf(" ");
                        }
                        printf("\n");
                        vmm_temp_unmap_raw(temp_code);
                        
                        uint32_t stack_page_phys = 0;
                        if ((regs->ebp & ~0xFFF) == (regs->eip & ~0xFFF)) {
                            stack_page_phys = page_phys;
                        } else {
                            uint32_t stack_pd_idx = regs->ebp >> 22;
                            uint32_t stack_pt_idx = (regs->ebp >> 12) & 0x3FF;
                            uint32_t stack_pde = user_pd[stack_pd_idx];
                            if (stack_pde & 1) {
                                uint32_t stack_pt_phys = stack_pde & ~0xFFF;
                                vmm_temp_map_raw(temp_pt, stack_pt_phys);
                                uint32_t *stack_pt = (uint32_t *)temp_pt;
                                uint32_t stack_pte = stack_pt[stack_pt_idx];
                                if (stack_pte & 1) stack_page_phys = stack_pte & ~0xFFF;
                                vmm_temp_unmap_raw(temp_pt);
                            }
                        }

                        if (stack_page_phys) {
                            vmm_temp_map_raw(temp_stack, stack_page_phys);
                            uint32_t off = (regs->ebp + 4) & 0xFFF;
                            uint32_t caller_ret = *((uint32_t *)(temp_stack + off));
                            printf("Caller return address = 0x%08X\n", caller_ret);
                            uint32_t off_fmt = (regs->ebp + 8) & 0xFFF;
                            uint32_t fmt_arg = *((uint32_t *)(temp_stack + off_fmt));
                            printf("Printf format arg = 0x%08X\n", fmt_arg);

                            printf("Stack dump around EBP (addr=0x%08X):\n", regs->ebp);
                            int start_off = -0x20;
                            for (int i = start_off; i <= 0x20; i += 4) {
                                uint32_t offw = (regs->ebp + i) & 0xFFF;
                                uint32_t val = *((uint32_t *)(temp_stack + offw));
                                printf("  [EBP%+04d] = 0x%08X\n", i, val);
                            }

                            vmm_temp_unmap_raw(temp_stack);
                        } else {
                            printf("Caller return address: stack page not present\n");
                        }
                        
                        uint32_t esp_pd_idx = regs->useresp >> 22;
                        uint32_t esp_pt_idx = (regs->useresp >> 12) & 0x3FF;
                        uint32_t esp_pde = user_pd[esp_pd_idx];
                        if (esp_pde & 1) {
                            uint32_t esp_pt_phys = esp_pde & ~0xFFF;
                            vmm_temp_map_raw(temp_pt, esp_pt_phys);
                            uint32_t *esp_pt = (uint32_t *)temp_pt;
                            uint32_t esp_pte = esp_pt[esp_pt_idx];
                            vmm_temp_unmap_raw(temp_pt);
                            if (esp_pte & 1) {
                                uint32_t esp_page_phys = esp_pte & ~0xFFF;
                                vmm_temp_map_raw(temp_stack, esp_page_phys);
                                printf("Stack dump around ESP (addr=0x%08X):\n", regs->useresp);
                                for (int i = 0; i <= 0x40; i += 4) {
                                    uint32_t offw = (regs->useresp + i) & 0xFFF;
                                    uint32_t val = *((uint32_t *)(temp_stack + offw));
                                    printf("  [ESP+%04X] = 0x%08X\n", i, val);
                                }
                                vmm_temp_unmap_raw(temp_stack);
                            }
                        }
                    }

                    vmm_temp_unmap_raw(temp_pt);
                }
                vmm_temp_unmap_raw(temp_pd);
            }

            task_exit_deferred(139);
            return schedule_from_irq(regs);
        }

        if (regs->int_no == 14 && !(regs->err_code & 0x4) && current_task && current_task->is_user && current_task->syscall_frame) {
            uint32_t fault_addr;
            __asm__ ("movl %%cr2, %0" : "=r" (fault_addr));
            if (fault_addr < 0xC0000000) {
                printf("User task %d page fault in syscall at 0x%08X (EIP=0x%08X) - killing task\n",
                       current_task->pid, fault_addr, regs->eip);
                task_exit_deferred(139);
                return schedule_from_irq(regs);
            }
        }
        
        terminal_writestring(">>> TRAPPED INT 0x");
        print_hex(regs->int_no);
        terminal_writestring(" | EIP=0x");
        print_hex(regs->eip);
        terminal_writestring(" <<<\n");
    }

    if (regs->int_no < 32) {
        terminal_writestring("\n\n!!! KERNEL PANIC !!!\n");
        terminal_writestring("Exception: ");
        terminal_writestring(exception_messages[regs->int_no]);
        terminal_writestring(" (vector 0x");
        print_hex(regs->int_no);
        terminal_writestring(")\n");
        terminal_writestring("Error code: 0x");
        print_hex(regs->err_code);
        terminal_writestring("\nEIP = 0x");
        print_hex(regs->eip);
        if (regs->int_no == 13) { 
            terminal_writestring("\nSegment registers: DS=0x");
            print_hex(regs->ds);
            terminal_writestring(" ES=0x");
            print_hex(regs->es);
            terminal_writestring(" FS=0x");
            print_hex(regs->fs);
            terminal_writestring(" GS=0x");
            print_hex(regs->gs);
            terminal_writestring("\n");
        }
        if (regs->int_no == 14) {
            uint32_t fault_addr;
            __asm__ ("movl %%cr2, %0" : "=r" (fault_addr));
            terminal_writestring("\nPage fault at address 0x");
            print_hex(fault_addr);
            uint32_t cur_cr3;
            __asm__ volatile ("mov %%cr3, %0" : "=r"(cur_cr3));
            terminal_writestring("\nCR3 = 0x"); print_hex(cur_cr3);
            if (current_task) {
                terminal_writestring(" current_task PD = 0x"); print_hex(current_task->pd_phys);
                terminal_writestring(" current_task pid = "); print_hex(current_task->pid);
            }

            terminal_writestring("\n-- PD/PT dump for fault address (user PD) --\n");
            if (current_task && current_task->pd_phys) vmm_dump_for_pd(current_task->pd_phys, fault_addr & ~0xFFF);
            terminal_writestring("\n-- PD/PT dump for fault address (kernel) --\n");
            vmm_dump_for_pd(vmm_get_kernel_cr3(), fault_addr & ~0xFFF);
            if (fault_addr >= 0xF7000000 && fault_addr < 0xF7003000) {
                terminal_writestring("\n-- Temp mapping diagnostics --\n");
                dump_map_debug();
                dump_pd_pt_for_virt(fault_addr & ~0xFFF);
            }
        }
        terminal_writestring("\n\nSystem halted.");
        for (;;) __asm__ ("hlt");
    } else {
        if (regs->int_no == 48) {
            return schedule_from_irq(regs);
        } else if (regs->int_no == 128) {
            do_syscall(regs);
            if (current_task && current_task->state == TASK_DEAD) {
                return schedule_from_irq(regs);
            }
            return regs;
        }

        uint8_t irq = regs->int_no - 32;
        if (irq >= 8) outb(0xA0, 0x20);
        outb(0x20, 0x20);
        
        if (irq == 0) {
            tick_count++;
            wake_sleeping_tasks();
            reap_dead_tasks();
            extern void fb_tick(void);
            uint32_t orig_cr3;
            __asm__ volatile ("mov %%cr3, %0" : "=r"(orig_cr3));
            uint32_t kernel_cr3 = vmm_get_kernel_cr3();
            if (kernel_cr3 && orig_cr3 != kernel_cr3) {
                __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
            }

            fb_tick();
            extern void net_tick(void);
            net_tick();

            if (kernel_cr3 && orig_cr3 != kernel_cr3) {
                __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
            }

            regs = schedule_from_irq(regs);
        } else if (irq == 1) {
            uint32_t orig_cr3;
            __asm__ volatile ("mov %%cr3, %0" : "=r"(orig_cr3));
            uint32_t kernel_cr3 = vmm_get_kernel_cr3();
            if (kernel_cr3 && orig_cr3 != kernel_cr3) {
                __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
            }
            keyboard_handler(regs);
            if (kernel_cr3 && orig_cr3 != kernel_cr3) {
                __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
            }
        } else if (irq < MAX_IRQ_HANDLERS && irq_handlers[irq]) {
            uint32_t orig_cr3;
            __asm__ volatile ("mov %%cr3, %0" : "=r"(orig_cr3));
            uint32_t kernel_cr3 = vmm_get_kernel_cr3();
            if (kernel_cr3 && orig_cr3 != kernel_cr3) {
                __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
            }
            irq_handlers[irq](regs);
            if (kernel_cr3 && orig_cr3 != kernel_cr3) {
                __asm__ volatile ("mov %0, %%cr3" : : "r"(orig_cr3) : "memory");
            }
        }

        if (debugMode && debugLevel >= 5) {
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
    idt[n].base_lo = handler & 0xFFFF;
    idt[n].base_hi = (handler >> 16) & 0xFFFF;
    idt[n].sel     = 0x08;
    idt[n].always0 = 0;
    idt[n].flags   = 0x8E;
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

    __asm__ volatile ("lidt %0" : : "m"(idtp) : "memory");

    outb(0x3F8, 'I');
    outb(0x3F8, 'D');
    outb(0x3F8, 'T');
    outb(0x3F8, '\n');
}