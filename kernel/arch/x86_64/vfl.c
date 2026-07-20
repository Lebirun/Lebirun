#include <lebirun/vfl.h>
#include <lebirun/vfs.h>
#include <lebirun/mem_map.h>
#include <lebirun/task.h>
#include <lebirun/tty.h>
#include <string.h>
#include <stdio.h>

extern void *kmalloc(size_t);
extern void kfree(void *);

static vfl_vm_t *vfl_vms;
static int vfl_vm_extent;

static int vfl_resize_vms(int new_extent) {
    vfl_vm_t *new_vms;

    if (new_extent <= vfl_vm_extent) return 0;
    new_vms = (vfl_vm_t *)krealloc(vfl_vms, new_extent * sizeof(vfl_vm_t));
    if (!new_vms) return -1;
    memset(&new_vms[vfl_vm_extent], 0,
           (new_extent - vfl_vm_extent) * sizeof(vfl_vm_t));
    vfl_vms = new_vms;
    vfl_vm_extent = new_extent;
    return 0;
}

static int vfl_add_vcpu(vfl_vm_t *vm) {
    vfl_vcpu_t *new_vcpus;

    new_vcpus = (vfl_vcpu_t *)krealloc(vm->vcpus,
        (vm->nr_vcpus + 1) * sizeof(vfl_vcpu_t));
    if (!new_vcpus) return -1;
    vm->vcpus = new_vcpus;
    return 0;
}

static int vfl_resize_mem_slots(vfl_vm_t *vm, int new_extent) {
    vfl_mem_slot_t *new_slots;

    if (new_extent <= vm->mem_slot_extent) return 0;
    new_slots = (vfl_mem_slot_t *)krealloc(
        vm->mem_slots, new_extent * sizeof(vfl_mem_slot_t));
    if (!new_slots) return -1;
    memset(&new_slots[vm->mem_slot_extent], 0,
           (new_extent - vm->mem_slot_extent) * sizeof(vfl_mem_slot_t));
    vm->mem_slots = new_slots;
    vm->mem_slot_extent = new_extent;
    return 0;
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
        vm->mem_slots, new_extent * sizeof(vfl_mem_slot_t));
    if (!new_slots) return;
    vm->mem_slots = new_slots;
    vm->mem_slot_extent = new_extent;
}

int vfl_create_vm(void) {
    int i;

    for (i = 0; i < vfl_vm_extent; i++) {
        if (!vfl_vms[i].active) {
            memset(&vfl_vms[i], 0, sizeof(vfl_vm_t));
            vfl_vms[i].active = 1;
            vfl_vms[i].id = (uint32_t)i;
            return i;
        }
    }
    i = vfl_vm_extent;
    if (vfl_resize_vms(vfl_vm_extent + 1) < 0) return -1;
    vfl_vms[i].active = 1;
    vfl_vms[i].id = (uint32_t)i;
    return i;
}

int vfl_destroy_vm(uint32_t vm_id) {
    int i;
    int new_extent;
    vfl_vm_t *vm;
    vfl_vm_t *new_vms;

    if (!vfl_vms) return -1;
    if (vm_id >= (uint32_t)vfl_vm_extent) return -1;
    vm = &vfl_vms[vm_id];
    if (!vm->active) return -1;

    for (i = 0; i < vm->mem_slot_extent; i++) {
        if (vm->mem_slots[i].in_use && vm->mem_slots[i].host_mem) {
            kfree(vm->mem_slots[i].host_mem);
        }
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
            vfl_vms, new_extent * sizeof(vfl_vm_t));
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

    if (!vfl_vms) return -1;
    if (vm_id >= (uint32_t)vfl_vm_extent) return -1;
    vm = &vfl_vms[vm_id];
    if (!vm->active) return -1;
    if (vfl_add_vcpu(vm) < 0) return -1;

    idx = vm->nr_vcpus;
    memset(&vm->vcpus[idx], 0, sizeof(vfl_vcpu_t));
    vm->vcpus[idx].active = 1;
    vm->vcpus[idx].regs.rflags = 0x2;
    vm->vcpus[idx].sregs.cs.selector = 0;
    vm->vcpus[idx].sregs.cs.base = 0;
    vm->vcpus[idx].sregs.cs.limit = 0xFFFFFFFF;
    vm->vcpus[idx].sregs.cs.attrib = 0x009B;
    vm->vcpus[idx].sregs.ds.limit = 0xFFFFFFFF;
    vm->vcpus[idx].sregs.ds.attrib = 0x0093;
    vm->vcpus[idx].sregs.es = vm->vcpus[idx].sregs.ds;
    vm->vcpus[idx].sregs.ss = vm->vcpus[idx].sregs.ds;
    vm->vcpus[idx].sregs.cr0 = 0x10;
    vm->nr_vcpus++;
    return idx;
}

