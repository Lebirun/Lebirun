#include "vfl_priv.h"
#include <lebirun/common.h>
#include <lebirun/pit.h>
#include <lebirun/vfs.h>
#include <lebirun/mem_map.h>
#include <lebirun/task.h>
#include <lebirun/mutex.h>
#include <lebirun/uaccess.h>
#include <string.h>
#include <stdint.h>
#include "../syscalls/syscall_defs.h"

static mutex_t vfl_lock;
static int vfl_lock_ready;
static vfl_vm_t *vfl_vms;
static uint32_t vfl_next_id;

#define VFL_DIRECT_MAX 0x40000000ULL
#define VFL_ROUND_PAGES(x) (((uint64_t)(x) + 0xFFFULL) >> 12)

static void vfl_free_slot_dirty(vfl_mem_slot_t *slot) {
    if (!slot->dirty)
        return;
    vfl_pages_free(slot->dirty,
                   VFL_ROUND_PAGES((slot->dirty_pages + 7) >> 3));
    slot->dirty = NULL;
    slot->dirty_pages = 0;
}

static void vfl_free_slot_pages(vfl_mem_slot_t *slot) {
    uint64_t npages;

    if (!slot->ram_phys)
        return;
    npages = (slot->size + 0xFFFULL) >> 12;
    if (npages)
        pfa_free_contiguous(slot->ram_phys, npages);
    slot->ram_phys = 0;
}

static void vfl_free_slot(vfl_mem_slot_t *slot) {
    if (slot->owned) {
        if (slot->ram_phys)
            vfl_free_slot_pages(slot);
        else
            kfree(slot->host_mem);
    }
    vfl_free_slot_dirty(slot);
    vfl_code_free(slot);
    vfl_pages_free(slot, VFL_ROUND_PAGES(sizeof(vfl_mem_slot_t)));
}

void *vfl_pages_alloc(uint64_t n) {
    uint64_t base;
    uint64_t i;

    if (!n)
        return NULL;
    base = pfa_alloc_contiguous_below(n, VFL_DIRECT_MAX);
    if (!base)
        return NULL;
    for (i = 0; i < n; i++) {
        if (pt_ensure_phys_mapped(base + i * 0x1000ULL) < 0) {
            pfa_free_contiguous(base, n);
            return NULL;
        }
    }
    memset((void *)(base + KERNEL_VMA), 0, (size_t)(n << 12));
    return (void *)(base + KERNEL_VMA);
}

void vfl_pages_free(void *va, uint64_t n) {
    uint64_t base;

    if (!va || !n)
        return;
    base = (uint64_t)va - KERNEL_VMA;
    pfa_free_contiguous(base, n);
}

static int vfl_alloc_slot_pages(uint64_t size, uint64_t *phys_out,
                                uint8_t **host_out) {
    uint64_t npages = (size + 0xFFFULL) >> 12;
    uint8_t *host;

    if (!npages)
        return -1;
    host = (uint8_t *)vfl_pages_alloc(npages);
    if (!host)
        return -1;
    *phys_out = (uint64_t)host - KERNEL_VMA;
    *host_out = host;
    return 0;
}

vfl_vm_t *vfl_get_vm(uint32_t vm_id) {
    vfl_vm_t *vm;

    for (vm = vfl_vms; vm; vm = vm->next)
        if (vm->id == vm_id)
            return vm;
    return NULL;
}

vfl_vcpu_t *vfl_get_vcpu(uint32_t vm_id, uint32_t vcpu_id) {
    vfl_vm_t *vm = vfl_get_vm(vm_id);
    vfl_vcpu_t *v;

    if (!vm) return NULL;
    for (v = vm->vcpus; v; v = v->next)
        if (v->id == vcpu_id)
            return v;
    return NULL;
}

vfl_mem_slot_t *vfl_find_slot_num(vfl_vm_t *vm, uint32_t slot) {
    vfl_mem_slot_t *s;

    for (s = vm->mem_slots; s; s = s->next)
        if (s->slot == slot)
            return s;
    return NULL;
}

static void vfl_init_vcpu_state(vfl_vcpu_t *v) {
    vfl_vcpu_t *next = v->next;
    uint32_t id = v->id;

    memset(v, 0, sizeof(vfl_vcpu_t));
    v->next = next;
    v->id = id;
    v->regs.rflags = 0x2;
    v->sregs.cs.selector = 0;
    v->sregs.cs.base = 0;
    v->sregs.cs.limit = 0xFFFFFFFF;
    v->sregs.cs.attrib = 0x009B;
    v->sregs.ds.limit = 0xFFFFFFFF;
    v->sregs.ds.attrib = 0x0093;
    v->sregs.es = v->sregs.ds;
    v->sregs.fs = v->sregs.ds;
    v->sregs.gs = v->sregs.ds;
    v->sregs.ss = v->sregs.ds;
    v->sregs.cr0 = 0x10;
    v->sregs.cr8 = 0;
    v->msr_pat = 0x0007040600070406ULL;
    v->msr_apic_base = 0xFEE00900ULL;
    v->msr_misc = (1ULL << 12);
    v->apic_tpr = 0;
    v->apic_spurious = 0xFF;
    v->apic_enabled = 1;
    v->fpu.fcw = 0x37F;
    v->fpu.ftw = 0xFFFF;
    v->fpu.mxcsr = 0x1F80;
}

int vfl_create_vm(void) {
    vfl_vm_t *vm = (vfl_vm_t *)vfl_pages_alloc(VFL_ROUND_PAGES(sizeof(vfl_vm_t)));

    if (!vm) return -ENOSPC;
    memset(vm, 0, sizeof(vfl_vm_t));
    vm->id = vfl_next_id++;
    vm->tlb_epoch = 1;
    vm->next = vfl_vms;
    vfl_vms = vm;
    return (int)vm->id;
}

int vfl_destroy_vm(uint32_t vm_id) {
    vfl_vm_t *vm;
    vfl_vm_t **link;
    vfl_vcpu_t *v;
    vfl_mem_slot_t *s;

    vm = vfl_get_vm(vm_id);
    if (!vm) return -EINVAL;
    if (vfl_vm_running(vm)) return -EBUSY;

    while ((s = vm->mem_slots) != NULL) {
        vm->mem_slots = s->next;
        vfl_free_slot(s);
    }
    while ((v = vm->vcpus) != NULL) {
        vm->vcpus = v->next;
        if (v->stats) kfree(v->stats);
        if (v->breakpoints) kfree(v->breakpoints);
        vfl_pages_free(v, VFL_ROUND_PAGES(sizeof(vfl_vcpu_t)));
    }
    for (link = &vfl_vms; *link; link = &(*link)->next) {
        if (*link == vm) {
            *link = vm->next;
            break;
        }
    }
    vfl_pages_free(vm, VFL_ROUND_PAGES(sizeof(vfl_vm_t)));
    return 0;
}

