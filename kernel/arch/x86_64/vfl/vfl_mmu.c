#include "vfl_priv.h"
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <string.h>
#include <stdint.h>

vfl_mem_slot_t *vfl_find_slot(vfl_vm_t *vm, uint64_t addr, uint64_t len) {
    vfl_mem_slot_t *s;

    for (s = vm->mem_slots; s; s = s->next) {
        if (len > s->size) continue;
        if (addr >= s->guest_phys &&
            addr - s->guest_phys <= s->size - len) {
            return s;
        }
    }
    return NULL;
}

int vfl_phys_read(vfl_vm_t *vm, uint64_t addr, uint64_t len, uint8_t *out) {
    vfl_mem_slot_t *s = vfl_find_slot(vm, addr, len);

    if (!s) return 1;
    if (!(s->flags & VFL_MEM_FLAG_READ)) return -1;
    memcpy(out, s->host_mem + (addr - s->guest_phys), (size_t)len);
    return 0;
}

int vfl_phys_write(vfl_vm_t *vm, uint64_t addr, uint64_t len, uint8_t *in) {
    vfl_mem_slot_t *s = vfl_find_slot(vm, addr, len);

    if (!s) return 1;
    if (!(s->flags & VFL_MEM_FLAG_WRITE)) return -1;
    memcpy(s->host_mem + (addr - s->guest_phys), in, (size_t)len);
    return 0;
}

void vfl_mark_dirty(vfl_vm_t *vm, uint64_t phys, uint64_t len) {
    vfl_mem_slot_t *s;
    uint64_t first;
    uint64_t last;
    uint64_t p;

    if (len == 0) return;
    s = vfl_find_slot(vm, phys, len);
    if (!s) return;
    if (!s->dirty) {
        s->dirty_pages = (s->size + 0xFFFULL) >> 12;
        if (s->dirty_pages == 0) return;
        s->dirty = (uint8_t *)vfl_pages_alloc(
            (((s->dirty_pages + 7) >> 3) + 0xFFFULL) >> 12);
        if (!s->dirty) {
            s->dirty_pages = 0;
            return;
        }
    }
    first = (phys - s->guest_phys) >> 12;
    last = (phys - s->guest_phys + len - 1) >> 12;
    if (last >= s->dirty_pages) last = s->dirty_pages - 1;
    for (p = first; p <= last; p++)
        s->dirty[p >> 3] |= (uint8_t)(1 << (p & 7));
}

static int vfl_table_read(vfl_vm_t *vm, uint64_t phys, uint64_t *entry) {
    uint8_t b[8];
    int r;

    r = vfl_phys_read(vm, phys, 8, b);
    if (r) return r;
    *entry = vfl_get64(b);
    return 0;
}

static int vfl_table_update(vfl_vm_t *vm, uint64_t phys, uint64_t entry) {
    uint8_t b[8];

    vfl_put64(b, entry, 8);
    return vfl_phys_write(vm, phys, 8, b);
}

static int vfl_check_prot(vfl_vcpu_t *v, uint64_t entry, int access, int user_page) {
    int cpl = (int)(v->sregs.cs.selector & 3);
    int wp = (v->sregs.cr0 >> 16) & 1ULL ? 1 : 0;

    if (user_page && cpl == 3) return 0;
    if (!user_page && cpl == 3) return 1;
    if (access == VFL_MMU_WRITE && !(entry & 2ULL)) {
        if (cpl == 3) return 1;
        if (wp) return 1;
    }
    return 0;
}

static int vfl_walk_level(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t table, uint64_t idx,
                          int access, int user, uint64_t *entry, uint64_t *ephys) {
    uint64_t phys = table + idx * 8;
    int r;

    r = vfl_table_read(vm, phys, entry);
    if (r) return r;
    *ephys = phys;
    if (!(*entry & 1ULL)) {
        v->mmu_code = (access == VFL_MMU_WRITE ? 2U : 0U) | (user ? 4U : 0U) |
                      (access == VFL_MMU_EXEC ? 16U : 0U);
        return -2;
    }
    if (!(*entry & 0x20ULL)) {
        *entry |= 0x20ULL;
        vfl_table_update(vm, phys, *entry);
    }
    return 0;
}

