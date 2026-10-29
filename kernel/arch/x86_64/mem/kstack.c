#include <lebirun/kstack.h>
#include <lebirun/mem_map.h>
#include <lebirun/panic.h>
#include <lebirun/common.h>
#include <lebirun/spinlock.h>
#include <lebirun/task.h>
#include <lebirun/smp.h>
#include <string.h>
#include <stdio.h>

extern void vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);
extern void vmm_unmap_page(uint64_t virt_addr);
extern void *pmm_alloc_page(void);
extern void *pmm_alloc_low_page(void);
extern void pfa_free(uint64_t phys_addr);
extern void *kmalloc(size_t size);
extern void kfree(void *ptr);

typedef struct kstack_slot {
    uint64_t page_phys[KSTACK_USABLE_PAGES];
    uint16_t slot;
    uint8_t bottom_mapped;
    uint8_t syscall_bottom;
    uint16_t map_cpu;
    uint8_t multi_cpu;
} kstack_slot_t;

static kstack_slot_t **kstack_table;
static uint64_t kstack_table_size;
static int kstack_initialized = 0;
static spinlock_t kstack_lock = {0};

static void kstack_lock_acquire(uint64_t *flags) {
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(*flags) :: "memory");
    while (!spin_trylock(&kstack_lock)) {
        cpu_relax();
    }
}