int vfl_create_vcpu(uint32_t vm_id) {
    vfl_vm_t *vm;
    vfl_vcpu_t *v;

    vm = vfl_get_vm(vm_id);
    if (!vm) return -EINVAL;
    if (vfl_vm_running(vm)) return -EBUSY;
    v = (vfl_vcpu_t *)vfl_pages_alloc(VFL_ROUND_PAGES(sizeof(vfl_vcpu_t)));
    if (!v) return -ENOSPC;
    v->id = vm->next_vcpu_id++;
    vfl_init_vcpu_state(v);
    v->apic_id = v->id;
    if (v->id > 0)
        v->halted = 1;
    v->next = vm->vcpus;
    vm->vcpus = v;
    vm->nr_vcpus++;
    return (int)v->id;
}

int vfl_reset_vcpu(uint32_t vm_id, uint32_t vcpu_id) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);

    if (!v) return -EINVAL;
    if (v->running) return -EBUSY;
    if (v->stats) kfree(v->stats);
    if (v->breakpoints) kfree(v->breakpoints);
    vfl_init_vcpu_state(v);
    return 0;
}

int vfl_set_regs(uint32_t vm_id, uint32_t vcpu_id, const vfl_regs_t *regs) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);

    if (!v || !regs) return -EINVAL;
    if (v->running) return -EBUSY;
    memcpy(&v->regs, regs, sizeof(vfl_regs_t));
    v->regs.rflags |= 0x2;
    v->halted = 0;
    v->pending = VFL_P_NONE;
    return 0;
}

int vfl_get_regs(uint32_t vm_id, uint32_t vcpu_id, vfl_regs_t *regs) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);

    if (!v || !regs) return -EINVAL;
    if (v->running) return -EBUSY;
    memcpy(regs, &v->regs, sizeof(vfl_regs_t));
    return 0;
}

int vfl_set_sregs(uint32_t vm_id, uint32_t vcpu_id, const vfl_sregs_t *sregs) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);

    if (!v || !sregs) return -EINVAL;
    if (v->running) return -EBUSY;
    memcpy(&v->sregs, sregs, sizeof(vfl_sregs_t));
    v->sregs.cr8 &= 0xFULL;
    v->apic_tpr = (uint32_t)(v->sregs.cr8 << 4);
    v->pending = VFL_P_NONE;
    __atomic_add_fetch(&vfl_get_vm(vm_id)->tlb_epoch, 1, __ATOMIC_RELEASE);
    return 0;
}

int vfl_get_sregs(uint32_t vm_id, uint32_t vcpu_id, vfl_sregs_t *sregs) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);

    if (!v || !sregs) return -EINVAL;
    if (v->running) return -EBUSY;
    memcpy(sregs, &v->sregs, sizeof(vfl_sregs_t));
    return 0;
}

int vfl_set_fpu(uint32_t vm_id, uint32_t vcpu_id, const vfl_fpu_t *fpu) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);

    if (!v || !fpu) return -EINVAL;
    if (v->running) return -EBUSY;
    memcpy(&v->fpu, fpu, sizeof(vfl_fpu_t));
    v->fpu.fcw |= 0x40;
    return 0;
}

int vfl_get_fpu(uint32_t vm_id, uint32_t vcpu_id, vfl_fpu_t *fpu) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);

    if (!v || !fpu) return -EINVAL;
    if (v->running) return -EBUSY;
    memcpy(fpu, &v->fpu, sizeof(vfl_fpu_t));
    return 0;
}

int vfl_inject_irq(uint32_t vm_id, uint32_t vcpu_id, const vfl_irq_t *irq) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);

    if (!v || !irq) return -EINVAL;
    if (irq->type != VFL_IRQ_EXTERNAL && irq->type != VFL_IRQ_NMI) return -EINVAL;
    v->irq.vector = irq->vector;
    v->irq.type = irq->type;
    v->irq.has_error = irq->has_error ? 1 : 0;
    v->irq.error_code = irq->error_code;
    v->irq_valid = 1;
    return 0;
}

int vfl_msr_read(vfl_vcpu_t *v, uint32_t index, uint64_t *val) {
    if (index >= 0x800U && index <= 0xBFFU)
        return vfl_x2apic_read(v, index, val);
    switch (index) {
        case 0x10: *val = v->tsc + v->tsc_offset; return 0;
        case 0x1A0: *val = v->msr_misc; return 0;
        case 0x1B: *val = v->msr_apic_base; return 0;
        case 0x277: *val = v->msr_pat; return 0;
        case 0x6E0: *val = v->msr_tsc_deadline; return 0;
        case 0x200: case 0x201: case 0x202: case 0x203:
        case 0x204: case 0x205: case 0x206: case 0x207:
        case 0x208: case 0x209: case 0x20A: case 0x20B:
        case 0x20C: case 0x20D: case 0x20E: case 0x20F:
        case 0x250: case 0x258: case 0x259: case 0x268:
        case 0x269: case 0x26A: case 0x26B: case 0x26C:
        case 0x26D: case 0x26E: case 0x26F:
            *val = 0; return 0;
        case 0x174: *val = v->msr_sysenter_cs; return 0;
        case 0x175: *val = v->msr_sysenter_esp; return 0;
        case 0x176: *val = v->msr_sysenter_eip; return 0;
        case 0xC0000080: *val = v->sregs.efer; return 0;
        case 0xC0000081: *val = v->msr_star; return 0;
        case 0xC0000082: *val = v->msr_lstar; return 0;
        case 0xC0000083: *val = v->msr_cstar; return 0;
        case 0xC0000084: *val = v->msr_sfmask; return 0;
        case 0xC0000100: *val = v->msr_fs_base; return 0;
        case 0xC0000101: *val = v->msr_gs_base; return 0;
        case 0xC0000102: *val = v->msr_kernel_gs_base; return 0;
        case 0xC0000103: *val = v->msr_tsc_aux; return 0;
        default: return -1;
    }
}