static int vfl_fault_resv(vfl_vcpu_t *v, int access, int user) {
    v->mmu_vec = 14;
    v->mmu_code = 1U | 8U | (access == VFL_MMU_WRITE ? 2U : 0U) |
                  (user ? 4U : 0U) | (access == VFL_MMU_EXEC ? 16U : 0U);
    return -2;
}

static int vfl_fault_prot(vfl_vcpu_t *v, int access, int user, int exec_nx) {
    v->mmu_vec = 14;
    v->mmu_code = 1U | (access == VFL_MMU_WRITE ? 2U : 0U) |
                  (user ? 4U : 0U) | (access == VFL_MMU_EXEC ? 16U : 0U);
    (void)exec_nx;
    return -2;
}

static void vfl_tlb_fill(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t linear, int access,
                         int user, uint64_t phys, uint64_t leaf_phys,
                         uint64_t leaf_val, uint32_t leaf_w) {
    uint64_t tag = (linear >> 12) | ((uint64_t)(access & 3) << 60) |
                   (user ? (1ULL << 59) : 0);
    vfl_tlb_t *e = &v->tlb[((linear >> 12) ^ ((uint64_t)access << 4) ^
                            (linear >> 16)) & 63];
    vfl_mem_slot_t *sl;

    if (leaf_w != 4 && leaf_w != 8) return;
    sl = vfl_find_slot(vm, leaf_phys, leaf_w);
    if (!sl) return;
    e->epoch = vm->tlb_epoch;
    e->tag = tag;
    e->phys = phys & ~0xFFFULL;
    e->leaf_val = leaf_val;
    e->leaf_host = sl->host_mem + (leaf_phys - sl->guest_phys);
    e->leaf_w = leaf_w;
}

static int vfl_tlb_lookup(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t linear, int access,
                          int user, uint64_t *phys) {
    uint64_t tag = (linear >> 12) | ((uint64_t)(access & 3) << 60) |
                   (user ? (1ULL << 59) : 0);
    vfl_tlb_t *e = &v->tlb[((linear >> 12) ^ ((uint64_t)access << 4) ^
                            (linear >> 16)) & 63];
    uint64_t lv;

    if (e->epoch != vm->tlb_epoch || e->tag != tag) return 1;
    if (!e->leaf_host) return 1;
    lv = e->leaf_w == 8 ? *(uint64_t *)e->leaf_host : *(uint32_t *)e->leaf_host;
    if (lv != e->leaf_val) return 1;
    *phys = e->phys + (linear & 0xFFFULL);
    return 0;
}

