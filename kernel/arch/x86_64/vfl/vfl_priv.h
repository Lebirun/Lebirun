#ifndef VFL_PRIV_H
#define VFL_PRIV_H

#include <lebirun/vfl.h>
#include <lebirun/vfs.h>
#include <stdint.h>

#define VFL_P_NONE          0
#define VFL_P_IO_IN_REG     1
#define VFL_P_IO_IN_MEM     2
#define VFL_P_MMIO_REG      3
#define VFL_P_MMIO_RMW      5
#define VFL_P_MMIO_JMP      6
#define VFL_P_MMIO_CALL     7
#define VFL_P_MMIO_PUSH     8
#define VFL_P_MMIO_XMM      9
#define VFL_P_MMIO_FILL     10
#define VFL_P_MMIO_STR      12

#define VFL_RMW_ADD 0
#define VFL_RMW_OR  1
#define VFL_RMW_ADC 2
#define VFL_RMW_SBB 3
#define VFL_RMW_AND 4
#define VFL_RMW_SUB 5
#define VFL_RMW_XOR 6
#define VFL_RMW_CMP 7
#define VFL_RMW_INC 8
#define VFL_RMW_DEC 9
#define VFL_RMW_NOT 10
#define VFL_RMW_NEG 11

#define VFL_SIMD_NOTME 2

#define VFL_MMU_READ  0
#define VFL_MMU_WRITE 1
#define VFL_MMU_EXEC  2

typedef struct vfl_mem_slot {
    int in_use;
    uint64_t guest_phys;
    uint64_t size;
    uint8_t *host_mem;
    uint32_t flags;
    uint8_t *dirty;
    uint64_t dirty_pages;
} vfl_mem_slot_t;

typedef struct {
    uint64_t epoch;
    uint64_t tag;
    uint64_t phys;
    uint64_t leaf_val;
    uint8_t *leaf_host;
    uint32_t leaf_w;
} vfl_tlb_t;

struct vfl_vm;
struct vfl_vcpu;
struct vfl_hot_op;
typedef int (*vfl_hot_fn_t)(struct vfl_vm *, struct vfl_vcpu *, vfl_run_t *,
                            struct vfl_hot_op *);
typedef struct vfl_hot_op vfl_hot_op_t;
struct vfl_hot_op {
    uint64_t tag;
    uint64_t epoch;
    vfl_hot_fn_t fn;
    uint8_t len;
    uint8_t bytes[15];
    uint8_t cls;
    uint8_t sub;
    uint8_t sub2;
    uint8_t reg;
    uint8_t size;
    uint8_t asz;
    uint8_t seg;
    uint8_t rex;
    uint64_t imm;
    uint64_t ripmask;
    uint8_t eamod;
    uint8_t eareg;
    uint8_t ea16rm;
    int8_t sib_scale;
    int8_t sib_index;
    int8_t sib_base;
    int32_t disp;
};

typedef struct vfl_vcpu {
    int active;
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
    vfl_hot_op_t hot[64];
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
    uint64_t tsc;
    uint32_t mmu_vec;
    uint32_t mmu_code;
    vfl_tlb_t tlb[16];
} vfl_vcpu_t;

typedef struct vfl_vm {
    int active;
    uint32_t id;
    uint64_t tlb_epoch;
    vfl_vcpu_t *vcpus;
    int nr_vcpus;
    vfl_mem_slot_t *mem_slots;
    int nr_mem_slots;
    int mem_slot_extent;
} vfl_vm_t;


typedef struct {
    int is_reg;
    int reg;
    uint64_t addr;
    int rip_rel;
    int32_t disp;
} vfl_ea_t;

uint64_t vfl_mask(int size);
uint16_t vfl_get16(uint8_t *b);
uint32_t vfl_get32(uint8_t *b);
uint64_t vfl_get64(uint8_t *b);
void vfl_put64(uint8_t *b, uint64_t v, int size);
int vfl_long_mode(vfl_vcpu_t *v);
uint64_t *vfl_reg64(vfl_vcpu_t *v, int r);
uint64_t vfl_read_reg(vfl_vcpu_t *v, int r, int rex, int size);
void vfl_write_reg(vfl_vcpu_t *v, int r, int rex, int size, uint64_t val);
void vfl_seg_ptr(vfl_vcpu_t *v, int seg, vfl_segment_t **out);
uint64_t vfl_seg_base(vfl_vcpu_t *v, int seg);
void vfl_adv(vfl_vcpu_t *v, int n, uint64_t ripmask);
int vfl_cond(vfl_vcpu_t *v, int cc);
void vfl_flag_logic(vfl_vcpu_t *v, uint64_t r, int size);
void vfl_flag_sub(vfl_vcpu_t *v, uint64_t a, uint64_t b, uint64_t borrow,
                  uint64_t r, int size);
uint64_t vfl_do_alu(vfl_vcpu_t *v, int op, uint64_t a, uint64_t b, int size);
uint64_t vfl_do_shift(vfl_vcpu_t *v, int op, uint64_t val, unsigned count, int size);
int vfl_decode_ea(vfl_vcpu_t *v, uint8_t *buf, int *pos, int mod, int rm,
                  int asz, int seg_ovr, int rex_b, int rex_x, vfl_ea_t *ea);