int vfl_msr_write(vfl_vm_t *vm, vfl_vcpu_t *v, uint32_t index, uint64_t val) {
    if (index >= 0x800U && index <= 0xBFFU)
        return vfl_x2apic_write(vm, v, index, val);
    switch (index) {
        case 0x10: v->tsc = val - v->tsc_offset; return 0;
        case 0x1A0:
            if (val & ~(1ULL << 12)) return -1;
            v->msr_misc = val; return 0;
        case 0x1B:
            v->msr_apic_base = (val & 0xFFFFFFFFFFFFF000ULL) |
                               (val & 0xC00ULL) | (v->msr_apic_base & 0x100ULL);
            v->apic_enabled = (val & 0x800ULL) ? 1 : 0;
            return 0;
        case 0x277:
            if (val & 0xF8F8F8F8F8F8F8F8ULL) return -1;
            v->msr_pat = val; return 0;
        case 0x6E0: {
            uint64_t now;
            uint64_t cur;

            v->msr_tsc_deadline = val;
            if (!val) {
                v->tsc_armed = 0;
                return 0;
            }
            cur = v->tsc + v->tsc_offset;
            now = pit_get_uptime_us();
            v->tsc_fire_us = now + (val > cur ? (val - cur) / 1000ULL : 0ULL);
            v->tsc_armed = 1;
            return 0;
        }
        case 0x200: case 0x201: case 0x202: case 0x203:
        case 0x204: case 0x205: case 0x206: case 0x207:
        case 0x208: case 0x209: case 0x20A: case 0x20B:
        case 0x20C: case 0x20D: case 0x20E: case 0x20F:
        case 0x250: case 0x258: case 0x259:
            return 0;
        case 0x268: case 0x269: case 0x26A: case 0x26B:
        case 0x26C: case 0x26D: case 0x26E: case 0x26F:
            return 0;
        case 0x174: v->msr_sysenter_cs = val & 0xFFFFFFFFULL; return 0;
        case 0x175: v->msr_sysenter_esp = val; return 0;
        case 0x176: v->msr_sysenter_eip = val; return 0;
        case 0xC0000080:
            v->sregs.efer = val & 0xD01ULL;
            v->pending = VFL_P_NONE;
            return 0;
        case 0xC0000081: v->msr_star = val; return 0;
        case 0xC0000082: v->msr_lstar = val; return 0;
        case 0xC0000083: v->msr_cstar = val; return 0;
        case 0xC0000084: v->msr_sfmask = val & 0xFFFFFFFFULL; return 0;
        case 0xC0000100: v->msr_fs_base = val; return 0;
        case 0xC0000101: v->msr_gs_base = val; return 0;
        case 0xC0000102: v->msr_kernel_gs_base = val; return 0;
        case 0xC0000103: v->msr_tsc_aux = val & 0xFFFFFFFFULL; return 0;
        default: return -1;
    }
}

int vfl_set_msr(uint32_t vm_id, uint32_t vcpu_id, const vfl_msr_t *msr) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);

    if (!v || !msr) return -EINVAL;
    if (v->running) return -EBUSY;
    if (vfl_msr_write(vfl_get_vm(vm_id), v, msr->index, msr->value) < 0) return -EINVAL;
    __atomic_add_fetch(&vfl_get_vm(vm_id)->tlb_epoch, 1, __ATOMIC_RELEASE);
    return 0;
}

int vfl_get_msr(uint32_t vm_id, uint32_t vcpu_id, vfl_msr_t *msr) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);
    uint64_t val = 0;

    if (!v || !msr) return -EINVAL;
    if (v->running) return -EBUSY;
    if (vfl_msr_read(v, msr->index, &val) < 0) return -EINVAL;
    msr->value = val;
    return 0;
}

int vfl_set_memory(uint32_t vm_id, const vfl_memory_region_t *region) {
    vfl_vm_t *vm;
    vfl_mem_slot_t *slot;
    vfl_mem_slot_t *o;
    uint64_t end;

    vm = vfl_get_vm(vm_id);
    if (!vm || !region) return -EINVAL;
    if (vfl_vm_running(vm)) return -EBUSY;
    if (region->flags & ~(uint32_t)(VFL_MEM_FLAG_RWX | VFL_MEM_FLAG_USER))
        return -EINVAL;
    if (region->size != 0 && region->guest_phys + region->size < region->guest_phys)
        return -EINVAL;

    slot = vfl_find_slot_num(vm, region->slot);
    __atomic_add_fetch(&vm->tlb_epoch, 1, __ATOMIC_RELEASE);

    if (region->size == 0) {
        vfl_mem_slot_t **link;

        if (!slot) return 0;
        for (link = &vm->mem_slots; *link; link = &(*link)->next) {
            if (*link == slot) {
                *link = slot->next;
                break;
            }
        }
        vfl_free_slot(slot);
        if (vm->nr_mem_slots > 0) vm->nr_mem_slots--;
        return 0;
    }

    end = region->guest_phys + region->size;
    for (o = vm->mem_slots; o; o = o->next) {
        uint64_t oend;

        if (o == slot) continue;
        oend = o->guest_phys + o->size;
        if (region->guest_phys < oend && end > o->guest_phys)
            return -EINVAL;
    }

    if (!slot) {
        slot = (vfl_mem_slot_t *)vfl_pages_alloc(
            VFL_ROUND_PAGES(sizeof(vfl_mem_slot_t)));
        if (!slot) return -ENOSPC;
        slot->slot = region->slot;
        slot->next = vm->mem_slots;
        vm->mem_slots = slot;
        vm->nr_mem_slots++;
    }

    if ((region->flags & VFL_MEM_FLAG_USER) && region->host_virt) {
        uint8_t probe;
        uint64_t last = region->host_virt + region->size - 1;

        if (last < region->host_virt) return -EINVAL;
        if (copy_from_user(&probe, (const void *)(uintptr_t)region->host_virt, 1) < 0)
            return -EFAULT;
        if (copy_from_user(&probe, (const void *)(uintptr_t)last, 1) < 0)
            return -EFAULT;
        if (slot->owned) {
            if (slot->ram_phys)
                vfl_free_slot_pages(slot);
            else
                kfree(slot->host_mem);
        }
        if (slot->size != region->size)
            vfl_free_slot_dirty(slot);
        slot->owned = 0;
        slot->ram_phys = 0;
        slot->guest_phys = region->guest_phys;
        slot->size = region->size;
        slot->host_mem = (uint8_t *)(uintptr_t)region->host_virt;
    } else {
        uint64_t new_phys = 0;
        uint8_t *new_host = NULL;

        if (vfl_alloc_slot_pages(region->size, &new_phys, &new_host) < 0) {
            new_host = (uint8_t *)kmalloc((size_t)region->size);
            if (!new_host) return -ENOMEM;
            memset(new_host, 0, (size_t)region->size);
        }
        if (region->host_virt) {
            if (copy_from_user(new_host,
                               (const void *)(uintptr_t)region->host_virt,
                               (size_t)region->size) < 0) {
                if (new_phys)
                    pfa_free_contiguous(new_phys,
                                        (region->size + 0xFFFULL) >> 12);
                else
                    kfree(new_host);
                return -EFAULT;
            }
        }
        if (slot->size != region->size)
            vfl_free_slot_dirty(slot);
        if (slot->owned) {
            if (slot->ram_phys)
                vfl_free_slot_pages(slot);
            else
                kfree(slot->host_mem);
        }
        slot->owned = 1;
        slot->ram_phys = new_phys;
        slot->host_mem = new_host;
        slot->guest_phys = region->guest_phys;
        slot->size = region->size;
    }
    vfl_code_free(slot);
    slot->flags = region->flags;
    if (slot->flags == 0)
        slot->flags = VFL_MEM_FLAG_RWX;

    __atomic_add_fetch(&vm->tlb_epoch, 1, __ATOMIC_RELEASE);
    return 0;
}

