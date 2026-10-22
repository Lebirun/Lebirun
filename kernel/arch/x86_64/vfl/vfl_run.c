#include "vfl_priv.h"
#include "../syscalls/syscall_defs.h"
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <lebirun/pit.h>
#include <lebirun/task.h>
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

int vfl_system_read(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t addr, uint64_t len, uint8_t *data) {
    uint16_t selector;
    int r;

    selector = v->sregs.cs.selector;
    v->sregs.cs.selector &= ~3U;
    r = vfl_mem_read(vm, v, addr, len, data);
    v->sregs.cs.selector = selector;
    return r;
}

int vfl_segment_read(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run,
                      uint16_t selector, vfl_segment_t *segment) {
    uint8_t data[8];
    uint64_t base;
    uint64_t limit;
    uint32_t offset;
    int r;

    base = selector & 4 ? v->sregs.ldt.base : v->sregs.gdt_base;
    limit = selector & 4 ? v->sregs.ldt.limit : v->sregs.gdt_limit;
    offset = selector & ~7U;
    if (!(selector & ~3U) || offset + 7 > limit)
        return vfl_exit_exception(v, run, 13, selector & ~3U);
    r = vfl_system_read(vm, v, base + offset, 8, data);
    if (r) return vfl_memerr(v, run, r);
    segment->selector = selector;
    segment->base = vfl_get16(data + 2) | ((uint64_t)data[4] << 16) | ((uint64_t)data[7] << 24);
    segment->limit = vfl_get16(data) | ((uint32_t)(data[6] & 15) << 16);
    if (data[6] & 0x80) segment->limit = (segment->limit << 12) | 4095;
    segment->attrib = data[5] | ((uint16_t)(data[6] & 0xF0) << 4);
    return 0;
}

int vfl_iret(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, int size) {
    uint64_t values[5];
    uint64_t sp;
    uint64_t mask;
    uint64_t base;
    uint8_t data[8];
    vfl_segment_t cs;
    vfl_segment_t ss;
    int count;
    int lm;
    int prot;
    int cpl;
    int rpl;
    int dpl;
    int i;
    int r;

    v->nmi_blocked = 0;
    lm = vfl_long_mode(v);
    prot = (v->sregs.cr0 & 1) != 0;
    cpl = v->sregs.cs.selector & 3;
    sp = vfl_sp(v);
    mask = vfl_stack_mask(v);
    base = vfl_seg_base(v, 2);
    count = 3;
    for (i = 0; i < count; i++) {
        memset(data, 0, sizeof(data));
        r = vfl_mem_read(vm, v, base + ((sp + (uint64_t)i * size) & mask), size, data);
        if (r) return vfl_memerr(v, run, r);
        values[i] = vfl_get64(data);
        if (i == 1 && prot && (lm || (values[1] & 3) > (unsigned)cpl)) count = 5;
    }
    if (prot) {
        rpl = values[1] & 3;
        if (rpl < cpl || (values[2] & (1ULL << 17))) return vfl_exit_exception(v, run, 13, 0);
        r = vfl_segment_read(vm, v, run, (uint16_t)values[1], &cs);
        if (r) return r;
        dpl = (cs.attrib >> 5) & 3;
        if ((cs.attrib & 0x18) != 0x18 || (cs.attrib & 4 ? dpl > rpl : dpl != rpl))
            return vfl_exit_exception(v, run, 13, cs.selector & ~3U);
        if (!(cs.attrib & 0x80)) return vfl_exit_exception(v, run, 11, cs.selector & ~3U);
        if ((lm && ((int64_t)values[0] >> 47 != 0 && (int64_t)values[0] >> 47 != -1)) ||
            (!lm && values[0] > cs.limit)) return vfl_exit_exception(v, run, 13, 0);
        if (count == 5) {
            if (lm && !(values[4] & ~3ULL) && rpl != 3) {
                memset(&ss, 0, sizeof(ss));
                ss.selector = rpl;
            } else {
                r = vfl_segment_read(vm, v, run, (uint16_t)values[4], &ss);
                if (r) return r;
                if ((ss.selector & 3) != rpl || ((ss.attrib >> 5) & 3) != rpl ||
                    (ss.attrib & 0x1A) != 0x12)
                    return vfl_exit_exception(v, run, 13, ss.selector & ~3U);
                if (!(ss.attrib & 0x80)) return vfl_exit_exception(v, run, 12, ss.selector & ~3U);
            }
            if (lm && ((int64_t)values[3] >> 47 != 0 && (int64_t)values[3] >> 47 != -1))
                return vfl_exit_exception(v, run, 12, 0);
            v->sregs.ss = ss;
            v->regs.rsp = values[3];
        }
        v->sregs.cs = cs;
    } else {
        v->sregs.cs.selector = values[1];
        v->sregs.cs.base = (values[1] & 0xFFFF) << 4;
    }
    if (count == 3) vfl_set_sp(v, (sp + (uint64_t)size * 3) & mask);
    if (prot && cpl) {
        values[2] = (values[2] & ~0x3000ULL) | (v->regs.rflags & 0x3000);
        if (cpl > (int)((v->regs.rflags >> 12) & 3))
            values[2] = (values[2] & ~0x200ULL) | (v->regs.rflags & 0x200);
    }
    v->regs.rflags = (v->regs.rflags & ~vfl_mask(size)) | (values[2] & vfl_mask(size)) | 2;
    v->regs.rip = values[0];
    return 0;
}

