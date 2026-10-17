#ifndef _LEBIRUN_VFL_H
#define _LEBIRUN_VFL_H

#include <stdint.h>

#define VFL_IOCTL_CREATE_VM         0xAE01
#define VFL_IOCTL_DESTROY_VM        0xAE02
#define VFL_IOCTL_CREATE_VCPU       0xAE03
#define VFL_IOCTL_RUN               0xAE04
#define VFL_IOCTL_SET_REGS          0xAE05
#define VFL_IOCTL_GET_REGS          0xAE06
#define VFL_IOCTL_SET_MEMORY        0xAE07
#define VFL_IOCTL_GET_MEMORY        0xAE08
#define VFL_IOCTL_SET_SREGS         0xAE09
#define VFL_IOCTL_GET_SREGS         0xAE0A
#define VFL_IOCTL_VM_INFO           0xAE0B
#define VFL_IOCTL_RESET_VCPU        0xAE0C
#define VFL_IOCTL_SET_FPU           0xAE0D
#define VFL_IOCTL_GET_FPU           0xAE0E
#define VFL_IOCTL_INJECT_IRQ        0xAE0F
#define VFL_IOCTL_SET_MSR           0xAE10
#define VFL_IOCTL_GET_MSR           0xAE11
#define VFL_IOCTL_GET_DIRTY         0xAE12
#define VFL_IOCTL_SET_LAPIC         0xAE13
#define VFL_IOCTL_GET_LAPIC         0xAE14
#define VFL_IOCTL_SET_CONTROL       0xAE15
#define VFL_IOCTL_GET_CONTROL       0xAE16
#define VFL_IOCTL_GET_STATS         0xAE17
#define VFL_IOCTL_RESET_STATS       0xAE18
#define VFL_IOCTL_INTERRUPT         0xAE19
#define VFL_IOCTL_GET_STATE         0xAE1A
#define VFL_IOCTL_SET_STATE         0xAE1B
#define VFL_IOCTL_READ_MEMORY       0xAE1C
#define VFL_IOCTL_WRITE_MEMORY      0xAE1D

#define VFL_EXIT_HLT        0
#define VFL_EXIT_IO          1
#define VFL_EXIT_MMIO        2
#define VFL_EXIT_SHUTDOWN    3
#define VFL_EXIT_INTERNAL    4
#define VFL_EXIT_EXCEPTION   5
#define VFL_EXIT_PARAVIRT    6
#define VFL_EXIT_IRQ_WINDOW  7
#define VFL_EXIT_TIMESLICE   8
#define VFL_EXIT_INTERRUPTED 9
#define VFL_EXIT_DEBUG       10

#define VFL_CONTROL_STATS       (1 << 0)
#define VFL_CONTROL_SINGLE_STEP (1 << 1)
#define VFL_CONTROL_BREAKPOINT  (1 << 2)
#define VFL_CONTROL_TIMESLICE   (1 << 3)

#define VFL_MEM_FLAG_READ   (1 << 0)
#define VFL_MEM_FLAG_WRITE  (1 << 1)
#define VFL_MEM_FLAG_EXEC   (1 << 2)
#define VFL_MEM_FLAG_RWX    (VFL_MEM_FLAG_READ | VFL_MEM_FLAG_WRITE | VFL_MEM_FLAG_EXEC)
#define VFL_MEM_FLAG_USER   (1 << 3)

#define VFL_IRQ_EXTERNAL    0
#define VFL_IRQ_NMI         2

typedef struct {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
} vfl_regs_t;

typedef struct {
    uint16_t selector;
    uint64_t base;
    uint32_t limit;
    uint16_t attrib;
} vfl_segment_t;

typedef struct {
    vfl_segment_t cs, ds, es, fs, gs, ss;
    vfl_segment_t tr, ldt;
    uint64_t cr0, cr2, cr3, cr4;
    uint64_t efer;
    uint64_t gdt_base;
    uint16_t gdt_limit;
    uint64_t idt_base;
    uint16_t idt_limit;
    uint64_t cr8;
} vfl_sregs_t;

typedef struct {
    uint64_t lo, hi;
} vfl_xmm_t;

typedef struct {
    vfl_xmm_t xmm[16];
    vfl_xmm_t ymm_hi[16];
    uint32_t mxcsr;
    uint64_t fpr_m[8];
    uint16_t fpr_e[8];
    uint16_t fcw, fsw, ftw, fop, fcs, fds;
    uint32_t fip, fdp;
} vfl_fpu_t;

typedef struct {
    uint8_t vector;
    uint8_t type;
    uint8_t has_error;
    uint8_t reserved;
    uint32_t error_code;
} vfl_irq_t;

typedef struct {
    uint32_t index;
    uint32_t reserved;
    uint64_t value;
} vfl_msr_t;

typedef struct {
    uint32_t id;
    uint32_t tpr;
    uint32_t eoi;
    uint32_t spurious;
    uint32_t icr_lo;
    uint32_t icr_hi;
    uint64_t base;
    uint32_t enabled;
} vfl_lapic_t;

typedef struct {
    uint32_t slot;
    uint32_t reserved;
    uint64_t capacity;
    uint64_t copied;
    uint64_t bitmap;
} vfl_dirty_t;

typedef struct {
    uint32_t slot;
    uint32_t flags;
    uint64_t guest_phys;
    uint64_t size;
    uint64_t host_virt;
} vfl_memory_region_t;