int vfl_get_memory(uint32_t vm_id, vfl_memory_region_t *region) {
    vfl_vm_t *vm;
    vfl_mem_slot_t *slot;

    vm = vfl_get_vm(vm_id);
    if (!vm || !region) return -EINVAL;
    slot = vfl_find_slot_num(vm, region->slot);
    if (!slot) return -ENOENT;
    region->flags = slot->flags;
    region->guest_phys = slot->guest_phys;
    region->size = slot->size;
    region->host_virt = 0;
    return 0;
}

int vfl_get_dirty(uint32_t vm_id, vfl_dirty_t *dirty) {
    vfl_vm_t *vm;
    vfl_mem_slot_t *slot;
    uint64_t *words;
    uint64_t i;
    uint64_t need;

    if (!dirty) return -EINVAL;
    vm = vfl_get_vm(vm_id);
    if (!vm) return -EINVAL;
    slot = vfl_find_slot_num(vm, dirty->slot);
    if (!slot) return -ENOENT;
    need = (slot->dirty_pages + 63) >> 6;
    if (!slot->dirty) need = 0;
    dirty->copied = need;
    if (dirty->capacity < need || !dirty->bitmap) return -EINVAL;
    if (need == 0) return 0;
    words = (uint64_t *)vfl_pages_alloc(VFL_ROUND_PAGES(need * 8));
    if (!words) return -ENOSPC;
    for (i = 0; i < need; i++) {
        uint64_t w = 0;
        uint64_t k;

        for (k = 0; k < 64; k++) {
            uint64_t bit = i * 64 + k;

            if (bit < slot->dirty_pages && (slot->dirty[bit >> 3] & (1 << (bit & 7))))
                w |= 1ULL << k;
        }
        words[i] = w;
    }
    memset(slot->dirty, 0, (size_t)((slot->dirty_pages + 7) >> 3));
    if (copy_to_user((void *)(uintptr_t)dirty->bitmap, words, (size_t)need * 8) < 0) {
        vfl_pages_free(words, VFL_ROUND_PAGES(need * 8));
        return -EFAULT;
    }
    vfl_pages_free(words, VFL_ROUND_PAGES(need * 8));
    return 0;
}

static uint32_t vfl_brand_word(int i) {
    static const char s[] = "Lebirun VFL Virtual CPU";
    uint32_t w = 0x20202020u;
    int k;

    for (k = 0; k < 4; k++) {
        int c = i * 4 + k;
        uint8_t ch = c < (int)sizeof(s) - 1 ? (uint8_t)s[c] : 0x20;

        w = (w & ~(0xFFu << (k * 8))) | ((uint32_t)ch << (k * 8));
    }
    return w;
}

void vfl_cpuid(vfl_vcpu_t *v, uint32_t leaf, uint32_t sub, uint32_t *a,
               uint32_t *b, uint32_t *c, uint32_t *d) {
    uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;

    switch (leaf) {
        case 0:
            r0 = 0xD; r1 = 0x756E6547; r3 = 0x49656E69; r2 = 0x6C65746E;
            break;
        case 1:
            r0 = 0x00000633U; r1 = (v->apic_id << 24);
            r3 = (1U << 0) | (1U << 4) | (1U << 5) | (1U << 6) |
                 (1U << 8) | (1U << 11) | (1U << 15) |
                 (1U << 23) | (1U << 24) | (1U << 25) | (1U << 26);
            r2 = (1U << 0) | (1U << 9) | (1U << 19) | (1U << 20) | (1U << 21) |
                 (1U << 23) | (1U << 24) | (1U << 28) | (1U << 30);
            break;
        case 2:
            r0 = 1; r1 = 0x76036301U; r2 = 0; r3 = 0x00F0B5FFU;
            break;
        case 7:
            if (sub == 0) {
                r0 = 0; r1 = (1U << 0) | (1U << 3) | (1U << 8);
                r2 = 0; r3 = 0;
            }
            break;
        case 0xB:
            if (sub == 0) {
                r0 = 0; r1 = 1; r2 = 0x100;
                r3 = v->apic_id;
            } else if (sub == 1) {
                r0 = 0; r1 = 0; r2 = 0x201;
                r3 = v->apic_id;
            }
            break;
        case 0xD:
            if (sub == 0) {
                r0 = 0x440; r1 = 0x440; r2 = 0x440; r3 = 0;
            } else if (sub == 1) {
                r0 = 0xF; r1 = 0; r2 = 0; r3 = 0;
            }
            break;
        case 0x40000000:
            r0 = 0x40000001U; r1 = 0x4C465600U; r2 = 0x204C4656U; r3 = 0x20202020U;
            break;
        case 0x40000001:
            r0 = 0; r1 = 0; r2 = 0; r3 = 0;
            break;
        case 0x80000000:
            r0 = 0x80000008U; r1 = 0; r2 = 0; r3 = 0;
            break;
        case 0x80000001:
            r1 = 0; r2 = (1U << 5) | (1U << 27);
            r3 = (1U << 20) | (1U << 22) | (1U << 27) | (1U << 29);
            break;
        case 0x80000002: case 0x80000003: case 0x80000004: {
            int b4 = (int)(leaf - 0x80000002) * 4;

            r0 = vfl_brand_word(b4); r1 = vfl_brand_word(b4 + 1);
            r2 = vfl_brand_word(b4 + 2); r3 = vfl_brand_word(b4 + 3);
            break;
        }
        case 0x80000007:
            r0 = 0; r1 = 0; r2 = 0; r3 = (1U << 8);
            break;
        case 0x80000008:
            r0 = 0x3028; r1 = 0; r2 = 0; r3 = 0;
            break;
        case 0x8000001E:
            r0 = v->apic_id; r1 = 0; r2 = 0; r3 = 0;
            break;
        default:
            break;
    }
    if (leaf != 0 && leaf != 1 && leaf != 7 && leaf != 0xB && leaf != 0xD &&
        leaf != 0x40000000 && leaf != 0x40000001 && leaf != 0x80000000 &&
        leaf != 0x80000001 && (leaf < 0x80000002 || leaf > 0x80000004) &&
        leaf != 0x80000007 && leaf != 0x80000008 && leaf != 0x8000001E &&
        leaf != 2) {
        r0 = 0; r1 = 0; r2 = 0; r3 = 0;
    }
    *a = r0; *b = r1; *c = r2; *d = r3;
}

