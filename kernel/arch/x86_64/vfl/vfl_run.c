#include "vfl_priv.h"
#include "../syscalls/syscall_defs.h"
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <string.h>
#include <stdint.h>

static void vfl_fpu_begin(uint8_t *host_fpu) {
    __asm__ volatile ("fxsave64 %0"
                      : "=m"(*(uint8_t (*)[512])host_fpu) : : "memory");
    __asm__ volatile ("fninit" ::: "memory");
}

static void vfl_fpu_end(uint8_t *host_fpu) {
    __asm__ volatile ("fxrstor64 %0"
                      : : "m"(*(uint8_t (*)[512])host_fpu) : "memory");
}

static int vfl_complete(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *in, vfl_run_t *out) {
    uint8_t b[8];
    uint64_t val = 0;
    uint64_t res;
    int i;
    int r;

    switch (v->pending) {
        case VFL_P_NONE:
            return 0;
        case VFL_P_IO_IN_REG:
            vfl_write_reg(v, v->pending_reg, v->pending_rex,
                          v->pending_size, (uint64_t)in->io.data);
            vfl_adv(v, (int)v->pending_adv, v->pending_mask);
            v->pending = VFL_P_NONE;
            return 0;
        case VFL_P_IO_IN_MEM:
            vfl_put64(b, (uint64_t)in->io.data, v->pending_size);
            r = vfl_mem_write(vm, v, v->pending_addr, (uint64_t)v->pending_size, b);
            if (r == -2)
                return vfl_mem_fault(v, out);
            if (r == 1)
                return vfl_exit_exception(v, out, 14, 0);
            if (r < 0)
                return vfl_exit_exception(v, out, 13, 0);
            if (v->pending_adv)
                vfl_adv(v, (int)v->pending_adv, v->pending_mask);
            v->pending = VFL_P_NONE;
            return 0;
        case VFL_P_MMIO_REG:
            for (i = 0; i < v->pending_size && i < 8; i++)
                val |= (uint64_t)in->mmio.data[i] << (i * 8);
            vfl_write_reg(v, v->pending_reg, v->pending_rex,
                          v->pending_size, val);
            vfl_adv(v, (int)v->pending_adv, v->pending_mask);
            v->pending = VFL_P_NONE;
            return 0;
        case VFL_P_MMIO_RMW: {
            uint32_t op = v->pending_op;
            int size = v->pending_size;

            for (i = 0; i < size && i < 8; i++)
                val |= (uint64_t)in->mmio.data[i] << (i * 8);
            val &= vfl_mask(size);
            if (op == 12) {
                vfl_write_reg(v, v->pending_reg, v->pending_rex, size, val);
                vfl_put64(b, v->pending_src, size);
                r = vfl_mem_write(vm, v, v->pending_addr, (uint64_t)size, b);
                if (r == -2)
                    return vfl_mem_fault(v, out);
                if (r == 1)
                    return vfl_exit_mmio_write(vm, v, out, v->pending_addr,
                                               v->pending_src, size,
                                               (int)v->pending_adv, v->pending_mask);
                if (r < 0)
                    return vfl_exit_exception(v, out, 13, 0);
                vfl_adv(v, (int)v->pending_adv, v->pending_mask);
                v->pending = VFL_P_NONE;
                return 0;
            }
            res = vfl_do_alu(v, (int)op, val, v->pending_src, size);
            if (op == VFL_RMW_CMP) {
                vfl_adv(v, (int)v->pending_adv, v->pending_mask);
                v->pending = VFL_P_NONE;
                return 0;
            }
            vfl_put64(b, res, size);
            r = vfl_mem_write(vm, v, v->pending_addr, (uint64_t)size, b);
            if (r == -2)
                return vfl_mem_fault(v, out);
            if (r == 1)
                return vfl_exit_mmio_write(vm, v, out, v->pending_addr, res, size,
                                           (int)v->pending_adv, v->pending_mask);
            if (r < 0)
                return vfl_exit_exception(v, out, 13, 0);
            vfl_adv(v, (int)v->pending_adv, v->pending_mask);
            v->pending = VFL_P_NONE;
            return 0;
        }
        case VFL_P_MMIO_JMP:
            for (i = 0; i < v->pending_size && i < 8; i++)
                val |= (uint64_t)in->mmio.data[i] << (i * 8);
            v->regs.rip = (v->regs.rip & ~v->pending_mask) | (val & v->pending_mask);
            v->pending = VFL_P_NONE;
            return 0;
        case VFL_P_MMIO_CALL: {
            uint64_t ret;

            for (i = 0; i < v->pending_size && i < 8; i++)
                val |= (uint64_t)in->mmio.data[i] << (i * 8);
            ret = (v->regs.rip + v->pending_adv) & v->pending_mask;
            r = vfl_push(vm, v, out, ret, v->pending_size);
            if (r) return r;
            v->regs.rip = (v->regs.rip & ~v->pending_mask) | (val & v->pending_mask);
            v->pending = VFL_P_NONE;
            return 0;
        }
        case VFL_P_MMIO_PUSH:
            for (i = 0; i < v->pending_size && i < 8; i++)
                val |= (uint64_t)in->mmio.data[i] << (i * 8);
            r = vfl_push(vm, v, out, val, v->pending_size);
            if (r) return r;
            vfl_adv(v, (int)v->pending_adv, v->pending_mask);
            v->pending = VFL_P_NONE;
            return 0;
        case VFL_P_MMIO_XMM: {
            int size = v->pending_size;

            if (size == 32) {
                memcpy(&v->fpu.xmm[v->pending_reg & 15], in->mmio.data, 16);
                memcpy(&v->fpu.ymm_hi[v->pending_reg & 15], in->mmio.data + 16, 16);
            } else if (size >= 0 && size <= 16) {
                uint8_t *x = (uint8_t *)&v->fpu.xmm[v->pending_reg & 15];
                int j;

                for (j = 0; j < size; j++)
                    x[j] = in->mmio.data[j];
                if (v->pending_rex)
                    memset(&v->fpu.ymm_hi[v->pending_reg & 15], 0, 16);
            } else {
                v->pending = VFL_P_NONE;
                return vfl_exit_internal(v, out);
            }
            vfl_adv(v, (int)v->pending_adv, v->pending_mask);
            v->pending = VFL_P_NONE;
            return 0;
        }
        case VFL_P_MMIO_FILL: {
            int i;
            int n = v->pending_size;

            if (n < 0 || n > 64) {
                v->pending = VFL_P_NONE;
                return vfl_exit_internal(v, out);
            }
            for (i = 0; i < n; i++)
                v->mmio_cache_data[i] = in->mmio.data[i];
            v->mmio_cache_addr = v->pending_src;
            v->mmio_cache_len = (uint64_t)n;
            v->mmio_cache_valid = 1;
            v->pending = VFL_P_NONE;
            return 0;
        }
        case VFL_P_MMIO_STR:
            return vfl_str_resume(vm, v, in, out);
        default:
            v->pending = VFL_P_NONE;
            return vfl_exit_internal(v, out);
    }
}

