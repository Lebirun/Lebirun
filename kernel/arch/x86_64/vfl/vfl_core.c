#include "vfl_priv.h"
#include <lebirun/common.h>
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
static int vfl_vm_extent;

static int vfl_resize_vms(int new_extent) {
    vfl_vm_t *new_vms;

    if (new_extent <= vfl_vm_extent) return 0;
    new_vms = (vfl_vm_t *)krealloc(vfl_vms, (size_t)new_extent * sizeof(vfl_vm_t));
    if (!new_vms) return -ENOMEM;
    memset(&new_vms[vfl_vm_extent], 0,
           (size_t)(new_extent - vfl_vm_extent) * sizeof(vfl_vm_t));
    vfl_vms = new_vms;
    vfl_vm_extent = new_extent;
    return 0;
}

static int vfl_add_vcpu(vfl_vm_t *vm) {
    vfl_vcpu_t *new_vcpus;

    if (vm->nr_vcpus < 0) return -EINVAL;
    new_vcpus = (vfl_vcpu_t *)krealloc(vm->vcpus,
        ((size_t)vm->nr_vcpus + 1) * sizeof(vfl_vcpu_t));
    if (!new_vcpus) return -ENOMEM;
    vm->vcpus = new_vcpus;
    return 0;
}

static int vfl_resize_mem_slots(vfl_vm_t *vm, int new_extent) {
    vfl_mem_slot_t *new_slots;

    if (new_extent <= vm->mem_slot_extent) return 0;
    new_slots = (vfl_mem_slot_t *)krealloc(
        vm->mem_slots, (size_t)new_extent * sizeof(vfl_mem_slot_t));
    if (!new_slots) return -ENOMEM;
    memset(&new_slots[vm->mem_slot_extent], 0,
           (size_t)(new_extent - vm->mem_slot_extent) * sizeof(vfl_mem_slot_t));
    vm->mem_slots = new_slots;
    vm->mem_slot_extent = new_extent;
    return 0;
}

static void vfl_free_slot(vfl_mem_slot_t *slot) {
    kfree(slot->host_mem);
    kfree(slot->dirty);
    memset(slot, 0, sizeof(vfl_mem_slot_t));
}

static void vfl_shrink_mem_slots(vfl_vm_t *vm) {
    int new_extent;
    vfl_mem_slot_t *new_slots;

    new_extent = vm->mem_slot_extent;
    while (new_extent > 0 && !vm->mem_slots[new_extent - 1].in_use) {
        new_extent--;
    }
    if (new_extent == vm->mem_slot_extent) return;
    if (new_extent == 0) {
        kfree(vm->mem_slots);
        vm->mem_slots = NULL;
        vm->mem_slot_extent = 0;
        return;
    }
    new_slots = (vfl_mem_slot_t *)krealloc(
        vm->mem_slots, (size_t)new_extent * sizeof(vfl_mem_slot_t));
    if (!new_slots) return;
    vm->mem_slots = new_slots;
    vm->mem_slot_extent = new_extent;
}

vfl_vm_t *vfl_get_vm(uint32_t vm_id) {
    if (!vfl_vms) return NULL;
    if (vm_id >= (uint32_t)vfl_vm_extent) return NULL;
    if (!vfl_vms[vm_id].active) return NULL;
    return &vfl_vms[vm_id];
}

vfl_vcpu_t *vfl_get_vcpu(uint32_t vm_id, uint32_t vcpu_id) {
    vfl_vm_t *vm = vfl_get_vm(vm_id);

    if (!vm) return NULL;
    if (vcpu_id >= (uint32_t)vm->nr_vcpus) return NULL;
    if (!vm->vcpus[vcpu_id].active) return NULL;
    return &vm->vcpus[vcpu_id];
}

static void vfl_init_vcpu_state(vfl_vcpu_t *v) {
    memset(v, 0, sizeof(vfl_vcpu_t));
    v->active = 1;
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
    v->fpu.fcw = 0x37F;
    v->fpu.ftw = 0xFFFF;
    v->fpu.mxcsr = 0x1F80;
}

int vfl_create_vm(void) {
    int i;

    for (i = 0; i < vfl_vm_extent; i++) {
        if (!vfl_vms[i].active) {
            memset(&vfl_vms[i], 0, sizeof(vfl_vm_t));
            vfl_vms[i].active = 1;
            vfl_vms[i].id = (uint32_t)i;
            vfl_vms[i].tlb_epoch = 1;
            return i;
        }
    }
    i = vfl_vm_extent;
    if (vfl_resize_vms(vfl_vm_extent + 1) < 0) return -ENOMEM;
    vfl_vms[i].active = 1;
    vfl_vms[i].id = (uint32_t)i;
    vfl_vms[i].tlb_epoch = 1;
    return i;
}