int vfl_translate(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t linear, int access, uint64_t *phys) {
    int cpl = (int)(v->sregs.cs.selector & 3);
    int user = cpl == 3;
    int lm = vfl_long_mode(v);
    int pae = (v->sregs.cr4 >> 5) & 1ULL ? 1 : 0;
    int pse = (v->sregs.cr4 >> 4) & 1ULL ? 1 : 0;
    int nxe = (v->sregs.efer >> 11) & 1ULL ? 1 : 0;
    uint64_t entry;
    uint64_t ephys;
    int r;

    v->mmu_vec = 14;
    v->mmu_code = 0;
    if (!(v->sregs.cr0 & 1ULL) || !(v->sregs.cr0 & 0x80000000ULL)) {
        *phys = linear;
        return 0;
    }
    if (!vfl_tlb_lookup(vm, v, linear, access, user, phys)) return 0;
    if (lm) {
        uint64_t table = v->sregs.cr3 & 0x000FFFFFFFFFF000ULL;
        int shifts[4] = {39, 30, 21, 12};
        int i;

        if ((linear >> 48) != 0 && (linear >> 48) != 0xFFFFULL) {
            v->mmu_code = (access == VFL_MMU_WRITE ? 2U : 0U) | (user ? 4U : 0U) |
                          (access == VFL_MMU_EXEC ? 16U : 0U);
            return -2;
        }
        for (i = 0; i < 4; i++) {
            uint64_t idx = (linear >> shifts[i]) & 0x1FFULL;

            r = vfl_walk_level(vm, v, table, idx, access, user, &entry, &ephys);
            if (r) {
                if (r == -2) return -2;
                v->mmu_code = (access == VFL_MMU_WRITE ? 2U : 0U) | (user ? 4U : 0U) |
                              (access == VFL_MMU_EXEC ? 16U : 0U);
                return -2;
            }
            if ((entry & 0x8000000000000000ULL) && !nxe)
                return vfl_fault_resv(v, access, user);
            if (entry & 0x80ULL) {
                int big = (i == 1) ? 30 : 21;

                if (i != 1 && i != 2) return vfl_fault_resv(v, access, user);
                if (entry & (((1ULL << big) - 1) & 0xFFFFFFFFF000ULL))
                    return vfl_fault_resv(v, access, user);
                if (vfl_check_prot(v, entry, access, (entry >> 2) & 1ULL))
                    return vfl_fault_prot(v, access, user, 0);
                if (access == VFL_MMU_EXEC && nxe && (entry & 0x8000000000000000ULL))
                    return vfl_fault_prot(v, access, user, 1);
                if (access == VFL_MMU_WRITE && !(entry & 0x40ULL)) {
                    entry |= 0x40ULL;
                    vfl_table_update(vm, ephys, entry);
                }
                *phys = (entry & ~(((1ULL << big) - 1))) + (linear & ((1ULL << big) - 1));
                vfl_tlb_fill(vm, v, linear, access, user, *phys, ephys, entry, 8);
                return 0;
            }
            if (vfl_check_prot(v, entry, access, (entry >> 2) & 1ULL))
                return vfl_fault_prot(v, access, user, 0);
            table = entry & 0x000FFFFFFFFFF000ULL;
        }
        if (vfl_check_prot(v, entry, access, (entry >> 2) & 1ULL))
            return vfl_fault_prot(v, access, user, 0);
        if (access == VFL_MMU_EXEC && nxe && (entry & 0x8000000000000000ULL))
            return vfl_fault_prot(v, access, user, 1);
        if (access == VFL_MMU_WRITE && !(entry & 0x40ULL)) {
            entry |= 0x40ULL;
            vfl_table_update(vm, ephys, entry);
        }
        *phys = (entry & 0x000FFFFFFFFFF000ULL) + (linear & 0xFFFULL);
        vfl_tlb_fill(vm, v, linear, access, user, *phys, ephys, entry, 8);
        return 0;
    }
    if (pae) {
        uint64_t table = v->sregs.cr3 & ~0x1FULL;
        uint64_t idx = (linear >> 30) & 3ULL;

        r = vfl_walk_level(vm, v, table, idx, access, user, &entry, &ephys);
        if (r) {
            if (r == -2) return -2;
            v->mmu_code = (access == VFL_MMU_WRITE ? 2U : 0U) | (user ? 4U : 0U);
            return -2;
        }
        if ((entry & 0x8000000000000000ULL) && !nxe)
            return vfl_fault_resv(v, access, user);
        if (vfl_check_prot(v, entry, access, (entry >> 2) & 1ULL))
            return vfl_fault_prot(v, access, user, 0);
        table = entry & 0xFFFFFF000ULL;
        if (entry & 0x80ULL) return vfl_fault_resv(v, access, user);
        {
            uint64_t pde = table + ((linear >> 21) & 0x1FFULL) * 8;

            r = vfl_table_read(vm, pde, &entry);
            if (r) {
                v->mmu_code = (access == VFL_MMU_WRITE ? 2U : 0U) | (user ? 4U : 0U);
                return -2;
            }
            if (!(entry & 1ULL)) {
                v->mmu_code = (access == VFL_MMU_WRITE ? 2U : 0U) | (user ? 4U : 0U);
                return -2;
            }
            if ((entry & 0x8000000000000000ULL) && !nxe)
                return vfl_fault_resv(v, access, user);
            if (!(entry & 0x20ULL)) {
                entry |= 0x20ULL;
                vfl_table_update(vm, pde, entry);
            }
            if (vfl_check_prot(v, entry, access, (entry >> 2) & 1ULL))
                return vfl_fault_prot(v, access, user, 0);
            if (entry & 0x80ULL) {
                if (entry & 0x1FE000ULL) return vfl_fault_resv(v, access, user);
                if (access == VFL_MMU_WRITE && !(entry & 0x40ULL)) {
                    entry |= 0x40ULL;
                    vfl_table_update(vm, pde, entry);
                }
                if (access == VFL_MMU_EXEC && nxe && (entry & 0x8000000000000000ULL))
                    return vfl_fault_prot(v, access, user, 1);
                *phys = (entry & 0xFFE00000ULL) + (linear & 0x1FFFFFULL);
                vfl_tlb_fill(vm, v, linear, access, user, *phys, pde, entry, 8);
                return 0;
            }
            {
                uint64_t pte = (entry & 0xFFFFFF000ULL) + ((linear >> 12) & 0x1FFULL) * 8;

                r = vfl_table_read(vm, pte, &entry);
                if (r) {
                    v->mmu_code = (access == VFL_MMU_WRITE ? 2U : 0U) | (user ? 4U : 0U);
                    return -2;
                }
                if (!(entry & 1ULL)) {
                    v->mmu_code = (access == VFL_MMU_WRITE ? 2U : 0U) | (user ? 4U : 0U);
                    return -2;
                }
                if ((entry & 0x8000000000000000ULL) && !nxe)
                    return vfl_fault_resv(v, access, user);
                if (!(entry & 0x20ULL)) {
                    entry |= 0x20ULL;
                    vfl_table_update(vm, pte, entry);
                }
                if (access == VFL_MMU_WRITE && !(entry & 0x40ULL)) {
                    entry |= 0x40ULL;
                    vfl_table_update(vm, pte, entry);
                }
                if (entry & 0x80ULL) return vfl_fault_resv(v, access, user);
                if (vfl_check_prot(v, entry, access, (entry >> 2) & 1ULL))
                    return vfl_fault_prot(v, access, user, 0);
                if (access == VFL_MMU_EXEC && nxe && (entry & 0x8000000000000000ULL))
                    return vfl_fault_prot(v, access, user, 1);
                *phys = (entry & 0xFFFFFF000ULL) + (linear & 0xFFFULL);
                vfl_tlb_fill(vm, v, linear, access, user, *phys, pte, entry, 8);
                return 0;
            }
        }
    }
    {
        uint64_t table = v->sregs.cr3 & ~0xFFFULL;
        uint64_t pde = table + ((linear >> 22) & 0x3FFULL) * 4;
        uint8_t b[4];

        if (vfl_phys_read(vm, pde, 4, b)) {
            v->mmu_code = (access == VFL_MMU_WRITE ? 2U : 0U) | (user ? 4U : 0U);
            return -2;
        }
        entry = vfl_get32(b);
        if (!(entry & 1ULL)) {
            v->mmu_code = (access == VFL_MMU_WRITE ? 2U : 0U) | (user ? 4U : 0U);
            return -2;
        }
        if (!(entry & 0x20ULL)) {
            uint8_t w[4];

            entry |= 0x20ULL;
            vfl_put64(w, entry, 4);
            vfl_phys_write(vm, pde, 4, w);
        }
        if (vfl_check_prot(v, entry, access, (entry >> 2) & 1ULL))
            return vfl_fault_prot(v, access, user, 0);
        if ((entry & 0x80ULL) && pse) {
            if (entry & 0x003FE000ULL) return vfl_fault_resv(v, access, user);
            if (access == VFL_MMU_WRITE && !(entry & 0x40ULL)) {
                uint8_t w[4];

                entry |= 0x40ULL;
                vfl_put64(w, entry, 4);
                vfl_phys_write(vm, pde, 4, w);
            }
            *phys = (entry & 0xFFC00000ULL) + (linear & 0x3FFFFFULL);
            vfl_tlb_fill(vm, v, linear, access, user, *phys, pde, entry, 4);
            return 0;
        }
        if (entry & 0x80ULL) return vfl_fault_resv(v, access, user);
        {
            uint64_t pte = (entry & 0xFFFFF000ULL) + ((linear >> 12) & 0x3FFULL) * 4;

            if (vfl_phys_read(vm, pte, 4, b)) {
                v->mmu_code = (access == VFL_MMU_WRITE ? 2U : 0U) | (user ? 4U : 0U);
                return -2;
            }
            entry = vfl_get32(b);
            if (!(entry & 1ULL)) {
                v->mmu_code = (access == VFL_MMU_WRITE ? 2U : 0U) | (user ? 4U : 0U);
                return -2;
            }
            if (!(entry & 0x20ULL)) {
                uint8_t w[4];

                entry |= 0x20ULL;
                vfl_put64(w, entry, 4);
                vfl_phys_write(vm, pte, 4, w);
            }
            if (access == VFL_MMU_WRITE && !(entry & 0x40ULL)) {
                uint8_t w[4];

                entry |= 0x40ULL;
                vfl_put64(w, entry, 4);
                vfl_phys_write(vm, pte, 4, w);
            }
            if (vfl_check_prot(v, entry, access, (entry >> 2) & 1ULL))
                return vfl_fault_prot(v, access, user, 0);
            *phys = (entry & 0xFFFFF000ULL) + (linear & 0xFFFULL);
            vfl_tlb_fill(vm, v, linear, access, user, *phys, pte, entry, 4);
            return 0;
        }
    }
}

