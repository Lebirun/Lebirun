#include <kernel/panic.h>
#include <kernel/registers.h>
#include <kernel/tty.h>
#include <kernel/console.h>
#include <kernel/io.h>
#include <kernel/mem_map.h>
#include <kernel/task.h>
#include <kernel/framebuffer.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

static volatile int in_panic = 0;

static void safe_panic_print(const char *s) {
    while (*s) {
        outb(0x3F8, *s);
        s++;
    }
}

static void fb_panic_print(const char *s) {
    fb_write_string(s);
}

static void panic_print(const char *s) {
    safe_panic_print(s);
    fb_panic_print(s);
}

#define WRAP_WIDTH 60

static void panic_print_wrapped(const char *prefix, const char *text) {
    int col;
    int indent;
    int i;
    const char *p;
    char c[2];

    col = 0;
    indent = 0;
    c[1] = '\0';
    p = prefix;
    while (*p) {
        c[0] = *p++;
        panic_print(c);
        col++;
        indent++;
    }
    p = text;
    while (*p) {
        if (col >= WRAP_WIDTH) {
            panic_print("\n");
            for (i = 0; i < indent; i++) {
                panic_print(" ");
            }
            col = indent;
        }
        c[0] = *p++;
        panic_print(c);
        col++;
    }
    panic_print("\n");
}

static void panic_print_hex(uint32_t v) {
    char buf[9];
    int i;
    uint8_t nib;
    buf[8] = '\0';
    for (i = 0; i < 8; ++i) {
        nib = (v >> ((7 - i) * 4)) & 0xF;
        buf[i] = (nib < 10) ? ('0' + nib) : ('A' + nib - 10);
    }
    panic_print(buf);
}

static void panic_print_dec(int v) {
    char buf[12];
    int i;
    int neg;
    unsigned int uv;

    if (v == 0) {
        panic_print("0");
        return;
    }
    neg = 0;
    if (v < 0) {
        neg = 1;
        uv = (unsigned int)(-(v + 1)) + 1u;
    } else {
        uv = (unsigned int)v;
    }
    i = 0;
    while (uv > 0 && i < 11) {
        buf[i++] = '0' + (uv % 10);
        uv /= 10;
    }
    if (neg) panic_print("-");
    while (i > 0) {
        char c[2];
        c[0] = buf[--i];
        c[1] = '\0';
        panic_print(c);
    }
}

static int is_valid_kernel_ptr(uint32_t addr) {
    uint32_t cr3;

    if (addr < 0xC0000000) return 0;
    if (addr >= 0xFFFF0000) return 0;

    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    if (vmm_get_phys_in_pd(cr3, addr) == 0) return 0;

    return 1;
}