static int vfl_deliver_once(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run) {
    uint8_t gate[16];
    uint8_t tss[8];
    vfl_segment_t cs;
    vfl_segment_t ss;
    uint64_t old_sp;
    uint64_t old_cs;
    uint64_t old_ss;
    uint64_t offset;
    uint64_t table_offset;
    uint64_t tss_offset;
    uint32_t error;
    uint16_t selector;
    unsigned vector;
    unsigned type;
    int size;
    int lm;
    int cpl;
    int dpl;
    int change;
    int ist;
    int r;

    vector = v->irq.vector;
    old_sp = vfl_sp(v);
    old_cs = v->sregs.cs.selector;
    old_ss = v->sregs.ss.selector;
    if (!(v->sregs.cr0 & 1)) {
        if ((uint64_t)vector * 4 + 3 > v->sregs.idt_limit)
            return vfl_exit_exception(v, run, 13, 0);
        r = vfl_phys_read(vm, v->sregs.idt_base + vector * 4, 4, gate);
        if (r) return vfl_memerr(v, run, r);
        offset = vfl_get16(gate);
        selector = vfl_get16(gate + 2);
        if (vfl_irq_push(vm, v, run, v->regs.rflags, 2) ||
            vfl_irq_push(vm, v, run, old_cs, 2) ||
            vfl_irq_push(vm, v, run, v->regs.rip, 2)) return 1;
        v->sregs.cs.selector = selector;
        v->sregs.cs.base = (uint64_t)selector << 4;
        v->regs.rip = offset;
        v->regs.rflags &= ~0x300ULL;
    } else {
        lm = vfl_long_mode(v);
        table_offset = vector * (lm ? 16U : 8U);
        error = vector * 8 + 2;
        if (v->irq.type != VFL_IRQ_EXCEPTION && v->irq.type != VFL_IRQ_SOFTWARE) error |= 1;
        if (table_offset + (lm ? 15U : 7U) > v->sregs.idt_limit)
            return vfl_exit_exception(v, run, 13, error);
        r = vfl_system_read(vm, v, v->sregs.idt_base + table_offset, lm ? 16 : 8, gate);
        if (r) return vfl_memerr(v, run, r);
        type = gate[5] & 31;
        if ((lm && type != 14 && type != 15) ||
            (!lm && type != 6 && type != 7 && type != 14 && type != 15))
            return vfl_exit_exception(v, run, 13, error);
        if (v->irq.type == VFL_IRQ_SOFTWARE &&
            (old_cs & 3) > ((gate[5] >> 5) & 3)) return vfl_exit_exception(v, run, 13, error);
        if (!(gate[5] & 0x80)) return vfl_exit_exception(v, run, 11, error);
        selector = vfl_get16(gate + 2);
        r = vfl_segment_read(vm, v, run, selector, &cs);
        if (r) return r;
        cpl = old_cs & 3;
        dpl = (cs.attrib >> 5) & 3;
        if ((cs.attrib & 0x18) != 0x18 || dpl > cpl)
            return vfl_exit_exception(v, run, 13, selector & ~3U);
        if (!(cs.attrib & 0x80)) return vfl_exit_exception(v, run, 11, selector & ~3U);
        if (cs.attrib & 4) dpl = cpl;
        offset = vfl_get16(gate) | ((uint64_t)vfl_get16(gate + 6) << 16);
        if (lm) offset |= (uint64_t)vfl_get32(gate + 8) << 32;
        if ((lm && ((int64_t)offset >> 47 != 0 && (int64_t)offset >> 47 != -1)) ||
            (!lm && offset > cs.limit)) return vfl_exit_exception(v, run, 13, 0);
        if (lm && (!(cs.attrib & 0x200) || (cs.attrib & 0x400)))
            return vfl_exit_exception(v, run, 13, selector & ~3U);
        size = lm ? 8 : type & 8 ? 4 : 2;
        ist = lm ? gate[4] & 7 : 0;
        change = dpl < cpl || ist;
        if (change) {
            tss_offset = lm ? (ist ? 36 + (uint64_t)(ist - 1) * 8 : 4 + (uint64_t)dpl * 8) :
                         4 + (uint64_t)dpl * 8;
            if (tss_offset + (lm ? 7U : 5U) > v->sregs.tr.limit ||
                !(v->sregs.tr.attrib & 0x80))
                return vfl_exit_exception(v, run, 10, v->sregs.tr.selector & ~3U);
            r = vfl_system_read(vm, v, v->sregs.tr.base + tss_offset, lm ? 8 : 6, tss);
            if (r) return vfl_memerr(v, run, r);
            if (lm) {
                v->regs.rsp = vfl_get64(tss);
                memset(&v->sregs.ss, 0, sizeof(v->sregs.ss));
                v->sregs.ss.selector = dpl;
            } else {
                r = vfl_segment_read(vm, v, run, vfl_get16(tss + 4), &ss);
                if (r) return r;
                if ((ss.selector & 3) != dpl || ((ss.attrib >> 5) & 3) != dpl ||
                    (ss.attrib & 0x1A) != 0x12)
                    return vfl_exit_exception(v, run, 10, ss.selector & ~3U);
                if (!(ss.attrib & 0x80)) return vfl_exit_exception(v, run, 12, ss.selector & ~3U);
                v->sregs.ss = ss;
                v->regs.rsp = vfl_get32(tss);
            }
        }
        v->sregs.cs = cs;
        v->sregs.cs.selector = (selector & ~3U) | dpl;
        if (lm) v->regs.rsp &= ~15ULL;
        if ((lm || change) &&
            (vfl_irq_push(vm, v, run, old_ss, size) ||
             vfl_irq_push(vm, v, run, old_sp, size))) return 1;
        if (vfl_irq_push(vm, v, run, v->regs.rflags, size) ||
            vfl_irq_push(vm, v, run, old_cs, size) ||
            vfl_irq_push(vm, v, run, v->regs.rip, size)) return 1;
        if (v->irq.has_error && vfl_irq_push(vm, v, run, v->irq.error_code, size)) return 1;
        if (!(type & 1)) v->regs.rflags &= ~0x200ULL;
        v->regs.rflags &= ~0x34100ULL;
        v->regs.rip = offset;
    }
    if (v->irq.type == VFL_IRQ_NMI) v->nmi_blocked = 1;
    v->int_shadow = 0;
    v->halted = 0;
    v->irq_valid = 0;
    return 2;
}