int vfl_mov_from_cr(vfl_vm_t *vm, vfl_vcpu_t *v, int cr, uint64_t *val) {
    (void)vm;
    switch (cr) {
        case 0: *val = v->sregs.cr0; return 0;
        case 2: *val = v->sregs.cr2; return 0;
        case 3: *val = v->sregs.cr3; return 0;
        case 4: *val = v->sregs.cr4; return 0;
        case 8: *val = v->sregs.cr8 & 0xFULL; return 0;
        default: return -1;
    }
}

int vfl_mov_to_cr(vfl_vm_t *vm, vfl_vcpu_t *v, int cr, uint64_t val) {
    switch (cr) {
        case 0:
            if (val & ~0xE005003FULL) return -1;
            if ((val & 1ULL) && !(v->sregs.cr0 & 1ULL))
                v->pending = VFL_P_NONE;
            v->sregs.cr0 = val | 0x10ULL;
            __atomic_add_fetch(&vm->tlb_epoch, 1, __ATOMIC_RELEASE);
            return 0;
        case 2:
            v->sregs.cr2 = val;
            return 0;
        case 3:
            v->sregs.cr3 = val & 0x000FFFFFFFFFF000ULL;
            __atomic_add_fetch(&vm->tlb_epoch, 1, __ATOMIC_RELEASE);
            return 0;
        case 4:
            if (val & ~0x2F6E7FULL) return -1;
            v->sregs.cr4 = val;
            __atomic_add_fetch(&vm->tlb_epoch, 1, __ATOMIC_RELEASE);
            return 0;
        case 8:
            if (val > 15) return -1;
            v->sregs.cr8 = val;
            v->apic_tpr = (uint32_t)(val << 4);
            return 0;
        default:
            return -1;
    }
}

int vfl_vm_running(vfl_vm_t *vm) {
    vfl_vcpu_t *v;

    for (v = vm->vcpus; v; v = v->next)
        if (v->running)
            return 1;
    return 0;
}

int vfl_set_lapic(uint32_t vm_id, uint32_t vcpu_id, const vfl_lapic_t *lapic) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);

    if (!v || !lapic) return -EINVAL;
    if (v->running) return -EBUSY;
    v->apic_id = lapic->id & 0xFFU;
    v->apic_tpr = lapic->tpr & 0xFFU;
    v->sregs.cr8 = (v->apic_tpr >> 4) & 0xFULL;
    v->apic_spurious = lapic->spurious;
    v->apic_enabled = lapic->enabled ? 1 : 0;
    v->apic_icr_lo = lapic->icr_lo;
    v->apic_icr_hi = lapic->icr_hi;
    v->msr_apic_base = (lapic->base & 0xFFFFFFFFFFFFF000ULL) |
                       (v->msr_apic_base & 0x900ULL);
    if (!v->apic_enabled)
        v->msr_apic_base &= ~0x800ULL;
    else
        v->msr_apic_base |= 0x800ULL;
    return 0;
}

int vfl_get_lapic(uint32_t vm_id, uint32_t vcpu_id, vfl_lapic_t *lapic) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);

    if (!v || !lapic) return -EINVAL;
    if (v->running) return -EBUSY;
    lapic->id = v->apic_id;
    lapic->tpr = v->apic_tpr;
    lapic->eoi = 0;
    lapic->spurious = v->apic_spurious;
    lapic->icr_lo = v->apic_icr_lo;
    lapic->icr_hi = v->apic_icr_hi;
    lapic->base = v->msr_apic_base & 0xFFFFFFFFFFFFF000ULL;
    lapic->enabled = v->apic_enabled;
    return 0;
}

static int vfl_valid_state(const vfl_state_t *state) {
    int size;

    if (state->mmio_cache_len > sizeof(state->mmio_cache_data) ||
        state->pending_len > sizeof(state->mmio_cache_data)) return 0;
    if (!state->pending) return 1;
    if (state->pending_adv > 15 ||
        (state->pending != VFL_P_MMIO_FILL && state->pending_mask != 0xFFFFULL &&
         state->pending_mask != 0xFFFFFFFFULL && state->pending_mask != ~0ULL)) return 0;
    size = state->pending_size;
    switch (state->pending) {
        case VFL_P_IO_IN_REG:
        case VFL_P_IO_IN_MEM:
            return size == 1 || size == 2 || size == 4;
        case VFL_P_MMIO_REG:
        case VFL_P_MMIO_RMW:
        case VFL_P_MMIO_JMP:
        case VFL_P_MMIO_CALL:
        case VFL_P_MMIO_PUSH:
            return (size == 1 || size == 2 || size == 4 || size == 8) &&
                   state->pending_op <= 14;
        case VFL_P_MMIO_XMM:
            return size == 4 || size == 8 || size == 16 || size == 32;
        case VFL_P_MMIO_FILL:
            return size > 0 && size <= (int)sizeof(state->mmio_cache_data);
        case VFL_P_MMIO_STR:
            return (size == 1 || size == 2 || size == 4 || size == 8) &&
                   state->pending_op <= 2 && (state->pending_port == 2 ||
                   state->pending_port == 4 || state->pending_port == 8);
        default:
            return 0;
    }
}

