#include <kernel/panic.h>
#include <kernel/registers.h>
#include <kernel/tty.h>
#include <kernel/console.h>
#include <kernel/io.h>
#include <stdint.h>

static volatile int in_panic = 0;

static void serial_hex(uint32_t v) {
    int i;
    char c;
    for (i = 7; i >= 0; i--) {
        uint8_t nib = (v >> (i * 4)) & 0xF;
        c = (nib < 10) ? ('0' + nib) : ('A' + nib - 10);
        outb(0x3F8, c);
    }
}

static void safe_panic_print(const char *s) {
    while (*s) {
        outb(0x3F8, *s);
        s++;
    }
}

static int is_valid_kernel_ptr(uint32_t addr) {
    if (addr < 0xC0000000) return 0;
    if (addr >= 0xFFFF0000) return 0;
    return 1;
}

static void serial_dump_memory(uint32_t addr, int count) {
    int i;
    uint32_t *ptr;
    
    if (!is_valid_kernel_ptr(addr)) return;
    ptr = (uint32_t *)addr;
    for (i = 0; i < count; i++) {
        if (!is_valid_kernel_ptr((uint32_t)&ptr[i])) break;
        safe_panic_print("  ");
        serial_hex(addr + i * 4);
        safe_panic_print(": ");
        serial_hex(ptr[i]);
        safe_panic_print("\n");
    }
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

void kernel_panic(const char *reason, registers_t *regs) {
    uint32_t fault_addr = 0;
    uint32_t cr0_val = 0;
    uint32_t cr3_val = 0;
    uint32_t cr4_val = 0;
    uint32_t *ebp_ptr;
    int frame_count;

    asm volatile("cli");

    if (in_panic) {
        safe_panic_print("\n!!! DOUBLE PANIC !!!");
        if (regs) {
            safe_panic_print("\nINT=0x");
            serial_hex(regs->int_no);
            safe_panic_print(" EIP=0x");
            serial_hex(regs->eip);
            safe_panic_print(" ERR=0x");
            serial_hex(regs->err_code);
            if (regs->int_no == 14) {
                __asm__ ("movl %%cr2, %0" : "=r" (fault_addr));
                safe_panic_print(" CR2=0x");
                serial_hex(fault_addr);
            }
        }
        safe_panic_print("\nHalted.\n");
        for (;;) __asm__ ("cli; hlt");
    }
    in_panic = 1;

    safe_panic_print("\n");
    safe_panic_print("+===============================+\n");
    safe_panic_print("|       !!! KERNEL PANIC !!!    |\n");
    safe_panic_print("+===============================+\n");
    safe_panic_print("| Reason: ");
    safe_panic_print(reason);
    safe_panic_print("\n");

    if (regs) {
        if (regs->int_no < 32) {
            safe_panic_print("| Exception: ");
            safe_panic_print(exception_messages[regs->int_no]);
            safe_panic_print("\n");
        }
        safe_panic_print("+-------------------------------+\n");
        safe_panic_print("| EXCEPTION INFO                |\n");
        safe_panic_print("+-------------------------------+\n");
        safe_panic_print("  INT=0x");
        serial_hex(regs->int_no);
        safe_panic_print("  ERR=0x");
        serial_hex(regs->err_code);
        safe_panic_print("\n  EIP=0x");
        serial_hex(regs->eip);
        safe_panic_print("  CS=0x");
        serial_hex(regs->cs);
        safe_panic_print("\n");
        safe_panic_print("+-------------------------------+\n");
        safe_panic_print("| REGISTERS                     |\n");
        safe_panic_print("+-------------------------------+\n");
        safe_panic_print("  EAX=0x");
        serial_hex(regs->eax);
        safe_panic_print("  EBX=0x");
        serial_hex(regs->ebx);
        safe_panic_print("\n  ECX=0x");
        serial_hex(regs->ecx);
        safe_panic_print("  EDX=0x");
        serial_hex(regs->edx);
        safe_panic_print("\n  ESI=0x");
        serial_hex(regs->esi);
        safe_panic_print("  EDI=0x");
        serial_hex(regs->edi);
        safe_panic_print("\n  EBP=0x");
        serial_hex(regs->ebp);
        safe_panic_print("  ESP=0x");
        serial_hex(regs->esp);
        safe_panic_print("\n+-------------------------------+\n");
        safe_panic_print("| SEGMENT REGISTERS             |\n");
        safe_panic_print("+-------------------------------+\n");
        safe_panic_print("  DS=0x");
        serial_hex(regs->ds);
        safe_panic_print("  ES=0x");
        serial_hex(regs->es);
        safe_panic_print("\n  FS=0x");
        serial_hex(regs->fs);
        safe_panic_print("  GS=0x");
        serial_hex(regs->gs);
        safe_panic_print("\n  EFLAGS=0x");
        serial_hex(regs->eflags);
        safe_panic_print("\n");

        if (regs->int_no == 14) {
            __asm__ ("movl %%cr2, %0" : "=r" (fault_addr));
            __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_val));
            __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0_val));
            __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4_val));
            safe_panic_print("+-------------------------------+\n");
            safe_panic_print("| PAGE FAULT INFO               |\n");
            safe_panic_print("+-------------------------------+\n");
            safe_panic_print("  CR2 (fault addr)=0x");
            serial_hex(fault_addr);
            safe_panic_print("\n  CR3=0x");
            serial_hex(cr3_val);
            safe_panic_print("  CR0=0x");
            serial_hex(cr0_val);
            safe_panic_print("\n  CR4=0x");
            serial_hex(cr4_val);
            safe_panic_print("\n  Flags: ");
            if (regs->err_code & 0x1) safe_panic_print("[Present] ");
            else safe_panic_print("[NotPresent] ");
            if (regs->err_code & 0x2) safe_panic_print("[Write] ");
            else safe_panic_print("[Read] ");
            if (regs->err_code & 0x4) safe_panic_print("[User] ");
            else safe_panic_print("[Kernel] ");
            if (regs->err_code & 0x8) safe_panic_print("[RSVD] ");
            if (regs->err_code & 0x10) safe_panic_print("[InstrFetch] ");
            safe_panic_print("\n");
        }

        safe_panic_print("\n+-------------------------------+\n");
        safe_panic_print("| STACK TRACE (EBP CHAIN)       |\n");
        safe_panic_print("+-------------------------------+\n");
        ebp_ptr = (uint32_t *)regs->ebp;
        frame_count = 0;
        while (ebp_ptr && frame_count < 10) {
            if (!is_valid_kernel_ptr((uint32_t)ebp_ptr)) break;
            if (!is_valid_kernel_ptr((uint32_t)&ebp_ptr[1])) break;
            safe_panic_print("  #");
            serial_hex(frame_count);
            safe_panic_print("  EBP=0x");
            serial_hex((uint32_t)ebp_ptr);
            safe_panic_print("  RET=0x");
            serial_hex(ebp_ptr[1]);
            safe_panic_print("\n");
            ebp_ptr = (uint32_t *)ebp_ptr[0];
            frame_count++;
        }

        if (is_valid_kernel_ptr(regs->esp)) {
            safe_panic_print("\n+-------------------------------+\n");
            safe_panic_print("| STACK DUMP (8 DWORDS @ ESP)   |\n");
            safe_panic_print("+-------------------------------+\n");
            serial_dump_memory(regs->esp, 8);
        }

        if (regs->int_no == 14 && is_valid_kernel_ptr(fault_addr & ~0xFFF)) {
            safe_panic_print("\n+-------------------------------+\n");
            safe_panic_print("| MEMORY NEAR FAULT ADDRESS     |\n");
            safe_panic_print("+-------------------------------+\n");
            serial_dump_memory(fault_addr & ~0xF, 4);
        }
    }

    safe_panic_print("\n+===============================+\n");
    safe_panic_print("|        SYSTEM HALTED          |\n");
    safe_panic_print("+===============================+\n");

    terminal_writestring("\n====== KERNEL PANIC ======\n");
    terminal_writestring(reason);
    terminal_writestring("\n");
    
    if (regs) {
        if (regs->int_no < 32) {
            terminal_writestring("Exception: ");
            terminal_writestring(exception_messages[regs->int_no]);
            terminal_writestring("\n");
        }
        terminal_writestring("EIP=0x");
        print_hex(regs->eip);
        terminal_writestring(" ERR=0x");
        print_hex(regs->err_code);
        if (regs->int_no == 14) {
            terminal_writestring("\nCR2=0x");
            print_hex(fault_addr);
            terminal_writestring(" CR3=0x");
            print_hex(cr3_val);
        }
        terminal_writestring("\nEAX=0x");
        print_hex(regs->eax);
        terminal_writestring(" EBX=0x");
        print_hex(regs->ebx);
        terminal_writestring(" ECX=0x");
        print_hex(regs->ecx);
        terminal_writestring("\n");
    }
    
    terminal_writestring("===========================\n");

    for (;;) __asm__ ("cli; hlt");
}