static int vfl_fault_class(unsigned vector) {
    if (vector == 14) return 2;
    return vector == 0 || (vector >= 10 && vector <= 13);
}

static int vfl_deliver_irq(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, unsigned software_length) {
    vfl_regs_t regs;
    vfl_segment_t cs;
    vfl_segment_t ss;
    uint64_t mask;
    int first;
    int second;
    int r;

    if (v->irq.type == VFL_IRQ_NMI) v->nmi_blocked = 1;
    for (;;) {
        regs = v->regs;
        cs = v->sregs.cs;
        ss = v->sregs.ss;
        r = vfl_deliver_once(vm, v, run);
        if (r == 2) return r;
        if (software_length) {
            mask = vfl_long_mode(v) ? ~0ULL : (v->sregs.cr0 & 1) ? 0xFFFFFFFFULL : 0xFFFFULL;
            regs.rip = (regs.rip & ~mask) | ((regs.rip - software_length) & mask);
            software_length = 0;
        }
        v->regs = regs;
        v->sregs.cs = cs;
        v->sregs.ss = ss;
        if (r < 0 || run->exit_reason != VFL_EXIT_EXCEPTION) return r;
        if (v->irq.type == VFL_IRQ_EXCEPTION && v->irq.vector == 8) {
            v->irq_valid = 0;
            return vfl_exit_shutdown(v, run);
        }
        first = v->irq.type == VFL_IRQ_EXCEPTION ? vfl_fault_class(v->irq.vector) : 0;
        second = vfl_fault_class(run->exception.vector);
        v->irq.vector = run->exception.vector;
        v->irq.type = VFL_IRQ_EXCEPTION;
        v->irq.error_code = run->exception.error_code;
        if ((first == 1 && second == 1) || (first == 2 && second)) {
            v->irq.vector = 8;
            v->irq.error_code = 0;
        }
        v->irq.has_error = v->irq.vector < 32 && ((0x60227D00U >> v->irq.vector) & 1U);
    }
}