static int vfl_state_ioctl(unsigned long request, void *arg) {
    uint32_t ids[2];
    vfl_vm_t *vm;
    vfl_vcpu_t *v;
    vfl_state_t *state;
    uint64_t now;
    int r;

    if (!arg || copy_from_user(ids, arg, sizeof(ids)) < 0) return -EFAULT;
    state = kmalloc(sizeof(*state));
    if (!state) return -ENOMEM;
    if (request == VFL_IOCTL_SET_STATE &&
        copy_from_user(state, (uint8_t *)arg + 8, sizeof(*state)) < 0) {
        kfree(state);
        return -EFAULT;
    }
    mutex_lock(&vfl_lock);
    vm = vfl_get_vm(ids[0]);
    v = vfl_get_vcpu(ids[0], ids[1]);
    r = 0;
    if (!v) r = -EINVAL;
    else if (vfl_vm_running(vm)) r = -EBUSY;
    else {
        now = pit_get_uptime_us();
        if (request == VFL_IOCTL_GET_STATE) {
            *state = v->state;
            state->tmr_remaining_us = v->tmr_armed && v->tmr_fire_us > now ?
                                      v->tmr_fire_us - now : 0;
            state->tsc_remaining_us = v->tsc_armed && v->tsc_fire_us > now ?
                                      v->tsc_fire_us - now : 0;
        } else if (!vfl_valid_state(state) || state->tmr_remaining_us > ~0ULL - now ||
                   state->tsc_remaining_us > ~0ULL - now) {
            r = -EINVAL;
        } else {
            v->state = *state;
            v->regs.rflags |= 2;
            v->fpu.fcw |= 0x40;
            v->sregs.cr8 &= 15;
            v->apic_tpr = (uint32_t)(v->sregs.cr8 << 4);
            v->tmr_fire_us = now + state->tmr_remaining_us;
            v->tsc_fire_us = now + state->tsc_remaining_us;
            v->lazy_op = 0;
            v->lazy_enabled = 0;
            __atomic_store_n(&v->stop, 0, __ATOMIC_RELEASE);
            __atomic_add_fetch(&vm->tlb_epoch, 1, __ATOMIC_RELEASE);
        }
    }
    mutex_unlock(&vfl_lock);
    if (!r && request == VFL_IOCTL_GET_STATE &&
        copy_to_user((uint8_t *)arg + 8, state, sizeof(*state)) < 0) r = -EFAULT;
    kfree(state);
    return r;
}

static int vfl_control_ioctl(unsigned long request, void *arg) {
    uint32_t ids[2];
    vfl_vcpu_t *v;
    vfl_control_t control;
    vfl_stats_t stats;
    vfl_stats_t *fresh;
    uint64_t *breakpoints;
    uint64_t bytes;
    int r;

    if (!arg || copy_from_user(ids, arg, sizeof(ids)) < 0) return -EFAULT;
    breakpoints = NULL;
    if (request == VFL_IOCTL_SET_CONTROL) {
        if (copy_from_user(&control, (uint8_t *)arg + 8, sizeof(control)) < 0)
            return -EFAULT;
        if (control.reserved || (control.flags & ~(VFL_CONTROL_STATS | VFL_CONTROL_SINGLE_STEP |
            VFL_CONTROL_BREAKPOINT | VFL_CONTROL_TIMESLICE))) return -EINVAL;
        if (!(control.flags & VFL_CONTROL_BREAKPOINT)) control.breakpoint_count = 0;
        if (control.breakpoint_count > ~0ULL / sizeof(*breakpoints)) return -EINVAL;
        bytes = control.breakpoint_count * sizeof(*breakpoints);
        if (bytes) {
            breakpoints = kmalloc(bytes);
            if (!breakpoints) return -ENOMEM;
            if (copy_from_user(breakpoints, (void *)(uintptr_t)control.breakpoints, bytes) < 0) {
                kfree(breakpoints);
                return -EFAULT;
            }
        }
    }
    mutex_lock(&vfl_lock);
    v = vfl_get_vcpu(ids[0], ids[1]);
    r = 0;
    if (!v) r = -EINVAL;
    else if (request == VFL_IOCTL_INTERRUPT)
        __atomic_store_n(&v->stop, 1, __ATOMIC_RELEASE);
    else if (v->running) r = -EBUSY;
    else if (request == VFL_IOCTL_SET_CONTROL) {
        if ((control.flags & VFL_CONTROL_STATS) && !v->stats) {
            fresh = kmalloc(sizeof(*fresh));
            if (!fresh) r = -ENOMEM;
            else {
                memset(fresh, 0, sizeof(*fresh));
                v->stats = fresh;
            }
        }
        if (!r) {
            if (!(control.flags & VFL_CONTROL_STATS) && v->stats) {
                kfree(v->stats);
                v->stats = NULL;
            }
            if (v->breakpoints) kfree(v->breakpoints);
            v->breakpoints = breakpoints;
            breakpoints = NULL;
            v->control = control;
        }
    } else if (request == VFL_IOCTL_GET_CONTROL) control = v->control;
    else if (request == VFL_IOCTL_GET_STATS) {
        memset(&stats, 0, sizeof(stats));
        if (v->stats) stats = *v->stats;
    } else if (request == VFL_IOCTL_RESET_STATS && v->stats)
        memset(v->stats, 0, sizeof(*v->stats));
    mutex_unlock(&vfl_lock);
    if (breakpoints) kfree(breakpoints);
    if (!r && request == VFL_IOCTL_GET_CONTROL &&
        copy_to_user((uint8_t *)arg + 8, &control, sizeof(control)) < 0) r = -EFAULT;
    if (!r && request == VFL_IOCTL_GET_STATS &&
        copy_to_user((uint8_t *)arg + 8, &stats, sizeof(stats)) < 0) r = -EFAULT;
    return r;
}

static int vfl_memory_ioctl(unsigned long request, void *arg) {
    uint32_t vm_id;
    vfl_memory_transfer_t transfer;
    vfl_vm_t *vm;
    vfl_mem_slot_t *slot;
    uint8_t *host;
    uint64_t offset;
    uint64_t chunk;
    int r;

    if (!arg || copy_from_user(&vm_id, arg, sizeof(vm_id)) < 0 ||
        copy_from_user(&transfer, (uint8_t *)arg + 4, sizeof(transfer)) < 0)
        return -EFAULT;
    if (transfer.guest_phys + transfer.size < transfer.guest_phys ||
        transfer.buffer + transfer.size < transfer.buffer) return -EINVAL;
    mutex_lock(&vfl_lock);
    vm = vfl_get_vm(vm_id);
    r = 0;
    if (!vm) r = -EINVAL;
    else if (vfl_vm_running(vm)) r = -EBUSY;
    else {
        offset = 0;
        while (offset < transfer.size) {
            slot = vfl_find_slot(vm, transfer.guest_phys + offset, 1);
            if (!slot) {
                r = -ENOENT;
                break;
            }
            chunk = slot->size - (transfer.guest_phys + offset - slot->guest_phys);
            if (chunk > transfer.size - offset) chunk = transfer.size - offset;
            host = slot->host_mem + (transfer.guest_phys + offset - slot->guest_phys);
            if (request == VFL_IOCTL_READ_MEMORY)
                r = copy_to_user((void *)(uintptr_t)(transfer.buffer + offset), host, chunk);
            else {
                r = copy_from_user(host, (void *)(uintptr_t)(transfer.buffer + offset), chunk);
                vfl_mark_dirty(vm, transfer.guest_phys + offset, chunk);
            }
            if (r < 0) {
                r = -EFAULT;
                break;
            }
            offset += chunk;
        }
    }
    mutex_unlock(&vfl_lock);
    return r;
}