static int vfl_irq_push(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint64_t val, int size) {
    uint64_t sp = (vfl_sp(v) - (uint64_t)size) & vfl_stack_mask(v);
    uint8_t b[8];
    int r;

    vfl_put64(b, val, size);
    r = vfl_mem_write(vm, v, vfl_seg_base(v, 2) + sp, (uint64_t)size, b);
    if (r == -2) return vfl_mem_fault(v, run);
    if (r) return vfl_exit_exception(v, run, 12, 0);
    vfl_set_sp(v, sp);
    return 0;
}

static int vfl_deliver_irq(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run) {
    uint8_t vec;
    int nmi;
    int prot;
    int lm;

    if (!v->irq_valid) return 0;
    vec = v->irq.vector;
    nmi = v->irq.type == VFL_IRQ_NMI;
    if (!nmi && !((v->regs.rflags >> 9) & 1ULL)) return 0;
    prot = (v->sregs.cr0 & 1ULL) != 0;
    lm = vfl_long_mode(v);
    v->irq_valid = 0;
    if (v->halted) v->halted = 0;
    if (!prot) {
        uint8_t b[4];
        uint16_t off;
        uint16_t seg;
        int r;

        r = vfl_phys_read(vm, (uint64_t)vec * 4, 4, b);
        if (r) return vfl_exit_exception(v, run, 8, 0);
        off = vfl_get16(b);
        seg = vfl_get16(b + 2);
        r = vfl_irq_push(vm, v, run, v->regs.rflags & 0xFFFFULL, 2);
        if (r) return r;
        r = vfl_irq_push(vm, v, run, v->sregs.cs.selector, 2);
        if (r) return r;
        r = vfl_irq_push(vm, v, run, v->regs.rip & 0xFFFFULL, 2);
        if (r) return r;
        v->regs.rflags &= ~((1ULL << 8) | (1ULL << 9));
        v->sregs.cs.selector = seg;
        v->sregs.cs.base = (uint64_t)seg << 4;
        v->regs.rip = off;
        v->irq.vector = 0;
        v->irq.type = VFL_IRQ_EXTERNAL;
        v->irq.has_error = 0;
        return 2;
    }
    if (!lm) {
        uint64_t gate = v->sregs.idt_base + (uint64_t)vec * 8;
        uint8_t b[8];
        uint32_t off;
        uint16_t sel;
        uint8_t type;
        int r;

        r = vfl_mem_read(vm, v, gate, 8, b);
        if (r == -2) return vfl_mem_fault(v, run);
        if (r) return vfl_exit_exception(v, run, 8, (uint64_t)vec * 8 + 2);
        off = vfl_get16(b) | ((uint32_t)vfl_get16(b + 6) << 16);
        sel = vfl_get16(b + 2);
        type = b[5] & 0x1F;
        if (!(b[5] & 0x80)) return vfl_exit_exception(v, run, 11, (uint64_t)vec * 8 + 2);
        if (type != 0xE && type != 0xF) {
            if (type == 0x5) return vfl_exit_shutdown(v, run);
            return vfl_exit_exception(v, run, 13, (uint64_t)vec * 8 + 2);
        }
        r = vfl_irq_push(vm, v, run, v->regs.rflags & 0xFFFFFFFFULL, 4);
        if (r) return r;
        r = vfl_irq_push(vm, v, run, v->sregs.cs.selector, 4);
        if (r) return r;
        r = vfl_irq_push(vm, v, run, v->regs.rip & 0xFFFFFFFFULL, 4);
        if (r) return r;
        if (v->irq.has_error) {
            r = vfl_irq_push(vm, v, run, v->irq.error_code, 4);
            if (r) return r;
        }
        if (type == 0xE) v->regs.rflags &= ~(1ULL << 9);
        v->regs.rflags &= ~(1ULL << 8);
        v->sregs.cs.selector = sel;
        v->regs.rip = (v->regs.rip & ~0xFFFFFFFFULL) | off;
        v->irq.vector = 0;
        v->irq.type = VFL_IRQ_EXTERNAL;
        v->irq.has_error = 0;
        return 2;
    }
    {
        uint64_t gate = v->sregs.idt_base + (uint64_t)vec * 16;
        uint8_t b[16];
        uint64_t off;
        uint16_t sel;
        uint8_t type;
        int r;

        r = vfl_mem_read(vm, v, gate, 16, b);
        if (r == -2) return vfl_mem_fault(v, run);
        if (r) return vfl_exit_exception(v, run, 8, (uint64_t)vec * 8 + 2);
        off = vfl_get16(b) | ((uint64_t)vfl_get16(b + 6) << 16) | ((uint64_t)vfl_get32(b + 8) << 32);
        sel = vfl_get16(b + 2);
        type = b[5] & 0x1F;
        if (!(b[5] & 0x80)) return vfl_exit_exception(v, run, 11, (uint64_t)vec * 8 + 2);
        if (type != 0xE && type != 0xF) {
            if (type == 0x5) return vfl_exit_shutdown(v, run);
            return vfl_exit_exception(v, run, 13, (uint64_t)vec * 8 + 2);
        }
        r = vfl_irq_push(vm, v, run, v->sregs.ss.selector, 8);
        if (r) return r;
        r = vfl_irq_push(vm, v, run, vfl_sp(v), 8);
        if (r) return r;
        r = vfl_irq_push(vm, v, run, v->regs.rflags, 8);
        if (r) return r;
        r = vfl_irq_push(vm, v, run, v->sregs.cs.selector, 8);
        if (r) return r;
        r = vfl_irq_push(vm, v, run, v->regs.rip, 8);
        if (r) return r;
        if (v->irq.has_error) {
            r = vfl_irq_push(vm, v, run, v->irq.error_code, 8);
            if (r) return r;
        }
        if (type == 0xE) v->regs.rflags &= ~(1ULL << 9);
        v->regs.rflags &= ~(1ULL << 8);
        v->sregs.cs.selector = sel;
        v->regs.rip = off;
        v->irq.vector = 0;
        v->irq.type = VFL_IRQ_EXTERNAL;
        v->irq.has_error = 0;
        return 2;
    }
}

