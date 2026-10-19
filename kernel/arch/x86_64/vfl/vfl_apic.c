#include "vfl_priv.h"
#include <lebirun/common.h>
#include <lebirun/pit.h>
#include <lebirun/task.h>
#include <string.h>
#include <stdint.h>
#include "../syscalls/syscall_defs.h"

#define APIC_ID     0x20
#define APIC_TPR    0x80
#define APIC_EOI    0xB0
#define APIC_SPUR   0xF0
#define APIC_ICR_LO 0x300
#define APIC_ICR_HI 0x310
#define APIC_LVT_TMR 0x320
#define APIC_TMR_INIT 0x380
#define APIC_TMR_CUR 0x390
#define APIC_TMR_DIV 0x3E0

#define X2_ID       0x802
#define X2_VER      0x803
#define X2_TPR      0x808
#define X2_EOI      0x80B
#define X2_SPUR     0x80F
#define X2_ICR      0x830
#define X2_LVT_TMR  0x832
#define X2_TMR_INIT 0x838
#define X2_TMR_CUR  0x839
#define X2_TMR_DIV  0x83E
#define X2_SELF_IPI 0x83F

#define VFL_TSC_KHZ 1000000ULL

static uint32_t vfl_tmr_divisor(uint32_t div) {
    switch ((div & 3U) | ((div >> 1) & 4U)) {
        case 0: return 2;
        case 1: return 4;
        case 2: return 8;
        case 3: return 16;
        case 4: return 32;
        case 5: return 64;
        case 6: return 128;
        default: return 1;
    }
}

int vfl_apic_x2(vfl_vcpu_t *v) {
    return (v->msr_apic_base & 0xC00ULL) == 0xC00ULL;
}

static uint64_t vfl_apic_base_addr(vfl_vcpu_t *v) {
    return v->msr_apic_base & 0xFFFFFFFFFFFFF000ULL;
}

int vfl_apic_mmio(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t phys) {
    uint64_t base;
    (void)vm;
    (void)v;
    base = v->msr_apic_base & 0xFFFFFFFFFFFFF000ULL;
    if (!base)
        base = 0xFEE00000ULL;
    return phys >= base && phys < base + 0x1000;
}

static int vfl_apic_highest(uint32_t *bits) {
    uint32_t word;
    int i;

    for (i = 7; i >= 0; i--) {
        word = __atomic_load_n(&bits[i], __ATOMIC_ACQUIRE);
        if (word) return i * 32 + 31 - __builtin_clz(word);
    }
    return -1;
}

void vfl_queue_irq(vfl_vcpu_t *v, uint8_t vector, uint8_t type) {
    if (type != VFL_IRQ_NMI && vector < 16) return;
    if (type == VFL_IRQ_NMI)
        __atomic_store_n(&v->nmi_pending, 1, __ATOMIC_RELEASE);
    else {
        __atomic_fetch_or(&v->apic_irr[vector >> 5], 1U << (vector & 31), __ATOMIC_RELEASE);
        __atomic_fetch_or(&v->apic_pending, 1U << (vector >> 5), __ATOMIC_RELEASE);
    }
    if (__atomic_load_n(&v->waiting, __ATOMIC_ACQUIRE)) descriptor_ready_notify();
}

int vfl_irq_ready(vfl_vcpu_t *v) {
    int vector;
    int service;
    int priority;

    if (__atomic_load_n(&v->nmi_pending, __ATOMIC_ACQUIRE) && !v->nmi_blocked) return 1;
    if (!__atomic_load_n(&v->apic_pending, __ATOMIC_ACQUIRE) ||
        v->int_shadow || !(v->regs.rflags & 0x200)) return 0;
    vector = vfl_apic_highest(v->apic_irr);
    if (vector < 0) return 0;
    if (!(v->sregs.cr0 & 1)) return 1;
    service = vfl_apic_highest(v->apic_isr);
    priority = v->apic_tpr & 0xF0;
    if (service >= 0 && (service & 0xF0) > priority) priority = service & 0xF0;
    return (vector & 0xF0) > priority;
}

