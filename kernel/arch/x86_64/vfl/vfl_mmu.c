#include "vfl_priv.h"
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <string.h>
#include <stdint.h>

vfl_code_page_t *vfl_code_page(vfl_vm_t *vm, vfl_mem_slot_t *slot, uint64_t phys) {
    vfl_code_page_t *head;
    vfl_code_page_t *page;
    vfl_code_page_t *fresh;

    if (!slot->owned) return NULL;
    phys &= ~0xFFFULL;
    fresh = NULL;
    for (;;) {
        head = __atomic_load_n(&slot->code, __ATOMIC_ACQUIRE);
        for (page = head; page; page = page->next) {
            if (page->phys == phys) {
                if (fresh) kfree(fresh);
                return page;
            }
        }
        if (!fresh) {
            fresh = kmalloc(sizeof(*fresh));
            if (!fresh) return NULL;
            fresh->phys = phys;
            fresh->generation = 1;
        }
        fresh->next = head;
        if (__atomic_compare_exchange_n(&slot->code, &head, fresh, 0,
                                         __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
            __atomic_add_fetch(&vm->tlb_epoch, 1, __ATOMIC_RELEASE);
            return fresh;
        }
    }
}

void vfl_code_write(vfl_mem_slot_t *slot, uint64_t phys, uint64_t len) {
    vfl_code_page_t *page;
    uint64_t first;
    uint64_t last;

    if (!len) return;
    first = phys & ~0xFFFULL;
    last = (phys + len - 1) & ~0xFFFULL;
    for (page = __atomic_load_n(&slot->code, __ATOMIC_ACQUIRE); page; page = page->next)
        if (page->phys >= first && page->phys <= last)
            __atomic_add_fetch(&page->generation, 1, __ATOMIC_RELEASE);
}

void vfl_code_free(vfl_mem_slot_t *slot) {
    vfl_code_page_t *page;

    while ((page = slot->code) != NULL) {
        slot->code = page->next;
        kfree(page);
    }
}

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

static const uint8_t vfl_zero_page[4096];

uint8_t *vfl_slot_ptr(vfl_vm_t *vm, vfl_mem_slot_t *slot, uint64_t addr,
                       uint64_t len, int write) {
    uint64_t offset;
    uint64_t index;
    void **link;
    void **node;
    unsigned depth;

    offset = addr - slot->guest_phys;
    if (offset > slot->size || len > slot->size - offset) return NULL;
    if (!slot->owned) return slot->host_mem + offset;
    if (len > 4096 - (offset & 4095)) return NULL;
    index = offset >> 12;
    link = &slot->pages;
    depth = slot->depth;
    for (;;) {
        if (!*link) {
            if (!write) return (uint8_t *)vfl_zero_page + (offset & 4095);
            *link = vfl_pages_alloc(1);
            if (!*link) return NULL;
            if (!depth) {
                __atomic_add_fetch(&slot->resident_pages, 1, __ATOMIC_RELAXED);
                __atomic_add_fetch(&vm->tlb_epoch, 1, __ATOMIC_RELEASE);
            }
        }
        if (!depth) return (uint8_t *)*link + (offset & 4095);
        node = *link;
        depth--;
        link = &node[(index >> (depth * 9)) & 511];
    }
}

void vfl_slot_free_pages(void *pages, unsigned depth) {
    void **node;
    unsigned i;

    if (!pages) return;
    if (depth) {
        node = pages;
        for (i = 0; i < 512; i++)
            if (node[i]) vfl_slot_free_pages(node[i], depth - 1);
    }
    vfl_pages_free(pages, 1);
}

int vfl_phys_read(vfl_vm_t *vm, uint64_t addr, uint64_t len, uint8_t *out) {
    vfl_mem_slot_t *s;
    uint8_t *host;
    uint64_t chunk;

    s = vfl_find_slot(vm, addr, len);
    if (!s) return 1;
    if (!(s->flags & VFL_MEM_FLAG_READ)) return -1;
    while (len) {
        chunk = 4096 - ((addr - s->guest_phys) & 4095);
        if (chunk > len) chunk = len;
        host = vfl_slot_ptr(vm, s, addr, chunk, 0);
        if (!host) return -1;
        memcpy(out, host, chunk);
        out += chunk;
        addr += chunk;
        len -= chunk;
    }
    return 0;
}

int vfl_phys_write(vfl_vm_t *vm, uint64_t addr, uint64_t len, uint8_t *in) {
    vfl_mem_slot_t *s;
    uint8_t *host;
    uint64_t chunk;

    s = vfl_find_slot(vm, addr, len);
    if (!s) return 1;
    if (!(s->flags & VFL_MEM_FLAG_WRITE)) return -1;
    while (len) {
        chunk = 4096 - ((addr - s->guest_phys) & 4095);
        if (chunk > len) chunk = len;
        host = vfl_slot_ptr(vm, s, addr, chunk, 1);
        if (!host) return -3;
        memcpy(host, in, chunk);
        vfl_code_write(s, addr, chunk);
        vfl_dirty_slot(s, addr, chunk);
        in += chunk;
        addr += chunk;
        len -= chunk;
    }
    return 0;
}

void vfl_dirty_slot(vfl_mem_slot_t *s, uint64_t phys, uint64_t len) {
    uint8_t *bitmap;
    uint8_t *expected;
    uint64_t pages;
    uint64_t count;
    uint64_t first;
    uint64_t last;
    uint64_t p;

    if (!len) return;
    bitmap = __atomic_load_n(&s->dirty, __ATOMIC_ACQUIRE);
    if (!bitmap) {
        pages = ((s->size - 1) >> 12) + 1;
        count = (((pages + 7) >> 3) + 0xFFFULL) >> 12;
        bitmap = vfl_pages_alloc(count);
        if (!bitmap) return;
        __atomic_store_n(&s->dirty_pages, pages, __ATOMIC_RELAXED);
        expected = NULL;
        if (!__atomic_compare_exchange_n(&s->dirty, &expected, bitmap, 0,
                                          __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
            vfl_pages_free(bitmap, count);
            bitmap = expected;
        }
    }
    first = (phys - s->guest_phys) >> 12;
    last = (phys - s->guest_phys + len - 1) >> 12;
    for (p = first; p <= last; p++)
        __atomic_fetch_or(&bitmap[p >> 3], (uint8_t)(1U << (p & 7)), __ATOMIC_RELAXED);
}

void vfl_mark_dirty(vfl_vm_t *vm, uint64_t phys, uint64_t len) {
    vfl_mem_slot_t *slot;

    if (!len) return;
    slot = vfl_find_slot(vm, phys, len);
    if (!slot) return;
    vfl_dirty_slot(slot, phys, len);
    vfl_code_write(slot, phys, len);
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
    vfl_code_page_t *page;

    e->host = NULL;
    e->slot = NULL;
    e->code = NULL;
    e->leaf_host = NULL;
    if (leaf_w) {
        sl = vfl_find_slot(vm, leaf_phys, leaf_w);
        if (!sl) return;
        e->leaf_host = vfl_slot_ptr(vm, sl, leaf_phys, leaf_w, 0);
        if (!e->leaf_host) return;
    }
    e->epoch = __atomic_load_n(&vm->tlb_epoch, __ATOMIC_ACQUIRE);
    e->tag = tag;
    e->phys = phys & ~0xFFFULL;
    e->leaf_val = leaf_val;
    e->leaf_w = leaf_w;
    sl = vfl_find_slot(vm, e->phys, 0x1000);
    if (!sl) return;
    e->slot = sl;
    e->host = vfl_slot_ptr(vm, sl, e->phys, 0x1000, access == VFL_MMU_WRITE);
    for (page = __atomic_load_n(&sl->code, __ATOMIC_ACQUIRE); page; page = page->next)
        if (page->phys == e->phys) {
            e->code = page;
            break;
        }
}

static int vfl_tlb_lookup(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t linear, int access,
                          int user, uint64_t *phys) {
    uint64_t tag = (linear >> 12) | ((uint64_t)(access & 3) << 60) |
                   (user ? (1ULL << 59) : 0);
    vfl_tlb_t *e = &v->tlb[((linear >> 12) ^ ((uint64_t)access << 4) ^
                            (linear >> 16)) & 63];
    uint64_t lv;

    if (e->epoch != __atomic_load_n(&vm->tlb_epoch, __ATOMIC_ACQUIRE) || e->tag != tag) return 1;
    if (e->leaf_w) {
        if (!e->leaf_host) return 1;
        lv = e->leaf_w == 8 ? *(uint64_t *)e->leaf_host : *(uint32_t *)e->leaf_host;
        if (lv != e->leaf_val) return 1;
    }
    *phys = e->phys + (linear & 0xFFFULL);
    return 0;
}

static int vfl_translate_address(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t linear, int access, uint64_t *phys) {
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
    if (!vfl_tlb_lookup(vm, v, linear, access, user, phys)) {
        if (v->stats) v->stats->tlb_hits++;
        return 0;
    }
    if (v->stats) v->stats->tlb_misses++;
    if (!(v->sregs.cr0 & 1ULL) || !(v->sregs.cr0 & 0x80000000ULL)) {
        *phys = linear;
        vfl_tlb_fill(vm, v, linear, access, user, *phys, 0, 0, 0);
        return 0;
    }
    if (v->stats) v->stats->page_walks++;
    if (lm) {
        uint64_t table = v->sregs.cr3 & 0x000FFFFFFFFFF000ULL;
        int shifts[4] = {39, 30, 21, 12};
        int i;

        if ((int64_t)linear >> 47 != 0 && (int64_t)linear >> 47 != -1) {
            v->mmu_vec = 13;
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
            if (access == VFL_MMU_EXEC && nxe && (entry & 0x8000000000000000ULL))
                return vfl_fault_prot(v, access, user, 1);
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
                *phys = (entry & 0x000FFFFFFFFFF000ULL & ~((1ULL << big) - 1)) +
                        (linear & ((1ULL << big) - 1));
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

int vfl_translate(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t linear, int access, uint64_t *phys) {
    int r;

    r = vfl_translate_address(vm, v, linear, access, phys);
    if (r == -2 && v->mmu_vec == 14) v->sregs.cr2 = linear;
    return r;
}

uint8_t *vfl_ram_ptr(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t addr, uint64_t phys,
                     uint64_t len, int access, vfl_tlb_t **entry) {
    vfl_tlb_t *e;
    uint64_t tag;
    uint32_t flag;

    if (len > 0x1000ULL - (addr & 0xFFFULL)) return NULL;
    tag = (addr >> 12) | ((uint64_t)(access & 3) << 60) |
          ((v->sregs.cs.selector & 3) == 3 ? (1ULL << 59) : 0);
    e = &v->tlb[((addr >> 12) ^ ((uint64_t)access << 4) ^ (addr >> 16)) & 63];
    if (e->epoch != __atomic_load_n(&vm->tlb_epoch, __ATOMIC_ACQUIRE) || e->tag != tag || !e->host ||
        e->phys != (phys & ~0xFFFULL)) return NULL;
    flag = access == VFL_MMU_WRITE ? VFL_MEM_FLAG_WRITE :
           access == VFL_MMU_EXEC ? VFL_MEM_FLAG_EXEC : VFL_MEM_FLAG_READ;
    if (!(e->slot->flags & flag)) return NULL;
    *entry = e;
    return e->host + (addr & 0xFFFULL);
}

static int vfl_mem_read_page(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t addr, uint64_t len, uint8_t *out) {
    uint8_t *host;
    vfl_tlb_t *entry;
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
    host = vfl_ram_ptr(vm, v, addr, phys, len, VFL_MMU_READ, &entry);
    if (host) {
        memcpy(out, host, (size_t)len);
        return 0;
    }
    return vfl_phys_read(vm, phys, len, out);
}

static int vfl_mem_write_page(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t addr, uint64_t len, uint8_t *in) {
    uint8_t *host;
    vfl_tlb_t *entry;
    uint64_t phys;
    int r;

    r = vfl_translate(vm, v, addr, VFL_MMU_WRITE, &phys);
    if (r) return -2;
    if (vfl_apic_mmio(vm, v, phys) && len <= 4 &&
        vfl_apic_write(vm, v, phys, in, len) == 0)
        return 0;
    host = vfl_ram_ptr(vm, v, addr, phys, len, VFL_MMU_WRITE, &entry);
    if (host) {
        memcpy(host, in, (size_t)len);
        if (entry->code)
            __atomic_add_fetch(&entry->code->generation, 1, __ATOMIC_RELEASE);
        vfl_dirty_slot(entry->slot, phys, len);
        return 0;
    }
    return vfl_phys_write(vm, phys, len, in);
}

int vfl_mem_read(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t addr, uint64_t len, uint8_t *out) {
    uint64_t chunk;
    int r;

    if (len <= 4096 - (addr & 4095)) return vfl_mem_read_page(vm, v, addr, len, out);
    while (len) {
        chunk = 4096 - (addr & 4095);
        if (chunk > len) chunk = len;
        r = vfl_mem_read_page(vm, v, addr, chunk, out);
        if (r) return r;
        addr += chunk;
        out += chunk;
        len -= chunk;
    }
    return 0;
}

int vfl_mem_write(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t addr, uint64_t len, uint8_t *in) {
    uint64_t offset;
    uint64_t chunk;
    uint64_t phys;
    vfl_mem_slot_t *slot;
    int r;

    if (len <= 4096 - (addr & 4095)) return vfl_mem_write_page(vm, v, addr, len, in);
    for (offset = 0; offset < len; offset += chunk) {
        chunk = 4096 - ((addr + offset) & 4095);
        if (chunk > len - offset) chunk = len - offset;
        if (vfl_translate(vm, v, addr + offset, VFL_MMU_WRITE, &phys)) return -2;
        slot = vfl_find_slot(vm, phys, chunk);
        if (!slot) return 1;
        if (!(slot->flags & VFL_MEM_FLAG_WRITE)) return -1;
    }
    while (len) {
        chunk = 4096 - (addr & 4095);
        if (chunk > len) chunk = len;
        r = vfl_mem_write_page(vm, v, addr, chunk, in);
        if (r) return r;
        addr += chunk;
        in += chunk;
        len -= chunk;
    }
    return 0;
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
        if (chunk > 4096 - ((phys - s->guest_phys) & 4095))
            chunk = 4096 - ((phys - s->guest_phys) & 4095);
        memcpy(buf + i, vfl_slot_ptr(vm, s, phys, chunk, 0), (size_t)chunk);
        i += (int)chunk;
    }
    return 0;
}

int vfl_mem_fault(vfl_vcpu_t *v, vfl_run_t *run) {
    return vfl_exit_exception(v, run, v->mmu_vec, v->mmu_code);
}