static int vfl_try_irq(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run) {
    int nmi;

    if (!v->irq_valid) return 0;
    nmi = v->irq.type == VFL_IRQ_NMI;
    if (!nmi && v->int_shadow) return 0;
    if (!nmi && !((v->regs.rflags >> 9) & 1ULL)) return 0;
    return vfl_deliver_irq(vm, v, run);
}

static int vfl_run_main(vfl_vm_t *vm, vfl_vcpu_t *vcpu, vfl_run_t *run) {
    vfl_run_t input;
    uint64_t steps;
    uint64_t budget;
    uint64_t chunk;
    uint64_t executed;
    uint64_t ip;
    uint64_t ripmask;
    int r;
    int pending;

    input = *run;
    pending = vcpu->pending;
    r = vfl_complete(vm, vcpu, &input, run);
    if (r != 0) return r < 0 ? -1 : 0;
    if (pending && pending != VFL_P_MMIO_FILL && !vcpu->pending &&
        (vcpu->control.flags & VFL_CONTROL_SINGLE_STEP)) {
        run->exit_reason = VFL_EXIT_DEBUG;
        return 0;
    }
    budget = vcpu->control.instruction_budget;
    if (!budget) budget = 65536;
    steps = 0;
    while (steps < budget) {
        if (__atomic_exchange_n(&vcpu->stop, 0, __ATOMIC_ACQ_REL)) {
            run->exit_reason = VFL_EXIT_INTERRUPTED;
            return 0;
        }
        if (!(steps & 1023)) vfl_timer_poll(vm, vcpu, run);
        r = vfl_try_irq(vm, vcpu, run);
        if (r == 1) return 0;
        if (vcpu->halted) {
            run->exit_reason = VFL_EXIT_HLT;
            return 0;
        }
        ripmask = vfl_long_mode(vcpu) ? ~0ULL :
                  (vcpu->sregs.cr0 & 1ULL) ? 0xFFFFFFFFULL : 0xFFFFULL;
        ip = vfl_seg_base(vcpu, 1) + (vcpu->regs.rip & ripmask);
        if (vfl_breakpoint(vcpu, ip)) {
            run->exit_reason = VFL_EXIT_DEBUG;
            return 0;
        }
        chunk = 1024 - (steps & 1023);
        if (chunk > budget - steps) chunk = budget - steps;
        if (vcpu->int_shadow || (vcpu->control.flags & VFL_CONTROL_SINGLE_STEP)) chunk = 1;
        r = vfl_hot_run(vm, vcpu, run, chunk, &executed);
        steps += executed;
        if (vcpu->stats) vcpu->stats->instructions += executed;
        if (r) return r < 0 ? -1 : 0;
        if (!executed) {
            if (__atomic_load_n(&vcpu->stop, __ATOMIC_ACQUIRE)) continue;
            r = vfl_step(vm, vcpu, run);
            steps++;
            if (vcpu->stats) vcpu->stats->instructions++;
            if (r) return r < 0 ? -1 : 0;
        }
        if (vcpu->control.flags & VFL_CONTROL_SINGLE_STEP) {
            run->exit_reason = VFL_EXIT_DEBUG;
            return 0;
        }
    }
    if (vcpu->control.flags & VFL_CONTROL_TIMESLICE)
        run->exit_reason = VFL_EXIT_TIMESLICE;
    else if (vcpu->irq_valid && vcpu->irq.type == VFL_IRQ_EXTERNAL &&
             !((vcpu->regs.rflags >> 9) & 1ULL))
        run->exit_reason = VFL_EXIT_IRQ_WINDOW;
    else
        run->exit_reason = VFL_EXIT_INTERNAL;
    return 0;
}