int vfl_irq_select(vfl_vcpu_t *v) {
    int vector;

    if (!vfl_irq_ready(v)) return 0;
    memset(&v->irq, 0, sizeof(v->irq));
    if (!v->nmi_blocked && __atomic_exchange_n(&v->nmi_pending, 0, __ATOMIC_ACQ_REL)) {
        v->irq.vector = 2;
        v->irq.type = VFL_IRQ_NMI;
    } else {
        vector = vfl_apic_highest(v->apic_irr);
        if (vector < 0) return 0;
        __atomic_fetch_and(&v->apic_irr[vector >> 5], ~(1U << (vector & 31)), __ATOMIC_ACQ_REL);
        if (!__atomic_load_n(&v->apic_irr[vector >> 5], __ATOMIC_ACQUIRE)) {
            __atomic_fetch_and(&v->apic_pending, ~(1U << (vector >> 5)), __ATOMIC_ACQ_REL);
            if (__atomic_load_n(&v->apic_irr[vector >> 5], __ATOMIC_ACQUIRE))
                __atomic_fetch_or(&v->apic_pending, 1U << (vector >> 5), __ATOMIC_RELEASE);
        }
        v->irq.vector = (uint8_t)vector;
        if (v->sregs.cr0 & 1)
            v->apic_isr[vector >> 5] |= 1U << (vector & 31);
    }
    v->irq_valid = 1;
    return 1;
}

void vfl_apic_eoi(vfl_vcpu_t *v) {
    int vector;

    vector = vfl_apic_highest(v->apic_isr);
    if (vector >= 0) v->apic_isr[vector >> 5] &= ~(1U << (vector & 31));
}

static void vfl_tmr_arm(vfl_vcpu_t *v) {
    uint64_t now = pit_get_uptime_us();

    v->tmr_periodic = (v->tmr_lvt >> 17) & 1U;
    if ((v->tmr_lvt & 0x10000U) || (v->tmr_lvt & 0x60000U) == 0x40000U ||
        !v->apic_enabled || !v->tmr_init) {
        v->tmr_armed = 0;
        return;
    }
    v->tmr_fire_us = now + (uint64_t)v->tmr_init * vfl_tmr_divisor(v->tmr_div);
    v->tmr_armed = 1;
}

static uint32_t vfl_tmr_current(vfl_vcpu_t *v) {
    uint64_t now;
    uint32_t div;

    if (!v->tmr_armed)
        return 0;
    now = pit_get_uptime_us();
    if (now >= v->tmr_fire_us)
        return 0;
    div = vfl_tmr_divisor(v->tmr_div);
    return (uint32_t)((v->tmr_fire_us - now) / div);
}

void vfl_timer_poll(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run) {
    uint64_t now;
    uint32_t vec;
    (void)vm;
    (void)run;

    if (!v->apic_enabled)
        return;
    vec = v->tmr_lvt & 0xFFU;
    if (v->tmr_armed) {
        now = pit_get_uptime_us();
        if (now >= v->tmr_fire_us) {
            if (v->tmr_periodic)
                v->tmr_fire_us = now + (uint64_t)v->tmr_init *
                                 vfl_tmr_divisor(v->tmr_div);
            else
                v->tmr_armed = 0;
            if (!(v->tmr_lvt & 0x10000U))
                vfl_queue_irq(v, (uint8_t)vec, VFL_IRQ_EXTERNAL);
        }
    }
    if (v->tsc_armed) {
        now = pit_get_uptime_us();
        if (now >= v->tsc_fire_us) {
            v->tsc_armed = 0;
            if (!(v->tmr_lvt & 0x10000U))
                vfl_queue_irq(v, (uint8_t)vec, VFL_IRQ_EXTERNAL);
        }
    }
}