static int vfl_ioctl(vfs_node_t *node, unsigned long request, void *arg) {
    uint32_t ids[2];
    uint32_t vm_id;
    vfl_regs_t regs;
    vfl_sregs_t sregs;
    vfl_fpu_t fpu;
    vfl_irq_t irq;
    vfl_msr_t msr;
    vfl_dirty_t dirty;
    vfl_memory_region_t region;
    vfl_run_t run;
    vfl_vm_info_t info;
    vfl_lapic_t lapic;
    int r;

    (void)node;

    if (!vfl_lock_ready) return -ENODEV;

    switch (request) {
        case VFL_IOCTL_SET_CONTROL:
        case VFL_IOCTL_GET_CONTROL:
        case VFL_IOCTL_GET_STATS:
        case VFL_IOCTL_RESET_STATS:
        case VFL_IOCTL_INTERRUPT:
            return vfl_control_ioctl(request, arg);
        case VFL_IOCTL_GET_STATE:
        case VFL_IOCTL_SET_STATE:
            return vfl_state_ioctl(request, arg);
        case VFL_IOCTL_READ_MEMORY:
        case VFL_IOCTL_WRITE_MEMORY:
            return vfl_memory_ioctl(request, arg);
        case VFL_IOCTL_CREATE_VM:
            mutex_lock(&vfl_lock);
            r = vfl_create_vm();
            mutex_unlock(&vfl_lock);
            return r;

        case VFL_IOCTL_DESTROY_VM:
            if (!arg) return -EINVAL;
            if (copy_from_user(&vm_id, arg, sizeof(vm_id)) < 0) return -EFAULT;
            mutex_lock(&vfl_lock);
            r = vfl_destroy_vm(vm_id);
            mutex_unlock(&vfl_lock);
            return r;

        case VFL_IOCTL_CREATE_VCPU:
            if (!arg) return -EINVAL;
            if (copy_from_user(&vm_id, arg, sizeof(vm_id)) < 0) return -EFAULT;
            mutex_lock(&vfl_lock);
            r = vfl_create_vcpu(vm_id);
            mutex_unlock(&vfl_lock);
            return r;

        case VFL_IOCTL_RESET_VCPU:
            if (!arg) return -EINVAL;
            if (copy_from_user(ids, arg, sizeof(ids)) < 0) return -EFAULT;
            mutex_lock(&vfl_lock);
            r = vfl_reset_vcpu(ids[0], ids[1]);
            mutex_unlock(&vfl_lock);
            return r;

        case VFL_IOCTL_SET_REGS:
            if (!arg) return -EINVAL;
            if (copy_from_user(ids, arg, sizeof(ids)) < 0) return -EFAULT;
            if (copy_from_user(&regs, (uint8_t *)arg + 8, sizeof(regs)) < 0)
                return -EFAULT;
            mutex_lock(&vfl_lock);
            r = vfl_set_regs(ids[0], ids[1], &regs);
            mutex_unlock(&vfl_lock);
            return r;

        case VFL_IOCTL_GET_REGS:
            if (!arg) return -EINVAL;
            if (copy_from_user(ids, arg, sizeof(ids)) < 0) return -EFAULT;
            mutex_lock(&vfl_lock);
            r = vfl_get_regs(ids[0], ids[1], &regs);
            mutex_unlock(&vfl_lock);
            if (r < 0) return r;
            if (copy_to_user((uint8_t *)arg + 8, &regs, sizeof(regs)) < 0)
                return -EFAULT;
            return 0;

        case VFL_IOCTL_SET_SREGS:
            if (!arg) return -EINVAL;
            if (copy_from_user(ids, arg, sizeof(ids)) < 0) return -EFAULT;
            if (copy_from_user(&sregs, (uint8_t *)arg + 8, sizeof(sregs)) < 0)
                return -EFAULT;
            mutex_lock(&vfl_lock);
            r = vfl_set_sregs(ids[0], ids[1], &sregs);
            mutex_unlock(&vfl_lock);
            return r;

        case VFL_IOCTL_GET_SREGS:
            if (!arg) return -EINVAL;
            if (copy_from_user(ids, arg, sizeof(ids)) < 0) return -EFAULT;
            mutex_lock(&vfl_lock);
            r = vfl_get_sregs(ids[0], ids[1], &sregs);
            mutex_unlock(&vfl_lock);
            if (r < 0) return r;
            if (copy_to_user((uint8_t *)arg + 8, &sregs, sizeof(sregs)) < 0)
                return -EFAULT;
            return 0;

        case VFL_IOCTL_SET_FPU:
            if (!arg) return -EINVAL;
            if (copy_from_user(ids, arg, sizeof(ids)) < 0) return -EFAULT;
            if (copy_from_user(&fpu, (uint8_t *)arg + 8, sizeof(fpu)) < 0)
                return -EFAULT;
            mutex_lock(&vfl_lock);
            r = vfl_set_fpu(ids[0], ids[1], &fpu);
            mutex_unlock(&vfl_lock);
            return r;

        case VFL_IOCTL_GET_FPU:
            if (!arg) return -EINVAL;
            if (copy_from_user(ids, arg, sizeof(ids)) < 0) return -EFAULT;
            mutex_lock(&vfl_lock);
            r = vfl_get_fpu(ids[0], ids[1], &fpu);
            mutex_unlock(&vfl_lock);
            if (r < 0) return r;
            if (copy_to_user((uint8_t *)arg + 8, &fpu, sizeof(fpu)) < 0)
                return -EFAULT;
            return 0;

        case VFL_IOCTL_INJECT_IRQ:
            if (!arg) return -EINVAL;
            if (copy_from_user(ids, arg, sizeof(ids)) < 0) return -EFAULT;
            if (copy_from_user(&irq, (uint8_t *)arg + 8, sizeof(irq)) < 0)
                return -EFAULT;
            mutex_lock(&vfl_lock);
            r = vfl_inject_irq(ids[0], ids[1], &irq);
            mutex_unlock(&vfl_lock);
            return r;

        case VFL_IOCTL_SET_MSR:
            if (!arg) return -EINVAL;
            if (copy_from_user(ids, arg, sizeof(ids)) < 0) return -EFAULT;
            if (copy_from_user(&msr, (uint8_t *)arg + 8, sizeof(msr)) < 0)
                return -EFAULT;
            mutex_lock(&vfl_lock);
            r = vfl_set_msr(ids[0], ids[1], &msr);
            mutex_unlock(&vfl_lock);
            return r;

        case VFL_IOCTL_GET_MSR:
            if (!arg) return -EINVAL;
            if (copy_from_user(ids, arg, sizeof(ids)) < 0) return -EFAULT;
            if (copy_from_user(&msr, (uint8_t *)arg + 8, sizeof(msr)) < 0)
                return -EFAULT;
            mutex_lock(&vfl_lock);
            r = vfl_get_msr(ids[0], ids[1], &msr);
            mutex_unlock(&vfl_lock);
            if (r < 0) return r;
            if (copy_to_user((uint8_t *)arg + 8, &msr, sizeof(msr)) < 0)
                return -EFAULT;
            return 0;

        case VFL_IOCTL_SET_MEMORY:
            if (!arg) return -EINVAL;
            if (copy_from_user(&vm_id, arg, sizeof(vm_id)) < 0) return -EFAULT;
            if (copy_from_user(&region, (uint8_t *)arg + 4, sizeof(region)) < 0)
                return -EFAULT;
            mutex_lock(&vfl_lock);
            r = vfl_set_memory(vm_id, &region);
            mutex_unlock(&vfl_lock);
            return r;

        case VFL_IOCTL_GET_MEMORY:
            if (!arg) return -EINVAL;
            if (copy_from_user(&vm_id, arg, sizeof(vm_id)) < 0) return -EFAULT;
            if (copy_from_user(&region, (uint8_t *)arg + 4, sizeof(region)) < 0)
                return -EFAULT;
            mutex_lock(&vfl_lock);
            r = vfl_get_memory(vm_id, &region);
            mutex_unlock(&vfl_lock);
            if (r < 0) return r;
            if (copy_to_user((uint8_t *)arg + 4, &region, sizeof(region)) < 0)
                return -EFAULT;
            return 0;

        case VFL_IOCTL_GET_DIRTY:
            if (!arg) return -EINVAL;
            if (copy_from_user(&vm_id, arg, sizeof(vm_id)) < 0) return -EFAULT;
            if (copy_from_user(&dirty, (uint8_t *)arg + 4, sizeof(dirty)) < 0)
                return -EFAULT;
            mutex_lock(&vfl_lock);
            r = vfl_get_dirty(vm_id, &dirty);
            mutex_unlock(&vfl_lock);
            if (copy_to_user((uint8_t *)arg + 4, &dirty, sizeof(dirty)) < 0)
                return -EFAULT;
            return r;

        case VFL_IOCTL_RUN: {
            vfl_vcpu_t *vcpu;

            if (!arg) return -EINVAL;
            if (copy_from_user(ids, arg, sizeof(ids)) < 0) return -EFAULT;
            if (copy_from_user(&run, (uint8_t *)arg + 8, sizeof(run)) < 0)
                return -EFAULT;
            mutex_lock(&vfl_lock);
            vcpu = vfl_get_vcpu(ids[0], ids[1]);
            if (!vcpu || vcpu->running) {
                mutex_unlock(&vfl_lock);
                return vcpu ? -EBUSY : -EINVAL;
            }
            vcpu->running = 1;
            mutex_unlock(&vfl_lock);
            r = vfl_run(ids[0], ids[1], &run);
            mutex_lock(&vfl_lock);
            vcpu->running = 0;
            mutex_unlock(&vfl_lock);
            if (copy_to_user((uint8_t *)arg + 8, &run, sizeof(run)) < 0)
                return -EFAULT;
            return r;
        }

        case VFL_IOCTL_SET_LAPIC:
            if (!arg) return -EINVAL;
            if (copy_from_user(ids, arg, sizeof(ids)) < 0) return -EFAULT;
            if (copy_from_user(&lapic, (uint8_t *)arg + 8, sizeof(lapic)) < 0)
                return -EFAULT;
            mutex_lock(&vfl_lock);
            r = vfl_set_lapic(ids[0], ids[1], &lapic);
            mutex_unlock(&vfl_lock);
            return r;

        case VFL_IOCTL_GET_LAPIC:
            if (!arg) return -EINVAL;
            if (copy_from_user(ids, arg, sizeof(ids)) < 0) return -EFAULT;
            mutex_lock(&vfl_lock);
            r = vfl_get_lapic(ids[0], ids[1], &lapic);
            mutex_unlock(&vfl_lock);
            if (r < 0) return r;
            if (copy_to_user((uint8_t *)arg + 8, &lapic, sizeof(lapic)) < 0)
                return -EFAULT;
            return 0;

        case VFL_IOCTL_VM_INFO:
            if (!arg) return -EINVAL;
            if (copy_from_user(&vm_id, arg, sizeof(vm_id)) < 0) return -EFAULT;
            mutex_lock(&vfl_lock);
            r = vfl_vm_info(vm_id, &info);
            mutex_unlock(&vfl_lock);
            if (r < 0) return r;
            if (copy_to_user((uint8_t *)arg + 4, &info, sizeof(info)) < 0)
                return -EFAULT;
            return 0;

        default:
            return -ENOTTY;
    }
}