int vfl_set_regs(uint32_t vm_id, uint32_t vcpu_id, const vfl_regs_t *regs) {
    vfl_vm_t *vm;

    if (!vfl_vms) return -1;
    if (vm_id >= (uint32_t)vfl_vm_extent) return -1;
    vm = &vfl_vms[vm_id];
    if (!vm->active) return -1;
    if (vcpu_id >= (uint32_t)vm->nr_vcpus) return -1;
    if (!vm->vcpus[vcpu_id].active) return -1;

    memcpy(&vm->vcpus[vcpu_id].regs, regs, sizeof(vfl_regs_t));
    return 0;
}

int vfl_get_regs(uint32_t vm_id, uint32_t vcpu_id, vfl_regs_t *regs) {
    vfl_vm_t *vm;

    if (!vfl_vms) return -1;
    if (vm_id >= (uint32_t)vfl_vm_extent) return -1;
    vm = &vfl_vms[vm_id];
    if (!vm->active) return -1;
    if (vcpu_id >= (uint32_t)vm->nr_vcpus) return -1;
    if (!vm->vcpus[vcpu_id].active) return -1;

    memcpy(regs, &vm->vcpus[vcpu_id].regs, sizeof(vfl_regs_t));
    return 0;
}

int vfl_set_sregs(uint32_t vm_id, uint32_t vcpu_id, const vfl_sregs_t *sregs) {
    vfl_vm_t *vm;

    if (!vfl_vms) return -1;
    if (vm_id >= (uint32_t)vfl_vm_extent) return -1;
    vm = &vfl_vms[vm_id];
    if (!vm->active) return -1;
    if (vcpu_id >= (uint32_t)vm->nr_vcpus) return -1;
    if (!vm->vcpus[vcpu_id].active) return -1;

    memcpy(&vm->vcpus[vcpu_id].sregs, sregs, sizeof(vfl_sregs_t));
    return 0;
}

int vfl_get_sregs(uint32_t vm_id, uint32_t vcpu_id, vfl_sregs_t *sregs) {
    vfl_vm_t *vm;

    if (!vfl_vms) return -1;
    if (vm_id >= (uint32_t)vfl_vm_extent) return -1;
    vm = &vfl_vms[vm_id];
    if (!vm->active) return -1;
    if (vcpu_id >= (uint32_t)vm->nr_vcpus) return -1;
    if (!vm->vcpus[vcpu_id].active) return -1;

    memcpy(sregs, &vm->vcpus[vcpu_id].sregs, sizeof(vfl_sregs_t));
    return 0;
}