static int vfl_handle_exception(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, int r) {
    if (r <= 0 || run->exit_reason != VFL_EXIT_EXCEPTION ||
        !(v->control.flags & VFL_CONTROL_EXCEPTIONS)) return r;
    v->irq.vector = run->exception.vector;
    v->irq.type = run->exception.software ? VFL_IRQ_SOFTWARE : VFL_IRQ_EXCEPTION;
    v->irq.error_code = run->exception.error_code;
    v->irq.has_error = !run->exception.software && v->irq.vector < 32 && ((0x60227D00U >> v->irq.vector) & 1U);
    v->irq_valid = 1;
    r = vfl_deliver_irq(vm, v, run, run->exception.instruction_length);
    return r == 2 ? 0 : r;
}

static int vfl_try_irq(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run) {
    if (!v->irq_valid && !vfl_irq_select(v)) return 0;
    if (v->irq.type == VFL_IRQ_EXTERNAL && (v->int_shadow || !(v->regs.rflags & 0x200))) return 0;
    if (v->irq.type == VFL_IRQ_NMI && v->nmi_blocked) return 0;
    return vfl_deliver_irq(vm, v, run, 0);
}

int vfl_wait_vcpu(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t timeout_us) {
    vfl_run_t run;
    uint64_t deadline;
    uint64_t now;
    uint64_t wake;
    uint64_t generation;
    uint64_t ticks;
    __uint128_t converted;
    int ready;
    int r;

    now = pit_get_uptime_us();
    deadline = timeout_us > ~0ULL - now ? ~0ULL : now + timeout_us;
    __atomic_store_n(&v->waiting, 1, __ATOMIC_RELEASE);
    r = 0;
    for (;;) {
        generation = descriptor_ready_generation();
        mutex_lock(&vm->execution_lock);
        vfl_timer_poll(vm, v, &run);
        ready = !v->halted || v->irq_valid || vfl_irq_ready(v) || __atomic_load_n(&v->stop, __ATOMIC_ACQUIRE);
        wake = deadline;
        if (v->apic_enabled && !(v->tmr_lvt & 0x10000)) {
            if (v->tmr_armed && v->tmr_fire_us < wake) wake = v->tmr_fire_us;
            if (v->tsc_armed && v->tsc_fire_us < wake) wake = v->tsc_fire_us;
        }
        mutex_unlock(&vm->execution_lock);
        if (ready) break;
        if (task_has_pending_signals()) {
            r = -EINTR;
            break;
        }
        now = pit_get_uptime_us();
        if (now >= deadline) {
            r = -ETIMEDOUT;
            break;
        }
        if (wake <= now) continue;
        converted = ((__uint128_t)(wake - now) * pit_get_frequency() + 999999) / 1000000;
        ticks = wake == ~0ULL || converted > ~0ULL ? ~0ULL : (uint64_t)converted;
        if (!ticks) ticks = 1;
        descriptor_ready_wait(generation, ticks);
    }
    __atomic_store_n(&v->waiting, 0, __ATOMIC_RELEASE);
    return r;
}