uint64_t vfl_stack_mask(vfl_vcpu_t *v);
uint64_t vfl_sp(vfl_vcpu_t *v);
void vfl_set_sp(vfl_vcpu_t *v, uint64_t sp);
int vfl_push(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint64_t val, int size);
int vfl_pop(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, int size, uint64_t *val);
uint64_t vfl_get_si(vfl_vcpu_t *v, int asz);
uint64_t vfl_get_di(vfl_vcpu_t *v, int asz);
void vfl_set_si(vfl_vcpu_t *v, int asz, uint64_t val);
void vfl_set_di(vfl_vcpu_t *v, int asz, uint64_t val);
uint64_t vfl_get_count(vfl_vcpu_t *v, int asz);
void vfl_set_count(vfl_vcpu_t *v, int asz, uint64_t val);

vfl_mem_slot_t *vfl_find_slot(vfl_vm_t *vm, uint64_t addr, uint64_t len);
int vfl_phys_read(vfl_vm_t *vm, uint64_t addr, uint64_t len, uint8_t *out);
int vfl_phys_write(vfl_vm_t *vm, uint64_t addr, uint64_t len, uint8_t *in);
int vfl_translate(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t linear, int access, uint64_t *phys);
int vfl_mem_read(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t addr, uint64_t len, uint8_t *out);
int vfl_mem_write(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t addr, uint64_t len, uint8_t *in);
int vfl_fetch_buf(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t addr, uint8_t *buf, int len);
int vfl_mem_fault(vfl_vcpu_t *v, vfl_run_t *run);
void vfl_mark_dirty(vfl_vm_t *vm, uint64_t phys, uint64_t len);

int vfl_exit_exception(vfl_vcpu_t *v, vfl_run_t *run, uint32_t vec, uint32_t err);
int vfl_exit_shutdown(vfl_vcpu_t *v, vfl_run_t *run);
int vfl_exit_internal(vfl_vcpu_t *v, vfl_run_t *run);
int vfl_exit_io_out(vfl_vcpu_t *v, vfl_run_t *run, uint16_t port, int size, uint32_t data);
int vfl_exit_io_in(vfl_vcpu_t *v, vfl_run_t *run, uint16_t port, int size,
                   int pend, int reg, int rex, uint64_t addr, int adv, uint64_t ripmask);
int vfl_exit_mmio_read(vfl_vcpu_t *v, vfl_run_t *run, uint64_t addr, int size,
                       int pend, int reg, int rex, uint64_t src, uint32_t op,
                       int adv, uint64_t ripmask);
int vfl_exit_mmio_write(vfl_vcpu_t *v, vfl_run_t *run, uint64_t addr,
                        uint64_t val, int size, int adv, uint64_t ripmask);
int vfl_exit_mmio_write64(vfl_vcpu_t *v, vfl_run_t *run, uint64_t addr,
                          uint8_t *d16, int len, int pend, int reg, int adv,
                          uint64_t ripmask);
int vfl_mmio_or_fault(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint64_t addr,
                      int size, int pend, int reg, int rex, uint64_t src,
                      uint32_t op, int adv, uint64_t ripmask);
int vfl_str_emit(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *out);
int vfl_str_resume(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *in, vfl_run_t *out);
int vfl_mmio_fill_exit(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint64_t linear,
                       int bytes);
int vfl_memerr(vfl_vcpu_t *v, vfl_run_t *run, int rr);
void vec_read(vfl_vcpu_t *v, int r, int lanes, uint32_t *d);
void vec_write(vfl_vcpu_t *v, int r, int lanes, uint32_t *s, int zero_hi);
int vfl_ea_read(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_ea_t *ea, int size, int rex, uint64_t *out);
int vfl_exec_rmw(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                 int op, uint64_t src, int size, int rex, int n, uint64_t ripmask);
int vfl_exec_store(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                   uint64_t val, int size, int rex, int n, uint64_t ripmask);
int vfl_exec_load_reg(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                      int reg, int size, int rex, int n, uint64_t ripmask);

int vfl_step(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run);
int vfl_hot_lookup(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint64_t ip,
                   int lm, int prot);
void vfl_hot_fill(vfl_vm_t *vm, vfl_vcpu_t *v, uint8_t *buf, uint64_t ip,
                  int lm, int prot, int osz, int asz);
int vfl_simd_escape(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf, int *pos,
                    int osz, int asz, int seg, int rep, int p66, int rex, int rex_w,
                    int rex_r, int rex_b, int rex_x, int lm, uint64_t ripmask);
int vfl_x87_step(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf, int *pos,
                 int osz, int asz, int seg, int rex, int rex_r, int rex_b,
                 int lm, uint64_t ripmask, uint8_t op);
int vfl_vex_step(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf, int *pos,
                 int asz, int seg, int lm, uint64_t ripmask, int is_c4);
int vfl_msr_read(vfl_vcpu_t *v, uint32_t index, uint64_t *val);
int vfl_msr_write(vfl_vcpu_t *v, uint32_t index, uint64_t val);

vfl_vm_t *vfl_get_vm(uint32_t vm_id);
vfl_vcpu_t *vfl_get_vcpu(uint32_t vm_id, uint32_t vcpu_id);

#endif