int vfl_set_memory(uint32_t vm_id, const vfl_memory_region_t *region) {
    vfl_vm_t *vm;
    vfl_mem_slot_t *slot;
    uint8_t *host_buf;

    if (!vfl_vms) return -1;
    if (vm_id >= (uint32_t)vfl_vm_extent) return -1;
    vm = &vfl_vms[vm_id];
    if (!vm->active) return -1;
    if (region->slot >= (uint32_t)INT32_MAX) return -1;

    if (region->slot >= (uint32_t)vm->mem_slot_extent) {
        if (region->size == 0) return 0;
        if (vfl_resize_mem_slots(vm, (int)region->slot + 1) < 0) return -1;
    }

    slot = &vm->mem_slots[region->slot];

    if (region->size == 0) {
        kfree(slot->host_mem);
        if (slot->in_use && vm->nr_mem_slots > 0) vm->nr_mem_slots--;
        memset(slot, 0, sizeof(vfl_mem_slot_t));
        vfl_shrink_mem_slots(vm);
        return 0;
    }

    host_buf = (uint8_t *)kmalloc(region->size);
    if (!host_buf) return -1;
    memset(host_buf, 0, region->size);

    if (region->host_virt) {
        memcpy(host_buf, (void *)region->host_virt, region->size);
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

    return 0;
}

static uint8_t *vfl_guest_to_host(vfl_vm_t *vm, uint64_t guest_addr, uint64_t len) {
    int i;
    vfl_mem_slot_t *slot;

    for (i = 0; i < vm->mem_slot_extent; i++) {
        slot = &vm->mem_slots[i];
        if (!slot->in_use) continue;
        if (guest_addr >= slot->guest_phys &&
            guest_addr + len <= slot->guest_phys + slot->size) {
            return slot->host_mem + (guest_addr - slot->guest_phys);
        }
    }
    return NULL;
}

static uint64_t vfl_seg_addr(vfl_vcpu_t *vcpu, int seg_id, uint64_t offset) {
    vfl_segment_t *seg;

    switch (seg_id) {
        case 0: seg = &vcpu->sregs.es; break;
        case 1: seg = &vcpu->sregs.cs; break;
        case 2: seg = &vcpu->sregs.ss; break;
        case 3: seg = &vcpu->sregs.ds; break;
        case 4: seg = &vcpu->sregs.fs; break;
        case 5: seg = &vcpu->sregs.gs; break;
        default: seg = &vcpu->sregs.ds; break;
    }
    if (vcpu->sregs.cr0 & 1) {
        return seg->base + offset;
    }
    return (seg->selector << 4) + offset;
}

static void vfl_handle_io_out(vfl_run_t *run, uint16_t port,
                              uint8_t size, uint32_t data) {
    run->exit_reason = VFL_EXIT_IO;
    run->io.port = port;
    run->io.size = size;
    run->io.direction = 1;
    run->io.data = data;
}

static void vfl_handle_io_in(vfl_run_t *run, uint16_t port, uint8_t size) {
    run->exit_reason = VFL_EXIT_IO;
    run->io.port = port;
    run->io.size = size;
    run->io.direction = 0;
    run->io.data = 0;
}

static int vfl_handle_paravirt(vfl_vcpu_t *vcpu, vfl_run_t *run) {
    run->exit_reason = VFL_EXIT_PARAVIRT;
    run->paravirt.nr = vcpu->regs.rax;
    run->paravirt.args[0] = vcpu->regs.rbx;
    run->paravirt.args[1] = vcpu->regs.rcx;
    run->paravirt.args[2] = vcpu->regs.rdx;
    run->paravirt.args[3] = vcpu->regs.rsi;
    return 1;
}

#define MODRM_MOD(b) (((b) >> 6) & 3)
#define MODRM_REG(b) (((b) >> 3) & 7)
#define MODRM_RM(b)  ((b) & 7)

static uint64_t *vfl_gpr(vfl_vcpu_t *vcpu, int reg) {
    switch (reg) {
        case 0: return &vcpu->regs.rax;
        case 1: return &vcpu->regs.rcx;
        case 2: return &vcpu->regs.rdx;
        case 3: return &vcpu->regs.rbx;
        case 4: return &vcpu->regs.rsp;
        case 5: return &vcpu->regs.rbp;
        case 6: return &vcpu->regs.rsi;
        case 7: return &vcpu->regs.rdi;
        default: return &vcpu->regs.rax;
    }
}

int vfl_run(uint32_t vm_id, uint32_t vcpu_id, vfl_run_t *run) {
    vfl_vm_t *vm;
    vfl_vcpu_t *vcpu;
    uint8_t *code;
    uint64_t ip;
    uint8_t op;
    uint8_t modrm;
    int prefix_66;
    int steps;
    int max_steps;

    if (!vfl_vms) return -1;
    if (vm_id >= (uint32_t)vfl_vm_extent) return -1;
    vm = &vfl_vms[vm_id];
    if (!vm->active) return -1;
    if (vcpu_id >= (uint32_t)vm->nr_vcpus) return -1;
    vcpu = &vm->vcpus[vcpu_id];
    if (!vcpu->active) return -1;

    if (vcpu->halted) {
        run->exit_reason = VFL_EXIT_HLT;
        return 0;
    }

    max_steps = 65536;
    for (steps = 0; steps < max_steps; steps++) {
        ip = vfl_seg_addr(vcpu, 1, vcpu->regs.rip);
        code = vfl_guest_to_host(vm, ip, 16);
        if (!code) {
            run->exit_reason = VFL_EXIT_EXCEPTION;
            run->exception.vector = 14;
            run->exception.error_code = 0;
            return 0;
        }

        prefix_66 = 0;
        op = *code++;

        while (op == 0x66 || op == 0x67 ||
               op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E ||
               op == 0x64 || op == 0x65 || op == 0xF0 || op == 0xF2 || op == 0xF3) {
            if (op == 0x66) prefix_66 = 1;
            op = *code++;
            vcpu->regs.rip++;
        }

        switch (op) {
            case 0xF4:
                vcpu->regs.rip++;
                vcpu->halted = 1;
                run->exit_reason = VFL_EXIT_HLT;
                return 0;

            case 0x90:
                vcpu->regs.rip++;
                break;

            case 0xB0: case 0xB1: case 0xB2: case 0xB3:
            case 0xB4: case 0xB5: case 0xB6: case 0xB7:
                *((uint8_t *)vfl_gpr(vcpu, op & 7)) = *code;
                vcpu->regs.rip += 2;
                break;

            case 0xB8: case 0xB9: case 0xBA: case 0xBB:
            case 0xBC: case 0xBD: case 0xBE: case 0xBF:
                if (prefix_66) {
                    *vfl_gpr(vcpu, op & 7) = *(uint16_t *)code;
                    vcpu->regs.rip += 3;
                } else {
                    *vfl_gpr(vcpu, op & 7) = *(uint32_t *)code;
                    vcpu->regs.rip += 5;
                }
                break;

            case 0xE6:
                vfl_handle_io_out(run, *code, 1,
                                  (uint32_t)(vcpu->regs.rax & 0xFF));
                vcpu->regs.rip += 2;
                return 0;

            case 0xE7:
                if (prefix_66) {
                    vfl_handle_io_out(run, *code, 2,
                                      (uint32_t)(vcpu->regs.rax & 0xFFFF));
                } else {
                    vfl_handle_io_out(run, *code, 4,
                                      (uint32_t)(vcpu->regs.rax & 0xFFFFFFFF));
                }
                vcpu->regs.rip += 2;
                return 0;

            case 0xEE:
                vfl_handle_io_out(run,
                                  (uint16_t)(vcpu->regs.rdx & 0xFFFF), 1,
                                  (uint32_t)(vcpu->regs.rax & 0xFF));
                vcpu->regs.rip++;
                return 0;

            case 0xEF:
                if (prefix_66) {
                    vfl_handle_io_out(run,
                                      (uint16_t)(vcpu->regs.rdx & 0xFFFF), 2,
                                      (uint32_t)(vcpu->regs.rax & 0xFFFF));
                } else {
                    vfl_handle_io_out(run,
                                      (uint16_t)(vcpu->regs.rdx & 0xFFFF), 4,
                                      (uint32_t)(vcpu->regs.rax & 0xFFFFFFFF));
                }
                vcpu->regs.rip++;
                return 0;

            case 0xE4:
                vfl_handle_io_in(run, *code, 1);
                vcpu->regs.rip += 2;
                return 0;

            case 0xE5:
                if (prefix_66) {
                    vfl_handle_io_in(run, *code, 2);
                } else {
                    vfl_handle_io_in(run, *code, 4);
                }
                vcpu->regs.rip += 2;
                return 0;

            case 0xEC:
                vfl_handle_io_in(run,
                                 (uint16_t)(vcpu->regs.rdx & 0xFFFF), 1);
                vcpu->regs.rip++;
                return 0;

            case 0xED:
                if (prefix_66) {
                    vfl_handle_io_in(run,
                                     (uint16_t)(vcpu->regs.rdx & 0xFFFF), 2);
                } else {
                    vfl_handle_io_in(run,
                                     (uint16_t)(vcpu->regs.rdx & 0xFFFF), 4);
                }
                vcpu->regs.rip++;
                return 0;

            case 0xCD:
                if (*code == 0xFF) {
                    vcpu->regs.rip += 2;
                    if (vfl_handle_paravirt(vcpu, run))
                        return 0;
                } else {
                    run->exit_reason = VFL_EXIT_EXCEPTION;
                    run->exception.vector = *code;
                    run->exception.error_code = 0;
                    vcpu->regs.rip += 2;
                    return 0;
                }
                break;

            case 0x89:
                modrm = *code;
                if (MODRM_MOD(modrm) == 3) {
                    *vfl_gpr(vcpu, MODRM_RM(modrm)) = *vfl_gpr(vcpu, MODRM_REG(modrm));
                    vcpu->regs.rip += 2;
                } else {
                    run->exit_reason = VFL_EXIT_INTERNAL;
                    return -1;
                }
                break;

            case 0x8B:
                modrm = *code;
                if (MODRM_MOD(modrm) == 3) {
                    *vfl_gpr(vcpu, MODRM_REG(modrm)) = *vfl_gpr(vcpu, MODRM_RM(modrm));
                    vcpu->regs.rip += 2;
                } else {
                    run->exit_reason = VFL_EXIT_INTERNAL;
                    return -1;
                }
                break;

            case 0x01:
                modrm = *code;
                if (MODRM_MOD(modrm) == 3) {
                    *vfl_gpr(vcpu, MODRM_RM(modrm)) += *vfl_gpr(vcpu, MODRM_REG(modrm));
                    vcpu->regs.rip += 2;
                } else {
                    run->exit_reason = VFL_EXIT_INTERNAL;
                    return -1;
                }
                break;

            case 0x29:
                modrm = *code;
                if (MODRM_MOD(modrm) == 3) {
                    *vfl_gpr(vcpu, MODRM_RM(modrm)) -= *vfl_gpr(vcpu, MODRM_REG(modrm));
                    vcpu->regs.rip += 2;
                } else {
                    run->exit_reason = VFL_EXIT_INTERNAL;
                    return -1;
                }
                break;

            case 0x31:
                modrm = *code;
                if (MODRM_MOD(modrm) == 3) {
                    *vfl_gpr(vcpu, MODRM_RM(modrm)) ^= *vfl_gpr(vcpu, MODRM_REG(modrm));
                    vcpu->regs.rip += 2;
                } else {
                    run->exit_reason = VFL_EXIT_INTERNAL;
                    return -1;
                }
                break;

            case 0xEB:
                vcpu->regs.rip += 2 + (int8_t)*code;
                break;

            case 0xE9:
                if (prefix_66) {
                    vcpu->regs.rip += 3 + (int16_t)(*(uint16_t *)code);
                } else {
                    vcpu->regs.rip += 5 + (int32_t)(*(uint32_t *)code);
                }
                break;

            case 0x74:
                if (vcpu->regs.rflags & (1 << 6))
                    vcpu->regs.rip += 2 + (int8_t)*code;
                else
                    vcpu->regs.rip += 2;
                break;

            case 0x75:
                if (!(vcpu->regs.rflags & (1 << 6)))
                    vcpu->regs.rip += 2 + (int8_t)*code;
                else
                    vcpu->regs.rip += 2;
                break;

            case 0x39:
                modrm = *code;
                if (MODRM_MOD(modrm) == 3) {
                    uint64_t a, b, r;
                    a = *vfl_gpr(vcpu, MODRM_RM(modrm));
                    b = *vfl_gpr(vcpu, MODRM_REG(modrm));
                    r = a - b;
                    vcpu->regs.rflags &= ~((1 << 0) | (1 << 6) | (1 << 7) | (1 << 11));
                    if (r == 0) vcpu->regs.rflags |= (1 << 6);
                    if (a < b) vcpu->regs.rflags |= (1 << 0);
                    if (r & (1ULL << 63)) vcpu->regs.rflags |= (1 << 7);
                    vcpu->regs.rip += 2;
                } else {
                    run->exit_reason = VFL_EXIT_INTERNAL;
                    return -1;
                }
                break;

            case 0xFA:
                vcpu->regs.rip++;
                break;

            case 0xFB:
                vcpu->regs.rip++;
                break;

            default:
                run->exit_reason = VFL_EXIT_INTERNAL;
                return -1;
        }

    }

    run->exit_reason = VFL_EXIT_INTERNAL;
    return 0;
}

int vfl_vm_info(uint32_t vm_id, vfl_vm_info_t *info) {
    vfl_vm_t *vm;
    int i;
    uint64_t total;

    if (!vfl_vms) return -1;
    if (vm_id >= (uint32_t)vfl_vm_extent) return -1;
    vm = &vfl_vms[vm_id];
    if (!vm->active) return -1;

    total = 0;
    for (i = 0; i < vm->mem_slot_extent; i++) {
        if (vm->mem_slots[i].in_use)
            total += vm->mem_slots[i].size;
    }

    info->vm_id = vm_id;
    info->nr_vcpus = (uint32_t)vm->nr_vcpus;
    info->nr_mem_slots = (uint32_t)vm->nr_mem_slots;
    info->total_mem = total;
    return 0;
}

static int vfl_ioctl(vfs_node_t *node, unsigned long request, void *arg) {
    (void)node;

    switch (request) {
        case VFL_IOCTL_GET_API_VERSION:
            if (arg) *(uint32_t *)arg = VFL_API_VERSION;
            return 0;

        case VFL_IOCTL_CREATE_VM:
            return vfl_create_vm();

        case VFL_IOCTL_DESTROY_VM:
            if (!arg) return -1;
            return vfl_destroy_vm(*(uint32_t *)arg);

        case VFL_IOCTL_CREATE_VCPU:
            if (!arg) return -1;
            return vfl_create_vcpu(*(uint32_t *)arg);

        case VFL_IOCTL_SET_REGS: {
            uint32_t *ids = (uint32_t *)arg;
            vfl_regs_t *regs = (vfl_regs_t *)((uint8_t *)arg + 8);
            return vfl_set_regs(ids[0], ids[1], regs);
        }

        case VFL_IOCTL_GET_REGS: {
            uint32_t *ids = (uint32_t *)arg;
            vfl_regs_t *regs = (vfl_regs_t *)((uint8_t *)arg + 8);
            return vfl_get_regs(ids[0], ids[1], regs);
        }

        case VFL_IOCTL_SET_MEMORY: {
            uint32_t vm_id = *(uint32_t *)arg;
            vfl_memory_region_t *region = (vfl_memory_region_t *)((uint8_t *)arg + 4);
            return vfl_set_memory(vm_id, region);
        }

        case VFL_IOCTL_RUN: {
            uint32_t *ids = (uint32_t *)arg;
            vfl_run_t *run_data = (vfl_run_t *)((uint8_t *)arg + 8);
            return vfl_run(ids[0], ids[1], run_data);
        }

        case VFL_IOCTL_SET_SREGS: {
            uint32_t *ids = (uint32_t *)arg;
            vfl_sregs_t *sregs = (vfl_sregs_t *)((uint8_t *)arg + 8);
            return vfl_set_sregs(ids[0], ids[1], sregs);
        }

        case VFL_IOCTL_GET_SREGS: {
            uint32_t *ids = (uint32_t *)arg;
            vfl_sregs_t *sregs = (vfl_sregs_t *)((uint8_t *)arg + 8);
            return vfl_get_sregs(ids[0], ids[1], sregs);
        }

        case VFL_IOCTL_VM_INFO: {
            uint32_t vm_id = *(uint32_t *)arg;
            vfl_vm_info_t *info = (vfl_vm_info_t *)((uint8_t *)arg + 4);
            return vfl_vm_info(vm_id, info);
        }

        default:
            return -1;
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

void vfl_register_devfs(void) {
    memset(&dev_vfl_node, 0, sizeof(vfs_node_t));
    strcpy(dev_vfl_node.name, "vfl");
    dev_vfl_node.flags = VFS_CHARDEVICE;
    dev_vfl_node.mask = 0660;
    dev_vfl_node.uid = 0;
    dev_vfl_node.gid = 0;
    dev_vfl_node.read = vfl_dev_read;
    dev_vfl_node.write = vfl_dev_write;
    dev_vfl_node.ioctl = vfl_ioctl;
    dev_vfl_node.ref_count = 1;
}

vfs_node_t *vfl_get_devfs_node(void) {
    return &dev_vfl_node;
}