static int vfl_run_main(vfl_vm_t *vm, vfl_vcpu_t *vcpu, vfl_run_t *run) {
    vfl_run_t input;
    uint64_t steps;
    uint64_t budget;
    uint64_t chunk;
    uint64_t executed;
#if CONFIG_VFL_DEBUG
    uint64_t ip;
    uint64_t ripmask;
#endif
    int r;
#if CONFIG_VFL_DEBUG
    int pending;
#endif

    input = *run;
#if CONFIG_VFL_DEBUG
    pending = vcpu->pending;
#endif
    r = vfl_complete(vm, vcpu, &input, run);
    r = vfl_handle_exception(vm, vcpu, run, r);
    if (r != 0) return r < 0 ? -1 : 0;
#if CONFIG_VFL_DEBUG
    if (pending && pending != VFL_P_MMIO_FILL && !vcpu->pending &&
        (vcpu->control.flags & VFL_CONTROL_SINGLE_STEP)) {
        run->exit_reason = VFL_EXIT_DEBUG;
        return 0;
    }
#endif
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
        if (r && r != 2) return r < 0 ? -1 : 0;
        if (vcpu->halted) {
            run->exit_reason = VFL_EXIT_HLT;
            return 0;
        }
#if CONFIG_VFL_DEBUG
        ripmask = vfl_long_mode(vcpu) ? ~0ULL :
                  (vcpu->sregs.cr0 & 1ULL) ? 0xFFFFFFFFULL : 0xFFFFULL;
        ip = vfl_seg_base(vcpu, 1) + (vcpu->regs.rip & ripmask);
        if (vfl_breakpoint(vcpu, ip)) {
            run->exit_reason = VFL_EXIT_DEBUG;
            return 0;
        }
#endif
        chunk = 1024 - (steps & 1023);
        if (chunk > budget - steps) chunk = budget - steps;
        if (vcpu->int_shadow) chunk = 1;
#if CONFIG_VFL_DEBUG
        if (vcpu->control.flags & VFL_CONTROL_SINGLE_STEP) chunk = 1;
#endif
        r = vfl_hot_run(vm, vcpu, run, chunk, &executed);
        r = vfl_handle_exception(vm, vcpu, run, r);
        steps += executed;
#if CONFIG_VFL_DEBUG
        if (vcpu->stats) vcpu->stats->instructions += executed;
#endif
        if (r) return r < 0 ? -1 : 0;
        if (!executed) {
            if (__atomic_load_n(&vcpu->stop, __ATOMIC_ACQUIRE)) continue;
            r = vfl_step(vm, vcpu, run);
            r = vfl_handle_exception(vm, vcpu, run, r);
            steps++;
#if CONFIG_VFL_DEBUG
            if (vcpu->stats) vcpu->stats->instructions++;
#endif
            if (r) return r < 0 ? -1 : 0;
        }
#if CONFIG_VFL_DEBUG
        if (vcpu->control.flags & VFL_CONTROL_SINGLE_STEP) {
            run->exit_reason = VFL_EXIT_DEBUG;
            return 0;
        }
#endif
    }
    run->exit_reason = VFL_EXIT_TIMESLICE;
    return 0;
}

int vfl_execute(vfl_vm_t *vm, vfl_vcpu_t *vcpu, vfl_run_t *run) {
    uint8_t host_fpu[512] __attribute__((aligned(16)));
    uint64_t cr2;
    uint32_t vector;
    uint32_t code;
    int r;

    mutex_lock(&vm->execution_lock);
    vfl_fpu_begin(host_fpu);
    r = vfl_run_main(vm, vcpu, run);
    vfl_flags_sync(vcpu);
    vcpu->lazy_enabled = 0;
    vcpu->last_exit = *run;
#if CONFIG_VFL_DEBUG
    if (vcpu->stats && run->exit_reason <= VFL_EXIT_DEBUG)
        vcpu->stats->exits[run->exit_reason]++;
#endif
    if (run->exit_reason == VFL_EXIT_INTERNAL) {
        cr2 = vcpu->sregs.cr2;
        vector = vcpu->mmu_vec;
        code = vcpu->mmu_code;
        run->internal.rip = vcpu->regs.rip;
        run->internal.length = 0;
        memset(run->internal.bytes, 0, sizeof(run->internal.bytes));
        while (run->internal.length < sizeof(run->internal.bytes)) {
            if (vfl_fetch_buf(vm, vcpu, vfl_seg_base(vcpu, 1) + vcpu->regs.rip +
                run->internal.length, run->internal.bytes + run->internal.length, 1)) break;
            run->internal.length++;
        }
        vcpu->sregs.cr2 = cr2;
        vcpu->mmu_vec = vector;
        vcpu->mmu_code = code;
        vcpu->last_exit = *run;
    }
    vfl_fpu_end(host_fpu);
    mutex_unlock(&vm->execution_lock);
    return r;
}

int vfl_vm_info(uint32_t vm_id, vfl_vm_info_t *info) {
    vfl_vm_t *vm;
    vfl_mem_slot_t *s;
    uint64_t total;
    uint64_t resident;

    if (!info) return -EINVAL;
    vm = vfl_get_vm(vm_id);
    if (!vm) return -EINVAL;

    total = 0;
    resident = 0;
    for (s = vm->mem_slots; s; s = s->next) {
        total += s->size;
        if (s->owned) resident += __atomic_load_n(&s->resident_pages, __ATOMIC_RELAXED) << 12;
    }

    info->vm_id = vm_id;
    info->nr_vcpus = (uint32_t)vm->nr_vcpus;
    info->nr_mem_slots = (uint32_t)vm->nr_mem_slots;
    info->total_mem = total;
    info->resident_mem = resident;
    return 0;
}