typedef struct {
    uint32_t exit_reason;
    union {
        struct {
            uint16_t port;
            uint8_t size;
            uint8_t direction;
            uint32_t data;
        } io;
        struct {
            uint64_t phys_addr;
            uint8_t data[64];
            uint32_t len;
            uint8_t is_write;
        } mmio;
        struct {
            uint32_t vector;
            uint32_t error_code;
        } exception;
        struct {
            uint64_t nr;
            uint64_t args[4];
        } paravirt;
    };
} vfl_run_t;

typedef struct {
    uint32_t vm_id;
    uint32_t nr_vcpus;
    uint32_t nr_mem_slots;
    uint64_t total_mem;
} vfl_vm_info_t;

typedef struct {
    uint64_t guest_phys;
    uint64_t size;
    uint64_t buffer;
} vfl_memory_transfer_t;

typedef struct {
    uint64_t instruction_budget;
    uint64_t breakpoints;
    uint64_t breakpoint_count;
    uint32_t flags;
    uint32_t reserved;
} vfl_control_t;

typedef struct {
    uint64_t instructions;
    uint64_t hot_hits;
    uint64_t hot_misses;
    uint64_t tlb_hits;
    uint64_t tlb_misses;
    uint64_t page_walks;
    uint64_t batches;
    uint64_t exits[VFL_EXIT_DEBUG + 1];
} vfl_stats_t;

typedef struct {
    vfl_regs_t regs;
    vfl_sregs_t sregs;
    vfl_fpu_t fpu;
    vfl_run_t last_exit;
    int halted;
    int pending;
    uint64_t pending_addr;
    uint32_t pending_len;
    int pending_reg;
    int pending_rex;
    int pending_size;
    uint64_t pending_src;
    uint32_t pending_op;
    uint64_t pending_adv;
    uint64_t pending_mask;
    uint64_t pending_count;
    uint64_t mmio_cache_addr;
    uint64_t mmio_cache_len;
    uint8_t mmio_cache_valid;
    uint8_t mmio_cache_data[64];
    uint16_t pending_port;
    vfl_irq_t irq;
    int irq_valid;
    uint64_t msr_star;
    uint64_t msr_lstar;
    uint64_t msr_cstar;
    uint64_t msr_sfmask;
    uint64_t msr_fs_base;
    uint64_t msr_gs_base;
    uint64_t msr_kernel_gs_base;
    uint64_t msr_tsc_aux;
    uint64_t msr_sysenter_cs;
    uint64_t msr_sysenter_esp;
    uint64_t msr_sysenter_eip;
    uint64_t msr_pat;
    uint64_t msr_apic_base;
    uint64_t msr_misc;
    uint64_t msr_tsc_deadline;
    uint64_t tsc;
    uint64_t tsc_offset;
    uint32_t apic_id;
    uint32_t apic_tpr;
    uint32_t apic_spurious;
    uint32_t apic_enabled;
    uint32_t apic_icr_lo;
    uint32_t apic_icr_hi;
    uint32_t tmr_lvt;
    uint32_t tmr_init;
    uint32_t tmr_div;
    uint64_t tmr_remaining_us;
    int tmr_armed;
    int tmr_periodic;
    uint64_t tsc_remaining_us;
    int tsc_armed;
    int int_shadow;
    uint32_t mmu_vec;
    uint32_t mmu_code;
} vfl_state_t;

int vfl_create_vm(void);
int vfl_destroy_vm(uint32_t vm_id);
int vfl_create_vcpu(uint32_t vm_id);
int vfl_reset_vcpu(uint32_t vm_id, uint32_t vcpu_id);
int vfl_set_regs(uint32_t vm_id, uint32_t vcpu_id, const vfl_regs_t *regs);
int vfl_get_regs(uint32_t vm_id, uint32_t vcpu_id, vfl_regs_t *regs);
int vfl_set_sregs(uint32_t vm_id, uint32_t vcpu_id, const vfl_sregs_t *sregs);
int vfl_get_sregs(uint32_t vm_id, uint32_t vcpu_id, vfl_sregs_t *sregs);
int vfl_set_fpu(uint32_t vm_id, uint32_t vcpu_id, const vfl_fpu_t *fpu);
int vfl_get_fpu(uint32_t vm_id, uint32_t vcpu_id, vfl_fpu_t *fpu);
int vfl_inject_irq(uint32_t vm_id, uint32_t vcpu_id, const vfl_irq_t *irq);
int vfl_set_lapic(uint32_t vm_id, uint32_t vcpu_id, const vfl_lapic_t *lapic);
int vfl_get_lapic(uint32_t vm_id, uint32_t vcpu_id, vfl_lapic_t *lapic);
int vfl_set_msr(uint32_t vm_id, uint32_t vcpu_id, const vfl_msr_t *msr);
int vfl_get_msr(uint32_t vm_id, uint32_t vcpu_id, vfl_msr_t *msr);
int vfl_set_memory(uint32_t vm_id, const vfl_memory_region_t *region);
int vfl_get_memory(uint32_t vm_id, vfl_memory_region_t *region);
int vfl_get_dirty(uint32_t vm_id, vfl_dirty_t *dirty);
int vfl_run(uint32_t vm_id, uint32_t vcpu_id, vfl_run_t *run);
int vfl_vm_info(uint32_t vm_id, vfl_vm_info_t *info);

void vfl_register_devfs(void);
struct vfs_node *vfl_get_devfs_node(void);

#endif