static void kstack_lock_release(uint64_t flags) {
    spin_unlock(&kstack_lock);
    __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

static uint64_t slot_guard_addr(int slot) {
    return KSTACK_REGION_START + (uint64_t)slot * KSTACK_SLOT_SIZE;
}

static uint64_t slot_bottom_addr(int slot) {
    return KSTACK_REGION_START + (uint64_t)slot * KSTACK_SLOT_SIZE + PAGE_SIZE;
}

static uint64_t slot_page_addr(int slot, int page_idx) {
    return KSTACK_REGION_START + (uint64_t)slot * KSTACK_SLOT_SIZE + (1 + page_idx) * PAGE_SIZE;
}

static void kstack_note_cpu(kstack_slot_t *entry) {
    uint16_t cpu;

    cpu = (uint16_t)smp_processor_id();
    if (entry->map_cpu != cpu) {
        entry->map_cpu = cpu;
        entry->multi_cpu = 1;
    }
}

static void kstack_drop_bottom(kstack_slot_t *entry, int slot, uint64_t phys,
                               int force) {
    vmm_unmap_page(slot_page_addr(slot, 0));
    if (force || entry->multi_cpu) smp_tlb_flush_all_sync();
    entry->multi_cpu = 0;
    pfa_free(phys);
}

static int addr_to_slot(uint64_t addr) {
    uint64_t offset;
    if (addr < KSTACK_REGION_START) return -1;
    offset = addr - KSTACK_REGION_START;
    return (int)(offset / KSTACK_SLOT_SIZE);
}

static kstack_slot_t *kstack_find_slot_locked(int slot) {
    if (slot < 0 || (uint64_t)slot >= kstack_table_size) return NULL;
    return kstack_table[slot];
}

static int kstack_table_grow(void) {
    kstack_slot_t **table;
    kstack_slot_t **old;
    uint64_t size;
    uint64_t flags;

    size = kstack_table_size ? kstack_table_size * 2 : 16;
    if (size > KSTACK_SLOT_COUNT) size = KSTACK_SLOT_COUNT;
    if (size <= kstack_table_size) return -1;
    table = (kstack_slot_t **)kcalloc(size, sizeof(*table));
    if (!table) return -1;
    kstack_lock_acquire(&flags);
    old = table;
    if (kstack_table_size < size) {
        if (kstack_table)
            memcpy(table, kstack_table, kstack_table_size * sizeof(*table));
        old = kstack_table;
        kstack_table = table;
        kstack_table_size = size;
    }
    kstack_lock_release(flags);
    kfree(old);
    return 0;
}

void KERNEL_INIT kstack_init(void) {
    kstack_table = NULL;
    kstack_table_size = 0;
    spinlock_init(&kstack_lock);
    kstack_initialized = 1;
}

uint8_t *kstack_alloc(void) {
    int slot;
    int p;
    void *phys;
    uint64_t page_virt;
    uint64_t base_virt;
    kstack_slot_t *entry;
    uint64_t flags;

    if (!kstack_initialized) return NULL;

    entry = (kstack_slot_t *)kmalloc(sizeof(kstack_slot_t));
    if (!entry) return NULL;
    memset(entry, 0, sizeof(kstack_slot_t));

    for (;;) {
        kstack_lock_acquire(&flags);
        for (slot = 0; (uint64_t)slot < kstack_table_size &&
                       kstack_table[slot]; slot++) ;
        if ((uint64_t)slot < kstack_table_size) break;
        kstack_lock_release(flags);
        if (kstack_table_grow() < 0) {
            kfree(entry);
            return NULL;
        }
    }
    entry->slot = (uint16_t)slot;
    kstack_table[slot] = entry;
    kstack_lock_release(flags);

    p = KSTACK_USABLE_PAGES - 1;
    phys = pmm_alloc_low_page();
    if (!phys) phys = pmm_alloc_page();
    if (phys) {
        page_virt = slot_page_addr(slot, p);
        vmm_map_page(page_virt, (uint64_t)phys, 0x003 | VMM_PTE_NX);
        if (vmm_get_phys_in_pml4(vmm_get_kernel_cr3(), page_virt) !=
            (uint64_t)phys) {
            pfa_free((uint64_t)phys);
            phys = NULL;
        }
    }
    if (!phys) {
        kstack_lock_acquire(&flags);
        if (kstack_table[slot] == entry) kstack_table[slot] = NULL;
        kstack_lock_release(flags);
        kfree(entry);
        return NULL;
    }

    kstack_lock_acquire(&flags);
    entry->page_phys[p] = (uint64_t)phys;
    kstack_lock_release(flags);
    base_virt = slot_bottom_addr(slot);
    if (smp_tlb_flush_all_sync() < 0) {
        kstack_free((uint8_t *)base_virt);
        return NULL;
    }
    memset((void *)page_virt, 0, PAGE_SIZE);

    return (uint8_t *)base_virt;
}

void kstack_free(uint8_t *base) {
    int slot;
    int p;
    uint64_t expected_base;
    kstack_slot_t *entry;
    uint64_t flags;

    if (!kstack_initialized || !base) return;

    slot = addr_to_slot((uint64_t)base);
    if (slot < 0) return;

    kstack_lock_acquire(&flags);
    entry = kstack_find_slot_locked(slot);
    if (!entry) {
        kstack_lock_release(flags);
        return;
    }

    expected_base = slot_bottom_addr(slot);
    if ((uint64_t)base != expected_base) {
        kstack_lock_release(flags);
        return;
    }

    for (p = 0; p < KSTACK_USABLE_PAGES; p++) {
        if (entry->page_phys[p]) {
            vmm_unmap_page(slot_page_addr(slot, p));
            pfa_free(entry->page_phys[p]);
        }
    }
    kstack_table[slot] = NULL;
    kstack_lock_release(flags);

    kfree(entry);
}

void kstack_reclaim_unused(void) {
    task_t *task;
    kstack_slot_t *entry;
    registers_t *frame;
    uint64_t base;
    uint64_t top_page;
    uint64_t top;
    uint64_t rsp;
    uint64_t frame_address;
    uint64_t phys;
    uint64_t flags;
    int slot;
    int cpu;

    if (!kstack_initialized) return;

    lock_scheduler();
    task = all_tasks_head;
    while (task) {
        if (task->state == TASK_DEAD || task->resources_released ||
            !task->kernel_stack_base) {
            task = task->all_next;
            continue;
        }
        for (cpu = 0; cpu < cpu_count; cpu++) {
            if (!cpus[cpu].active) continue;
            if (cpus[cpu].running_task == task ||
                __atomic_load_n(&cpus[cpu].switching_task, __ATOMIC_ACQUIRE) == task)
                break;
        }
        if (cpu < cpu_count) {
            task = task->all_next;
            continue;
        }
        base = (uint64_t)task->kernel_stack_base;
        top_page = base + PAGE_SIZE;
        top = base + KSTACK_USABLE_SIZE;
        rsp = task->regs.rsp;
        if (top_page < base || top < top_page ||
            rsp < top_page || rsp >= top) {
            task = task->all_next;
            continue;
        }
        frame = task->syscall_frame;
        if (frame) {
            frame_address = (uint64_t)frame;
            if (frame_address < top_page || frame_address >= top ||
                sizeof(registers_t) > top - frame_address) {
                task = task->all_next;
                continue;
            }
        }
        slot = addr_to_slot(base);
        if (slot < 0 || base != slot_bottom_addr(slot)) {
            task = task->all_next;
            continue;
        }
        kstack_lock_acquire(&flags);
        entry = kstack_find_slot_locked(slot);
        if (entry && entry->page_phys[0]) {
            phys = entry->page_phys[0];
            entry->page_phys[0] = 0;
            entry->bottom_mapped = 0;
            entry->syscall_bottom = 0;
            kstack_drop_bottom(entry, slot, phys, 1);
        }
        kstack_lock_release(flags);
        task = task->all_next;
    }
    unlock_scheduler();
}

void kstack_memory_stats(uint64_t *slots, uint64_t *pages) {
    kstack_slot_t *entry;
    uint64_t slot_count;
    uint64_t page_count;
    uint64_t flags;
    uint64_t s;
    int i;

    slot_count = 0;
    page_count = 0;
    kstack_lock_acquire(&flags);
    for (s = 0; s < kstack_table_size; s++) {
        entry = kstack_table[s];
        if (!entry) continue;
        slot_count++;
        for (i = 0; i < KSTACK_USABLE_PAGES; i++) {
            if (entry->page_phys[i]) page_count++;
        }
    }
    kstack_lock_release(flags);
    if (slots) *slots = slot_count;
    if (pages) *pages = page_count;
}

int kstack_page_fault_handler(uint64_t fault_addr, uint64_t fault_rip,
                              uint64_t fault_rsp) {
    int slot;
    int page_idx;
    void *phys;
    uint64_t page_virt;
    uint64_t bottom;
    uint64_t guard;
    kstack_slot_t *entry;
    uint64_t flags;
    uint64_t top;
    uint64_t used;
    int self_slot;
    int result;

    if (!kstack_initialized) return 0;
    if (fault_addr < KSTACK_REGION_START) return 0;

    slot = addr_to_slot(fault_addr);
    if (slot < 0) return 0;
    kstack_lock_acquire(&flags);
    entry = kstack_find_slot_locked(slot);
    if (!entry) {
        kstack_lock_release(flags);
        return 0;
    }
    kstack_lock_release(flags);

    guard = slot_guard_addr(slot);
    page_virt = fault_addr & ~(PAGE_SIZE - 1);

    if (page_virt == guard) {
        top = slot_bottom_addr(slot) + KSTACK_USABLE_SIZE;
        self_slot = addr_to_slot(fault_rsp);
        used = self_slot == slot && fault_rsp <= top ? top - fault_rsp : 0;
        kernel_panic_msg("KSTACK BAD WRITE slot=%d self=%d use=%u max=%u addr=0x%016lX rip=0x%016lX rsp=0x%016lX",
                         slot, self_slot, (unsigned)used,
                         (unsigned)KSTACK_USABLE_SIZE, fault_addr,
                         fault_rip, fault_rsp);
        return 0;
    }

    bottom = slot_bottom_addr(slot);
    if (page_virt < bottom || page_virt >= bottom + KSTACK_USABLE_SIZE) return 0;
    page_idx = (int)((page_virt - bottom) / PAGE_SIZE);
    kstack_lock_acquire(&flags);
    entry = kstack_find_slot_locked(slot);
    if (!entry || entry->page_phys[page_idx]) {
        kstack_lock_release(flags);
        return 0;
    }
    kstack_lock_release(flags);

    phys = pmm_alloc_low_page();
    if (!phys) phys = pmm_alloc_page();
    if (!phys) return 0;

    kstack_lock_acquire(&flags);
    entry = kstack_find_slot_locked(slot);
    if (!entry || entry->page_phys[page_idx]) {
        result = entry ? 1 : 0;
        kstack_lock_release(flags);
        pfa_free((uint64_t)phys);
        return result;
    }
    vmm_map_page(page_virt, (uint64_t)phys, 0x003 | VMM_PTE_NX);
    if (vmm_get_phys_in_pml4(vmm_get_kernel_cr3(), page_virt) !=
        (uint64_t)phys) {
        kstack_lock_release(flags);
        pfa_free((uint64_t)phys);
        return 0;
    }
    memset((void *)page_virt, 0, PAGE_SIZE);
    entry->page_phys[page_idx] = (uint64_t)phys;
    if (page_idx == 0) entry->bottom_mapped = 1;
    kstack_lock_release(flags);

    return 1;
}

int kstack_is_in_region(uint64_t addr) {
    return addr >= KSTACK_REGION_START;
}

int kstack_prepare_syscall(void) {
    uint64_t flags;
    uint64_t rsp;
    int current_slot;
    kstack_slot_t *entry;
    uint64_t lock_flags;

    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
    current_slot = addr_to_slot(rsp);
    kstack_lock_acquire(&lock_flags);
    entry = kstack_find_slot_locked(current_slot);
    if (current_slot < 0 || !entry) {
        kstack_lock_release(lock_flags);
        __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
        return -1;
    }

    kstack_note_cpu(entry);
    if (entry->bottom_mapped == 2 &&
        rsp >= slot_page_addr(current_slot, 1)) {
        if (entry->page_phys[0]) {
            kstack_drop_bottom(entry, current_slot, entry->page_phys[0], 0);
            entry->page_phys[0] = 0;
        }
        entry->bottom_mapped = 0;
        entry->syscall_bottom = 0;
    } else if (entry->bottom_mapped == 2) {
        entry->bottom_mapped = 1;
    }

    kstack_lock_release(lock_flags);
    __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
    return 0;
}

int kstack_expand_syscall(void) {
    uint64_t flags;
    uint64_t rsp;
    uint64_t page_virt;
    uint64_t lock_flags;
    void *phys;
    int slot;
    int p;
    int result;
    kstack_slot_t *entry;

    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
    slot = addr_to_slot(rsp);
    result = -1;
    if (slot < 0) goto out;

    for (p = 0; p < KSTACK_USABLE_PAGES; p++) {
        kstack_lock_acquire(&lock_flags);
        entry = kstack_find_slot_locked(slot);
        if (!entry) {
            kstack_lock_release(lock_flags);
            goto out;
        }
        if (entry->page_phys[p]) {
            kstack_lock_release(lock_flags);
            continue;
        }
        kstack_lock_release(lock_flags);
        phys = pmm_alloc_low_page();
        if (!phys) phys = pmm_alloc_page();
        if (!phys) goto out;
        kstack_lock_acquire(&lock_flags);
        entry = kstack_find_slot_locked(slot);
        if (!entry || entry->page_phys[p]) {
            kstack_lock_release(lock_flags);
            pfa_free((uint64_t)phys);
            if (!entry) goto out;
            continue;
        }
        page_virt = slot_page_addr(slot, p);
        vmm_map_page(page_virt, (uint64_t)phys, 0x003 | VMM_PTE_NX);
        if (vmm_get_phys_in_pml4(vmm_get_kernel_cr3(), page_virt) !=
            (uint64_t)phys) {
            kstack_lock_release(lock_flags);
            pfa_free((uint64_t)phys);
            goto out;
        }
        memset((void *)page_virt, 0, PAGE_SIZE);
        entry->page_phys[p] = (uint64_t)phys;
        if (p == 0) {
            entry->bottom_mapped = 1;
            entry->syscall_bottom = 1;
        }
        kstack_lock_release(lock_flags);
    }
    kstack_lock_acquire(&lock_flags);
    entry = kstack_find_slot_locked(slot);
    if (entry) {
        entry->bottom_mapped = 1;
        result = 0;
    }
    kstack_lock_release(lock_flags);
out:
    __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
    return result;
}

void kstack_finish_syscall(void) {
    uint64_t flags;
    uint64_t rsp;
    uint64_t phys;
    int slot;
    kstack_slot_t *entry;
    uint64_t lock_flags;

    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
    slot = addr_to_slot(rsp);
    kstack_lock_acquire(&lock_flags);
    entry = kstack_find_slot_locked(slot);
    phys = 0;
    if (entry) kstack_note_cpu(entry);
    if (slot >= 0 && entry && entry->syscall_bottom &&
        rsp >= slot_page_addr(slot, 1)) {
        phys = entry->page_phys[0];
        entry->page_phys[0] = 0;
        entry->bottom_mapped = 0;
        entry->syscall_bottom = 0;
    } else if (slot >= 0 && entry && entry->page_phys[0]) {
        entry->bottom_mapped = 2;
    }
    if (phys) kstack_drop_bottom(entry, slot, phys, 0);
    kstack_lock_release(lock_flags);
    __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}