int vfl_ipi(vfl_vm_t *vm, vfl_vcpu_t *src, uint32_t icr_lo, uint32_t icr_hi) {
    uint32_t short_hand = (icr_lo >> 18) & 3U;
    uint32_t mode = (icr_lo >> 8) & 7U;
    uint32_t dest = vfl_apic_x2(src) ? icr_hi : (icr_hi >> 24) & 0xFFU;
    uint8_t vec = (uint8_t)(icr_lo & 0xFFU);
    vfl_vcpu_t *t;
    unsigned i;

    for (t = vm->vcpus; t; t = t->next) {
        int hit = 0;

        if (t == src && short_hand == 3)
            continue;
        switch (short_hand) {
            case 0:
                hit = (((icr_lo >> 11) & 1U) && dest == 0xFFU) ||
                      (!((icr_lo >> 11) & 1U) && t->apic_id == dest);
                break;
            case 1:
                hit = (t == src);
                break;
            case 2:
                hit = 1;
                break;
            case 3:
                hit = 1;
                break;
        }
        if (!hit)
            continue;
        switch (mode) {
            case 0:
            case 1:
                vfl_queue_irq(t, vec, VFL_IRQ_EXTERNAL);
                break;
            case 4:
                vfl_queue_irq(t, vec, VFL_IRQ_NMI);
                break;
            case 5:
                if ((icr_lo & 0x8000U) && !(icr_lo & 0x4000U)) break;
                t->halted = 1;
                t->pending = VFL_P_NONE;
                t->irq_valid = 0;
                for (i = 0; i < 8; i++)
                    __atomic_store_n(&t->apic_irr[i], 0, __ATOMIC_RELEASE);
                memset(t->apic_isr, 0, sizeof(t->apic_isr));
                __atomic_store_n(&t->apic_pending, 0, __ATOMIC_RELEASE);
                for (i = 0; i < 8; i++)
                    if (__atomic_load_n(&t->apic_irr[i], __ATOMIC_ACQUIRE))
                        __atomic_fetch_or(&t->apic_pending, 1U << i, __ATOMIC_RELEASE);
                __atomic_store_n(&t->nmi_pending, 0, __ATOMIC_RELEASE);
                t->nmi_blocked = 0;
                break;
            case 6:
                t->sregs.cs.selector = (uint16_t)(vec << 8);
                t->sregs.cs.base = (uint64_t)vec << 12;
                t->regs.rip = 0;
                t->halted = 0;
                t->pending = VFL_P_NONE;
                t->irq_valid = 0;
                for (i = 0; i < 8; i++)
                    __atomic_store_n(&t->apic_irr[i], 0, __ATOMIC_RELEASE);
                memset(t->apic_isr, 0, sizeof(t->apic_isr));
                __atomic_store_n(&t->apic_pending, 0, __ATOMIC_RELEASE);
                for (i = 0; i < 8; i++)
                    if (__atomic_load_n(&t->apic_irr[i], __ATOMIC_ACQUIRE))
                        __atomic_fetch_or(&t->apic_pending, 1U << i, __ATOMIC_RELEASE);
                __atomic_store_n(&t->nmi_pending, 0, __ATOMIC_RELEASE);
                t->nmi_blocked = 0;
                break;
            default:
                break;
        }
        if (__atomic_load_n(&t->waiting, __ATOMIC_ACQUIRE)) descriptor_ready_notify();
    }
    return 0;
}

static uint32_t vfl_apic_reg_read(vfl_vcpu_t *v, uint32_t off) {
    int service;
    uint32_t priority;

    if (off >= 0x100 && off <= 0x170 && !(off & 15)) return v->apic_isr[(off - 0x100) >> 4];
    if (off >= 0x200 && off <= 0x270 && !(off & 15))
        return __atomic_load_n(&v->apic_irr[(off - 0x200) >> 4], __ATOMIC_ACQUIRE);
    switch (off) {
        case 0x30: return 0x50014;
        case 0xA0:
            service = vfl_apic_highest(v->apic_isr);
            priority = v->apic_tpr & 0xFF;
            if (service >= 0 && (uint32_t)(service & 0xF0) > (priority & 0xF0))
                priority = service & 0xF0;
            return priority;
        case APIC_ID: return v->apic_id << 24;
        case APIC_TPR: return v->apic_tpr & 0xFFU;
        case APIC_SPUR: return v->apic_spurious;
        case APIC_ICR_LO: return v->apic_icr_lo;
        case APIC_ICR_HI: return v->apic_icr_hi;
        case APIC_LVT_TMR: return v->tmr_lvt;
        case APIC_TMR_INIT: return v->tmr_init;
        case APIC_TMR_CUR: return vfl_tmr_current(v);
        case APIC_TMR_DIV: return v->tmr_div & 0xBU;
        default: return 0;
    }
}

int vfl_apic_read(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t phys, uint8_t *out, uint64_t len) {
    uint64_t base = vfl_apic_base_addr(v);
    uint32_t off = (uint32_t)(phys - base);
    uint32_t reg = off & ~3U;
    uint32_t val;
    uint64_t i;
    (void)vm;

    if (off >= 0x1000 || (off & 3U) + len > 4 || len > 4)
        return 1;
    val = vfl_apic_reg_read(v, reg);
    for (i = 0; i < len; i++)
        out[i] = (uint8_t)(val >> ((off & 3U) + i) * 8);
    return 0;
}