int vfl_destroy_vm(uint32_t vm_id) {
    int i;
    int new_extent;
    vfl_vm_t *vm;
    vfl_vm_t *new_vms;

    vm = vfl_get_vm(vm_id);
    if (!vm) return -EINVAL;

    for (i = 0; i < vm->mem_slot_extent; i++) {
        if (vm->mem_slots[i].in_use) vfl_free_slot(&vm->mem_slots[i]);
    }
    kfree(vm->vcpus);
    kfree(vm->mem_slots);
    memset(vm, 0, sizeof(vfl_vm_t));
    new_extent = vfl_vm_extent;
    while (new_extent > 0 && !vfl_vms[new_extent - 1].active) new_extent--;
    if (new_extent == 0) {
        kfree(vfl_vms);
        vfl_vms = NULL;
        vfl_vm_extent = 0;
    } else if (new_extent < vfl_vm_extent) {
        new_vms = (vfl_vm_t *)krealloc(
            vfl_vms, (size_t)new_extent * sizeof(vfl_vm_t));
        if (new_vms) {
            vfl_vms = new_vms;
            vfl_vm_extent = new_extent;
        }
    }
    return 0;
}

int vfl_create_vcpu(uint32_t vm_id) {
    vfl_vm_t *vm;
    int idx;

    vm = vfl_get_vm(vm_id);
    if (!vm) return -EINVAL;
    if (vfl_add_vcpu(vm) < 0) return -ENOMEM;

    idx = vm->nr_vcpus;
    vfl_init_vcpu_state(&vm->vcpus[idx]);
    vm->nr_vcpus++;
    return idx;
}

int vfl_reset_vcpu(uint32_t vm_id, uint32_t vcpu_id) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);

    if (!v) return -EINVAL;
    vfl_init_vcpu_state(v);
    return 0;
}

int vfl_set_regs(uint32_t vm_id, uint32_t vcpu_id, const vfl_regs_t *regs) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);

    if (!v || !regs) return -EINVAL;
    memcpy(&v->regs, regs, sizeof(vfl_regs_t));
    v->regs.rflags |= 0x2;
    v->halted = 0;
    v->pending = VFL_P_NONE;
    return 0;
}

int vfl_get_regs(uint32_t vm_id, uint32_t vcpu_id, vfl_regs_t *regs) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);

    if (!v || !regs) return -EINVAL;
    memcpy(regs, &v->regs, sizeof(vfl_regs_t));
    return 0;
}

int vfl_set_sregs(uint32_t vm_id, uint32_t vcpu_id, const vfl_sregs_t *sregs) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);

    if (!v || !sregs) return -EINVAL;
    memcpy(&v->sregs, sregs, sizeof(vfl_sregs_t));
    v->pending = VFL_P_NONE;
    vfl_get_vm(vm_id)->tlb_epoch++;
    return 0;
}

int vfl_get_sregs(uint32_t vm_id, uint32_t vcpu_id, vfl_sregs_t *sregs) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);

    if (!v || !sregs) return -EINVAL;
    memcpy(sregs, &v->sregs, sizeof(vfl_sregs_t));
    return 0;
}

int vfl_set_fpu(uint32_t vm_id, uint32_t vcpu_id, const vfl_fpu_t *fpu) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);

    if (!v || !fpu) return -EINVAL;
    memcpy(&v->fpu, fpu, sizeof(vfl_fpu_t));
    v->fpu.fcw |= 0x40;
    return 0;
}