int vfl_run(uint32_t vm_id, uint32_t vcpu_id, vfl_run_t *run) {
    uint8_t host_fpu[512] __attribute__((aligned(16)));
    vfl_vm_t *vm;
    vfl_vcpu_t *vcpu;
    int r;

    if (!run) return -EINVAL;
    vm = vfl_get_vm(vm_id);
    vcpu = vfl_get_vcpu(vm_id, vcpu_id);
    if (!vm || !vcpu) return -EINVAL;
    vfl_fpu_begin(host_fpu);
    r = vfl_run_main(vm, vcpu, run);
    vfl_flags_sync(vcpu);
    vcpu->lazy_enabled = 0;
    vcpu->last_exit = *run;
    if (vcpu->stats && run->exit_reason <= VFL_EXIT_DEBUG)
        vcpu->stats->exits[run->exit_reason]++;
    vfl_fpu_end(host_fpu);
    return r;
}

int vfl_vm_info(uint32_t vm_id, vfl_vm_info_t *info) {
    vfl_vm_t *vm;
    vfl_mem_slot_t *s;
    uint64_t total;

    if (!info) return -EINVAL;
    vm = vfl_get_vm(vm_id);
    if (!vm) return -EINVAL;

    total = 0;
    for (s = vm->mem_slots; s; s = s->next)
        total += s->size;

    info->vm_id = vm_id;
    info->nr_vcpus = (uint32_t)vm->nr_vcpus;
    info->nr_mem_slots = (uint32_t)vm->nr_mem_slots;
    info->total_mem = total;
    return 0;
}