int vfl_mem_read(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t addr, uint64_t len, uint8_t *out) {
    uint64_t phys;
    int r;

    r = vfl_translate(vm, v, addr, VFL_MMU_READ, &phys);
    if (r) return -2;
    if (vfl_apic_mmio(vm, v, phys) && len <= 4 &&
        vfl_apic_read(vm, v, phys, out, len) == 0)
        return 0;
    if (v->mmio_cache_valid && len <= v->mmio_cache_len &&
        addr >= v->mmio_cache_addr &&
        addr - v->mmio_cache_addr <= v->mmio_cache_len - len) {
        if (!vfl_find_slot(vm, phys, len)) {
            memcpy(out, v->mmio_cache_data + (addr - v->mmio_cache_addr), (size_t)len);
            v->mmio_cache_valid = 0;
            return 0;
        }
    }
    return vfl_phys_read(vm, phys, len, out);
}

int vfl_mem_write(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t addr, uint64_t len, uint8_t *in) {
    uint64_t phys;
    int r;

    r = vfl_translate(vm, v, addr, VFL_MMU_WRITE, &phys);
    if (r) return -2;
    if (vfl_apic_mmio(vm, v, phys) && len <= 4 &&
        vfl_apic_write(vm, v, phys, in, len) == 0)
        return 0;
    r = vfl_phys_write(vm, phys, len, in);
    if (r == 0) vfl_mark_dirty(vm, phys, len);
    return r;
}

int vfl_fetch_buf(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t addr, uint8_t *buf, int len) {
    int i = 0;

    while (i < len) {
        uint64_t phys;
        uint64_t chunk;
        uint64_t remain;
        vfl_mem_slot_t *s;
        int r;

        r = vfl_translate(vm, v, addr + (uint64_t)i, VFL_MMU_EXEC, &phys);
        if (r) return -2;
        s = vfl_find_slot(vm, phys, 1);
        if (!s) return 1;
        if (!(s->flags & VFL_MEM_FLAG_EXEC)) return -1;
        chunk = 0x1000ULL - (phys & 0xFFFULL);
        remain = s->size - (phys - s->guest_phys);
        if (chunk > remain) chunk = remain;
        if (chunk > (uint64_t)(len - i)) chunk = (uint64_t)(len - i);
        memcpy(buf + i, s->host_mem + (phys - s->guest_phys), (size_t)chunk);
        i += (int)chunk;
    }
    return 0;
}

int vfl_mem_fault(vfl_vcpu_t *v, vfl_run_t *run) {
    run->exit_reason = VFL_EXIT_EXCEPTION;
    run->exception.vector = v->mmu_vec;
    run->exception.error_code = v->mmu_code;
    v->last_exit = *run;
    return 1;
}