int vfl_get_fpu(uint32_t vm_id, uint32_t vcpu_id, vfl_fpu_t *fpu) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);

    if (!v || !fpu) return -EINVAL;
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
    switch (index) {
        case 0x10: *val = v->tsc; return 0;
        case 0x1B: *val = 0xFEE00900ULL; return 0;
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

int vfl_msr_write(vfl_vcpu_t *v, uint32_t index, uint64_t val) {
    switch (index) {
        case 0x10: v->tsc = val; return 0;
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
    if (vfl_msr_write(v, msr->index, msr->value) < 0) return -EINVAL;
    vfl_get_vm(vm_id)->tlb_epoch++;
    return 0;
}

int vfl_get_msr(uint32_t vm_id, uint32_t vcpu_id, vfl_msr_t *msr) {
    vfl_vcpu_t *v = vfl_get_vcpu(vm_id, vcpu_id);
    uint64_t val = 0;

    if (!v || !msr) return -EINVAL;
    if (vfl_msr_read(v, msr->index, &val) < 0) return -EINVAL;
    msr->value = val;
    return 0;
}

int vfl_set_memory(uint32_t vm_id, const vfl_memory_region_t *region) {
    vfl_vm_t *vm;
    vfl_mem_slot_t *slot;
    uint8_t *host_buf;
    int i;
    uint64_t end;

    vm = vfl_get_vm(vm_id);
    if (!vm || !region) return -EINVAL;
    if (region->slot >= (uint32_t)INT32_MAX) return -EINVAL;
    if (region->flags & ~(uint32_t)VFL_MEM_FLAG_RWX) return -EINVAL;
    if (region->size != 0 && region->guest_phys + region->size < region->guest_phys)
        return -EINVAL;

    if (region->slot >= (uint32_t)vm->mem_slot_extent) {
        if (region->size == 0) return 0;
        if (vfl_resize_mem_slots(vm, (int)region->slot + 1) < 0) return -ENOMEM;
    }

    slot = &vm->mem_slots[region->slot];

    if (region->size == 0) {
        if (slot->in_use && vm->nr_mem_slots > 0) vm->nr_mem_slots--;
        vfl_free_slot(slot);
        vfl_shrink_mem_slots(vm);
        return 0;
    }

    end = region->guest_phys + region->size;
    for (i = 0; i < vm->mem_slot_extent; i++) {
        vfl_mem_slot_t *o;
        uint64_t oend;

        if (i == (int)region->slot) continue;
        o = &vm->mem_slots[i];
        if (!o->in_use) continue;
        oend = o->guest_phys + o->size;
        if (region->guest_phys < oend && end > o->guest_phys)
            return -EINVAL;
    }

    host_buf = (uint8_t *)kmalloc((size_t)region->size);
    if (!host_buf) return -ENOMEM;
    memset(host_buf, 0, (size_t)region->size);

    if (region->host_virt) {
        if (copy_from_user(host_buf, (const void *)(uintptr_t)region->host_virt,
                           (size_t)region->size) < 0) {
            kfree(host_buf);
            return -EFAULT;
        }
    }

    if (slot->in_use && vm->nr_mem_slots > 0 && slot->size != region->size) {
        kfree(slot->dirty);
        slot->dirty = NULL;
        slot->dirty_pages = 0;
    }
    kfree(slot->host_mem);
    if (!slot->in_use) vm->nr_mem_slots++;
    slot->in_use = 1;
    slot->guest_phys = region->guest_phys;
    slot->size = region->size;
    slot->host_mem = host_buf;
    slot->flags = region->flags;
    if (slot->flags == 0)
        slot->flags = VFL_MEM_FLAG_RWX;

    vm->tlb_epoch++;
    return 0;
}

int vfl_get_memory(uint32_t vm_id, vfl_memory_region_t *region) {
    vfl_vm_t *vm;
    vfl_mem_slot_t *slot;

    vm = vfl_get_vm(vm_id);
    if (!vm || !region) return -EINVAL;
    if (region->slot >= (uint32_t)vm->mem_slot_extent) return -ENOENT;
    slot = &vm->mem_slots[region->slot];
    if (!slot->in_use) return -ENOENT;
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
    if (dirty->slot >= (uint32_t)vm->mem_slot_extent) return -ENOENT;
    slot = &vm->mem_slots[dirty->slot];
    if (!slot->in_use) return -ENOENT;
    need = (slot->dirty_pages + 63) >> 6;
    if (!slot->dirty) need = 0;
    dirty->copied = need;
    if (dirty->capacity < need || !dirty->bitmap) return -EINVAL;
    if (need == 0) return 0;
    words = (uint64_t *)kmalloc((size_t)need * 8);
    if (!words) return -ENOMEM;
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
        kfree(words);
        return -EFAULT;
    }
    kfree(words);
    return 0;
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
    uint32_t value;
    int r;

    (void)node;

    if (!vfl_lock_ready) return -ENODEV;

    switch (request) {
        case VFL_IOCTL_GET_API_VERSION:
            if (!arg) return -EINVAL;
            value = VFL_API_VERSION;
            if (copy_to_user(arg, &value, sizeof(value)) < 0) return -EFAULT;
            return 0;

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

        case VFL_IOCTL_RUN:
            if (!arg) return -EINVAL;
            if (copy_from_user(ids, arg, sizeof(ids)) < 0) return -EFAULT;
            if (copy_from_user(&run, (uint8_t *)arg + 8, sizeof(run)) < 0)
                return -EFAULT;
            mutex_lock(&vfl_lock);
            r = vfl_run(ids[0], ids[1], &run);
            mutex_unlock(&vfl_lock);
            if (copy_to_user((uint8_t *)arg + 8, &run, sizeof(run)) < 0)
                return -EFAULT;
            return r;

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