static uint64_t vfl_dev_read(vfs_node_t *n, uint64_t off, uint64_t sz, uint8_t *buf) {
    (void)n; (void)off; (void)sz; (void)buf;
    return 0;
}

static uint64_t vfl_dev_write(vfs_node_t *n, uint64_t off, uint64_t sz, uint8_t *buf) {
    (void)n; (void)off; (void)sz; (void)buf;
    return 0;
}

static vfs_node_t dev_vfl_node;

void KERNEL_INIT vfl_register_devfs(void) {
    memset(&dev_vfl_node, 0, sizeof(vfs_node_t));
    dev_vfl_node.flags = VFS_CHARDEVICE;
    vfs_node_set_name(&dev_vfl_node, "vfl");
    dev_vfl_node.mask = 0660;
    dev_vfl_node.uid = 0;
    dev_vfl_node.gid = 0;
    dev_vfl_node.read = vfl_dev_read;
    dev_vfl_node.write = vfl_dev_write;
    dev_vfl_node.ioctl = vfl_ioctl;
    dev_vfl_node.ref_count = 1;
    if (!vfl_lock_ready) {
        mutex_init(&vfl_lock);
        vfl_lock_ready = 1;
    }
}

vfs_node_t *vfl_get_devfs_node(void) {
    return &dev_vfl_node;
}
