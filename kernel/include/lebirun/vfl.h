#ifndef _LEBIRUN_VFL_H
#define _LEBIRUN_VFL_H

#include <stdint.h>

#define VFL_API_VERSION     1

#define VFL_IOCTL_GET_API_VERSION   0xAE00
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

#define VFL_EXIT_HLT        0
#define VFL_EXIT_IO          1
#define VFL_EXIT_MMIO        2
#define VFL_EXIT_SHUTDOWN    3
#define VFL_EXIT_INTERNAL    4
#define VFL_EXIT_EXCEPTION   5
#define VFL_EXIT_PARAVIRT    6

#define VFL_MEM_FLAG_READ   (1 << 0)
#define VFL_MEM_FLAG_WRITE  (1 << 1)
#define VFL_MEM_FLAG_EXEC   (1 << 2)
#define VFL_MEM_FLAG_RWX    (VFL_MEM_FLAG_READ | VFL_MEM_FLAG_WRITE | VFL_MEM_FLAG_EXEC)

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
} vfl_sregs_t;

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
            uint8_t data[8];
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

typedef struct vfl_mem_slot {
    int in_use;
    uint64_t guest_phys;
    uint64_t size;
    uint8_t *host_mem;
    uint32_t flags;
} vfl_mem_slot_t;

typedef struct vfl_vcpu {
    int active;
    vfl_regs_t regs;
    vfl_sregs_t sregs;
    vfl_run_t last_exit;
    int halted;
} vfl_vcpu_t;

typedef struct vfl_vm {
    int active;
    uint32_t id;
    vfl_vcpu_t *vcpus;
    int nr_vcpus;
    vfl_mem_slot_t *mem_slots;
    int nr_mem_slots;
    int mem_slot_extent;
} vfl_vm_t;

int vfl_create_vm(void);
int vfl_destroy_vm(uint32_t vm_id);
int vfl_create_vcpu(uint32_t vm_id);
int vfl_set_regs(uint32_t vm_id, uint32_t vcpu_id, const vfl_regs_t *regs);
int vfl_get_regs(uint32_t vm_id, uint32_t vcpu_id, vfl_regs_t *regs);
int vfl_set_sregs(uint32_t vm_id, uint32_t vcpu_id, const vfl_sregs_t *sregs);
int vfl_get_sregs(uint32_t vm_id, uint32_t vcpu_id, vfl_sregs_t *sregs);
int vfl_set_memory(uint32_t vm_id, const vfl_memory_region_t *region);
int vfl_run(uint32_t vm_id, uint32_t vcpu_id, vfl_run_t *run);
int vfl_vm_info(uint32_t vm_id, vfl_vm_info_t *info);

void vfl_register_devfs(void);
struct vfs_node *vfl_get_devfs_node(void);

#endif