int vfl_apic_write(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t phys, uint8_t *in, uint64_t len) {
    uint64_t base = vfl_apic_base_addr(v);
    uint32_t off = (uint32_t)(phys - base);
    uint32_t reg = off & ~3U;
    uint32_t cur;
    uint32_t mask = 0;
    uint32_t val;
    uint64_t i;

    if (off >= 0x1000 || (off & 3U) + len > 4 || len > 4)
        return 1;
    switch (reg) {
        case APIC_EOI:
            vfl_apic_eoi(v);
            return 0;
        case APIC_ID:
        case APIC_TMR_CUR:
            return 0;
        case APIC_TPR:
        case APIC_SPUR:
        case APIC_ICR_LO:
        case APIC_ICR_HI:
        case APIC_LVT_TMR:
        case APIC_TMR_INIT:
        case APIC_TMR_DIV:
            break;
        default:
            return 0;
    }
    switch (reg) {
        case APIC_TPR: cur = v->apic_tpr; break;
        case APIC_SPUR: cur = v->apic_spurious; break;
        case APIC_ICR_LO: cur = v->apic_icr_lo; break;
        case APIC_LVT_TMR: cur = v->tmr_lvt; break;
        case APIC_TMR_INIT: cur = v->tmr_init; break;
        case APIC_TMR_DIV: cur = v->tmr_div; break;
        default: cur = v->apic_icr_hi; break;
    }
    for (i = 0; i < len; i++)
        mask |= 0xFFU << (((off & 3U) + i) * 8);
    val = 0;
    for (i = 0; i < len; i++)
        val |= (uint32_t)in[i] << (((off & 3U) + i) * 8);
    cur = (cur & ~mask) | (val & mask);
    switch (reg) {
        case APIC_TPR:
            v->apic_tpr = cur & 0xFFU;
            v->sregs.cr8 = (v->apic_tpr >> 4) & 0xFULL;
            break;
        case APIC_SPUR:
            v->apic_spurious = cur & 0x1FFU;
            v->apic_enabled = (cur & 0x100U) ? 1 : 0;
            break;
        case APIC_ICR_HI:
            v->apic_icr_hi = cur;
            break;
        case APIC_ICR_LO:
            v->apic_icr_lo = cur & ~0x3000U;
            vfl_ipi(vm, v, v->apic_icr_lo, v->apic_icr_hi);
            break;
        case APIC_LVT_TMR:
            v->tmr_lvt = cur;
            if (v->tmr_armed)
                vfl_tmr_arm(v);
            break;
        case APIC_TMR_INIT:
            v->tmr_init = cur;
            vfl_tmr_arm(v);
            break;
        case APIC_TMR_DIV:
            v->tmr_div = cur & 0xBU;
            break;
        default:
            break;
    }
    return 0;
}

int vfl_x2apic_read(vfl_vcpu_t *v, uint32_t index, uint64_t *val) {
    if (!vfl_apic_x2(v))
        return -1;
    if (index >= 0x810 && index <= 0x827) {
        *val = vfl_apic_reg_read(v, (index - 0x800) << 4);
        return 0;
    }
    switch (index) {
        case 0x80A: *val = vfl_apic_reg_read(v, 0xA0); return 0;
        case X2_ID: *val = v->apic_id; return 0;
        case X2_VER: *val = 0x50014; return 0;
        case X2_TPR: *val = v->apic_tpr & 0xFFU; return 0;
        case X2_SPUR: *val = v->apic_spurious; return 0;
        case X2_ICR: *val = (uint64_t)v->apic_icr_hi << 32 | v->apic_icr_lo; return 0;
        case X2_LVT_TMR: *val = v->tmr_lvt; return 0;
        case X2_TMR_INIT: *val = v->tmr_init; return 0;
        case X2_TMR_CUR: *val = vfl_tmr_current(v); return 0;
        case X2_TMR_DIV: *val = v->tmr_div & 0xBU; return 0;
        default: *val = 0; return 0;
    }
}

int vfl_x2apic_write(vfl_vm_t *vm, vfl_vcpu_t *v, uint32_t index, uint64_t val) {
    uint32_t lo;
    uint32_t hi;

    if (!vfl_apic_x2(v))
        return -1;
    switch (index) {
        case X2_TPR:
            v->apic_tpr = (uint32_t)val & 0xFFU;
            v->sregs.cr8 = (v->apic_tpr >> 4) & 0xFULL;
            return 0;
        case X2_EOI:
            if (val) return -1;
            vfl_apic_eoi(v);
            return 0;
        case X2_SPUR:
            v->apic_spurious = (uint32_t)val & 0x1FFU;
            v->apic_enabled = (val & 0x100U) ? 1 : 0;
            return 0;
        case X2_ICR:
            lo = (uint32_t)val;
            hi = (uint32_t)(val >> 32);
            v->apic_icr_hi = hi;
            v->apic_icr_lo = lo & ~0x3000U;
            vfl_ipi(vm, v, v->apic_icr_lo, v->apic_icr_hi);
            return 0;
        case X2_LVT_TMR:
            v->tmr_lvt = (uint32_t)val;
            if (v->tmr_armed)
                vfl_tmr_arm(v);
            return 0;
        case X2_TMR_INIT:
            v->tmr_init = (uint32_t)val;
            vfl_tmr_arm(v);
            return 0;
        case X2_TMR_DIV:
            v->tmr_div = (uint32_t)val & 0xBU;
            return 0;
        case X2_SELF_IPI:
            vfl_queue_irq(v, (uint8_t)val, VFL_IRQ_EXTERNAL);
            return 0;
        case X2_ID:
        case X2_VER:
        case X2_TMR_CUR:
            return -1;
        default:
            return 0;
    }
}