static void panic_dump_memory(uint32_t addr, int count) {
    int i;
    uint32_t *ptr;
    
    if (!is_valid_kernel_ptr(addr)) return;
    ptr = (uint32_t *)addr;
    for (i = 0; i < count; i++) {
        if (!is_valid_kernel_ptr((uint32_t)&ptr[i])) break;
        panic_print("  ");
        panic_print_hex(addr + i * 4);
        panic_print(": ");
        panic_print_hex(ptr[i]);
        panic_print("\n");
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

static void print_task_info(void) {
    if (!current_task) return;

    panic_print("\n--- TASK INFO ---\n");
    panic_print("  PID=");
    panic_print_dec(current_task->pid);
    panic_print("  Name=");
    panic_print(current_task->name[0] ? current_task->name : "(none)");
    panic_print("\n  Console=");
    panic_print_dec(current_task->console_id);
    panic_print("  is_user=");
    panic_print_dec(current_task->is_user);
    panic_print("  state=");
    panic_print_dec(current_task->state);
    panic_print("\n");
}

static void print_selector_error_code(uint32_t err_code, const char *header) {
    panic_print("\n--- ");
    panic_print(header);
    panic_print(" ---\n");
    panic_print("  ");
    if (err_code == 0) {
        panic_print("(no selector error code)\n");
    } else {
        if (err_code & 0x1) panic_print("[EXT] ");
        switch ((err_code >> 1) & 0x3) {
            case 0: panic_print("Table=GDT "); break;
            case 1: panic_print("Table=IDT "); break;
            case 2: panic_print("Table=LDT "); break;
            case 3: panic_print("Table=IDT "); break;
        }
        panic_print("Index=");
        panic_print_dec((int)((err_code >> 3) & 0x1FFF));
        panic_print("\n");
    }
}

static void print_invalid_opcode_info(uint32_t eip) {
    uint8_t *code;
    int i;

    panic_print("\n--- INVALID OPCODE INFO ---\n");
    panic_print("  Faulting EIP=0x");
    panic_print_hex(eip);
    panic_print("\n  Bytes at EIP: ");
    if (is_valid_kernel_ptr(eip)) {
        code = (uint8_t *)eip;
        for (i = 0; i < 8; i++) {
            if (!is_valid_kernel_ptr((uint32_t)&code[i])) break;
            panic_print_hex(code[i]);
            panic_print(" ");
        }
    } else {
        panic_print("(unmapped)");
    }
    panic_print("\n");
}

static void print_double_fault_info(void) {
    panic_print("\n--- DOUBLE FAULT INFO ---\n");
    panic_print("  Error code is always 0\n");
    panic_print("  Caused by nested exception\n");
}

static void print_alignment_check_info(uint32_t err_code) {
    panic_print("\n--- ALIGNMENT CHECK INFO ---\n");
    panic_print("  Error code=0x");
    panic_print_hex(err_code);
    panic_print("\n");
    if (err_code == 0) {
        panic_print("  Unaligned memory access\n");
    }
}

static void print_fpu_info(uint32_t int_no) {
    if (int_no == 16) {
        panic_print("\n--- x87 FPE INFO ---\n");
    } else {
        panic_print("\n--- SIMD FPE INFO ---\n");
    }
    panic_print("  Floating-point exception #");
    panic_print_dec(int_no);
    panic_print("\n");
}

static void print_divide_info(uint32_t eip) {
    panic_print("\n--- DIVIDE ERROR INFO ---\n");
    panic_print("  Division by zero at EIP=0x");
    panic_print_hex(eip);
    panic_print("\n");
}

static void print_bound_range_info(uint32_t eip) {
    panic_print("\n--- BOUND RANGE INFO ---\n");
    panic_print("  BOUND instruction failed at EIP=0x");
    panic_print_hex(eip);
    panic_print("\n");
}

static void print_device_not_avail_info(void) {
    panic_print("\n--- DEVICE NOT AVAILABLE INFO ---\n");
    panic_print("  FPU/SSE not available (CR0.EM or CR0.TS set)\n");
}

static void print_machine_check_info(void) {
    panic_print("\n--- MACHINE CHECK INFO ---\n");
    panic_print("  Hardware error detected\n");
    panic_print("  Check MCi_STATUS MSRs for details\n");
}

void kernel_panic(const char *reason, registers_t *regs) {
    uint32_t fault_addr;
    uint32_t cr0_val;
    uint32_t cr3_val;
    uint32_t cr4_val;
    uint32_t *ebp_ptr;
    int frame_count;

    fault_addr = 0;
    cr0_val = 0;
    cr3_val = 0;
    cr4_val = 0;

    asm volatile("cli");

    if (in_panic) {
        panic_print("\n!!! DOUBLE PANIC !!!");
        if (regs) {
            panic_print("\nINT=0x");
            panic_print_hex(regs->int_no);
            panic_print(" EIP=0x");
            panic_print_hex(regs->eip);
            panic_print(" ERR=0x");
            panic_print_hex(regs->err_code);
            if (regs->int_no == 14) {
                __asm__ ("movl %%cr2, %0" : "=r" (fault_addr));
                panic_print(" CR2=0x");
                panic_print_hex(fault_addr);
            }
        }
        panic_print("\nHalted.\n");
        for (;;) __asm__ ("cli; hlt");
    }
    in_panic = 1;

    panic_print("\n!!! KERNEL PANIC !!!\n");
    panic_print_wrapped("Reason: ", reason);

    print_task_info();

    if (regs) {
        if (regs->int_no < 32) {
            panic_print_wrapped("Exception: ", exception_messages[regs->int_no]);
        }

        panic_print("\n--- EXCEPTION INFO ---\n");
        panic_print("  INT=0x");
        panic_print_hex(regs->int_no);
        panic_print("  ERR=0x");
        panic_print_hex(regs->err_code);
        panic_print("\n  EIP=0x");
        panic_print_hex(regs->eip);
        panic_print("  CS=0x");
        panic_print_hex(regs->cs);
        panic_print("\n");

        panic_print("\n--- REGISTERS ---\n");
        panic_print("  EAX=0x");
        panic_print_hex(regs->eax);
        panic_print("  EBX=0x");
        panic_print_hex(regs->ebx);
        panic_print("\n  ECX=0x");
        panic_print_hex(regs->ecx);
        panic_print("  EDX=0x");
        panic_print_hex(regs->edx);
        panic_print("\n  ESI=0x");
        panic_print_hex(regs->esi);
        panic_print("  EDI=0x");
        panic_print_hex(regs->edi);
        panic_print("\n  EBP=0x");
        panic_print_hex(regs->ebp);
        panic_print("  ESP=0x");
        panic_print_hex(regs->esp);
        panic_print("\n");

        panic_print("\n--- SEGMENT REGISTERS ---\n");
        panic_print("  DS=0x");
        panic_print_hex(regs->ds);
        panic_print("  ES=0x");
        panic_print_hex(regs->es);
        panic_print("\n  FS=0x");
        panic_print_hex(regs->fs);
        panic_print("  GS=0x");
        panic_print_hex(regs->gs);
        panic_print("\n  EFLAGS=0x");
        panic_print_hex(regs->eflags);
        panic_print("\n");

        switch (regs->int_no) {
        case 0:
            print_divide_info(regs->eip);
            break;
        case 5:
            print_bound_range_info(regs->eip);
            break;
        case 6:
            print_invalid_opcode_info(regs->eip);
            break;
        case 7:
            print_device_not_avail_info();
            break;
        case 8:
            print_double_fault_info();
            break;
        case 10:
            print_selector_error_code(regs->err_code, "INVALID TSS ERROR CODE     ");
            break;
        case 11:
            print_selector_error_code(regs->err_code, "SEGMENT NOT PRESENT CODE   ");
            break;
        case 12:
            print_selector_error_code(regs->err_code, "STACK-SEGMENT FAULT CODE   ");
            break;
        case 13:
            print_selector_error_code(regs->err_code, "GPF ERROR CODE DECODE      ");
            break;
        case 14:
            __asm__ ("movl %%cr2, %0" : "=r" (fault_addr));
            __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_val));
            __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0_val));
            __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4_val));
            panic_print("\n--- PAGE FAULT INFO ---\n");
            panic_print("  CR2 (fault addr)=0x");
            panic_print_hex(fault_addr);
            panic_print("\n  CR3=0x");
            panic_print_hex(cr3_val);
            panic_print("  CR0=0x");
            panic_print_hex(cr0_val);
            panic_print("\n  CR4=0x");
            panic_print_hex(cr4_val);
            panic_print("\n  Flags: ");
            if (regs->err_code & 0x1) panic_print("[Present] ");
            else panic_print("[NotPresent] ");
            if (regs->err_code & 0x2) panic_print("[Write] ");
            else panic_print("[Read] ");
            if (regs->err_code & 0x4) panic_print("[User] ");
            else panic_print("[Kernel] ");
            if (regs->err_code & 0x8) panic_print("[RSVD] ");
            if (regs->err_code & 0x10) panic_print("[InstrFetch] ");
            panic_print("\n");
            break;
        case 16:
        case 19:
            print_fpu_info(regs->int_no);
            break;
        case 17:
            print_alignment_check_info(regs->err_code);
            break;
        case 18:
            print_machine_check_info();
            break;
        default:
            break;
        }

        panic_print("\n--- STACK TRACE (EBP CHAIN) ---\n");
        ebp_ptr = (uint32_t *)regs->ebp;
        frame_count = 0;
        while (ebp_ptr && frame_count < 10) {
            if (!is_valid_kernel_ptr((uint32_t)ebp_ptr)) break;
            if (!is_valid_kernel_ptr((uint32_t)&ebp_ptr[1])) break;
            panic_print("  #");
            panic_print_hex(frame_count);
            panic_print("  EBP=0x");
            panic_print_hex((uint32_t)ebp_ptr);
            panic_print("  RET=0x");
            panic_print_hex(ebp_ptr[1]);
            panic_print("\n");
            ebp_ptr = (uint32_t *)ebp_ptr[0];
            frame_count++;
        }

        if (is_valid_kernel_ptr(regs->esp)) {
            panic_print("\n--- STACK DUMP (8 DWORDS @ ESP) ---\n");
            panic_dump_memory(regs->esp, 8);
        }

        if (regs->int_no == 14 && is_valid_kernel_ptr(fault_addr & ~0xFFF)) {
            panic_print("\n--- MEMORY NEAR FAULT ADDRESS ---\n");
            panic_dump_memory(fault_addr & ~0xF, 4);
        }
    } else {
        __asm__ volatile ("mov %%ebp, %0" : "=r"(ebp_ptr));
        if (ebp_ptr) {
            panic_print("\n--- STACK TRACE (EBP CHAIN) ---\n");
            frame_count = 0;
            while (ebp_ptr && frame_count < 10) {
                if (!is_valid_kernel_ptr((uint32_t)ebp_ptr)) break;
                if (!is_valid_kernel_ptr((uint32_t)&ebp_ptr[1])) break;
                panic_print("  #");
                panic_print_hex(frame_count);
                panic_print("  EBP=0x");
                panic_print_hex((uint32_t)ebp_ptr);
                panic_print("  RET=0x");
                panic_print_hex(ebp_ptr[1]);
                panic_print("\n");
                ebp_ptr = (uint32_t *)ebp_ptr[0];
                frame_count++;
            }
        }
    }

    panic_print("\n*** SYSTEM HALTED ***\n");

    for (;;) __asm__ ("cli; hlt");
}

void kernel_panic_msg(const char *fmt, ...) {
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    kernel_panic(buf, NULL);
}

void kernel_panic_custom(const char *category, const char *fmt, ...) {
    char buf[256];
    char reason[320];
    va_list ap;
    int cat_len;
    int buf_len;
    int i;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    cat_len = 0;
    while (category[cat_len] && cat_len < 60) cat_len++;
    buf_len = 0;
    while (buf[buf_len] && buf_len < 250) buf_len++;

    i = 0;
    reason[i++] = '[';
    {
        int j;
        for (j = 0; j < cat_len && i < 318; j++) {
            reason[i++] = category[j];
        }
    }
    reason[i++] = ']';
    reason[i++] = ' ';
    {
        int j;
        for (j = 0; j < buf_len && i < 319; j++) {
            reason[i++] = buf[j];
        }
    }
    reason[i] = '\0';

    kernel_panic(reason, NULL);
}
