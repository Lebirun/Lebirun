#include "task_priv.h"

#define KSM_SPAN 0x4000000ULL

extern void *pt_direct_ptr(uint64_t phys_addr);

typedef struct {
    uint64_t hash;
    uint64_t phys;
    uint64_t address;
    uint64_t flags;
    task_t *task;
} ksm_item_t;

static uint64_t ksm_cursor;

static uint64_t ksm_hash(const uint64_t *words) {
    uint64_t hash;
    int i;

    hash = 0xCBF29CE484222325ULL;
    for (i = 0; i < (int)(PAGE_SIZE / sizeof(uint64_t)); i++)
        hash = (hash ^ words[i]) * 0x100000001B3ULL;
    return hash;
}

static int ksm_area_ok(task_file_map_t *area) {
    int file_backed;

    file_backed = area->node && !(area->map_flags & TASK_VMA_ANONYMOUS);
    if (area->map_flags & TASK_VMA_SHARED) return 0;
    if (file_backed) return (area->flags & VMM_PTE_WRITE) != 0;
    return (area->map_flags & TASK_VMA_ANONYMOUS) != 0;
}

static int ksm_merge(ksm_item_t *target, ksm_item_t *source) {
    uint64_t flags;

    flags = (target->flags & ~VMM_PTE_WRITE) | VMM_PTE_COW;
    if (flags != target->flags) {
        if (vmm_map_page_in_pml4(target->task->pml4_phys, target->address,
                                 target->phys, flags) != 0)
            return 0;
        target->flags = flags;
    }
    if (pfa_ref_share(target->phys)) return 0;
    flags = (source->flags & ~VMM_PTE_WRITE) | VMM_PTE_COW;
    if (vmm_map_page_in_pml4(source->task->pml4_phys, source->address,
                             target->phys, flags) != 0 ||
        vmm_get_phys_in_pml4(source->task->pml4_phys, source->address) !=
        target->phys) {
        vmm_map_page_in_pml4(source->task->pml4_phys, source->address,
                             source->phys, source->flags);
        pfa_cow_release64(target->phys);
        return 0;
    }
    pfa_cow_release64(source->phys);
    source->phys = target->phys;
    source->flags = flags;
    return 1;
}

uint64_t task_ksm_merge(uint64_t max_pages) {
    task_file_map_t *area;
    ksm_item_t *items;
    task_t *task;
    uint64_t start;
    uint64_t end;
    uint64_t next;
    uint64_t address;
    uint64_t area_end;
    uint64_t flags;
    uint64_t phys;
    uint64_t merged;
    uint64_t count;
    uint64_t first;
    uint64_t i;
    uint64_t j;
    uint8_t *page;
    int k;

    if (!max_pages) return 0;
    items = (ksm_item_t *)kmalloc(max_pages * sizeof(ksm_item_t));
    if (!items) return 0;
    start = ksm_cursor;
    end = start + KSM_SPAN;
    next = UINT64_MAX;
    count = 0;
    merged = 0;
    lock_scheduler();
    for (task = all_tasks_head; task && task_ptr_valid(task) &&
         count < max_pages; task = task->all_next) {
        if (!task->is_user || task->state == TASK_DEAD ||
            task->resources_released || !task->pml4_phys ||
            task_pml4_is_current_on_any_cpu(task->pml4_phys) ||
            !task_vm_trylock(task))
            continue;
        first = count;
        for (k = 0; k < task->vm->file_map_count && count < max_pages; k++) {
            area = &task->vm->file_maps[k];
            area_end = area->vaddr + area->memsz;
            if (!ksm_area_ok(area) || area_end <= start) continue;
            if (area->vaddr >= end) {
                if (area->vaddr < next) next = area->vaddr;
                continue;
            }
            address = area->vaddr > start ? area->vaddr : start;
            for (; address < area_end && address < end && count < max_pages;
                 address += PAGE_SIZE) {
                flags = vmm_get_flags_in_pml4(task->pml4_phys, address);
                if ((flags & (VMM_PTE_PRESENT | VMM_PTE_USER)) !=
                    (VMM_PTE_PRESENT | VMM_PTE_USER) ||
                    (flags & (VMM_PTE_NOFREE | VMM_PTE_SHARED)) ||
                    !(flags & (VMM_PTE_WRITE | VMM_PTE_COW)))
                    continue;
                phys = vmm_get_phys_in_pml4(task->pml4_phys, address);
                page = phys ? (uint8_t *)pt_direct_ptr(phys) : NULL;
                if (!page) continue;
                items[count].hash = ksm_hash((const uint64_t *)page);
                items[count].phys = phys;
                items[count].address = address;
                items[count].flags = flags;
                items[count].task = task;
                count++;
                if (count == max_pages) next = address + PAGE_SIZE;
            }
        }
        if (count == first) task_vm_unlock(task);
    }
    for (i = 1; i < count; i++) {
        for (j = 0; j < i; j++) {
            if (items[j].hash != items[i].hash ||
                items[j].phys == items[i].phys ||
                memcmp(pt_direct_ptr(items[j].phys),
                       pt_direct_ptr(items[i].phys), PAGE_SIZE))
                continue;
            merged += ksm_merge(&items[j], &items[i]);
            break;
        }
    }
    for (i = 0; i < count; i++)
        if (i + 1 == count || items[i + 1].task != items[i].task)
            task_vm_unlock(items[i].task);
    unlock_scheduler();
    ksm_cursor = count == max_pages ? next :
                 next != UINT64_MAX ? (next > end ? next : end) : 0;
    kfree(items);
    return merged;
}
