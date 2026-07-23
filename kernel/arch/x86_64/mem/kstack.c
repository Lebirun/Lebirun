#include <lebirun/kstack.h>
#include <lebirun/mem_map.h>
#include <lebirun/panic.h>
#include <lebirun/common.h>
#include <lebirun/spinlock.h>
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
    struct kstack_slot *next;
    uint16_t slot;
    uint8_t bottom_mapped;
    uint8_t syscall_bottom;
} kstack_slot_t;

static kstack_slot_t *kstack_slots;
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

static int addr_to_slot(uint64_t addr) {
    uint64_t offset;
    if (addr < KSTACK_REGION_START || addr >= KSTACK_REGION_END) return -1;
    offset = addr - KSTACK_REGION_START;
    return (int)(offset / KSTACK_SLOT_SIZE);
}

static kstack_slot_t *kstack_find_slot_locked(int slot) {
    kstack_slot_t *entry;

    entry = kstack_slots;
    while (entry && entry->slot < slot) entry = entry->next;
    if (entry && entry->slot == slot) return entry;
    return NULL;
}

void KERNEL_INIT kstack_init(void) {
    kstack_slots = NULL;
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
    kstack_slot_t *current;
    kstack_slot_t *previous;
    uint64_t flags;

    if (!kstack_initialized) return NULL;

    entry = (kstack_slot_t *)kmalloc(sizeof(kstack_slot_t));
    if (!entry) return NULL;
    memset(entry, 0, sizeof(kstack_slot_t));

    kstack_lock_acquire(&flags);
    slot = 0;
    previous = NULL;
    current = kstack_slots;
    while (current && current->slot == slot) {
        slot++;
        previous = current;
        current = current->next;
    }
    if (slot >= KSTACK_MAX_STACKS) {
        kstack_lock_release(flags);
        kfree(entry);
        return NULL;
    }

    entry->slot = (uint16_t)slot;
    entry->next = current;
    if (previous) previous->next = entry;
    else kstack_slots = entry;
    kstack_lock_release(flags);

    p = KSTACK_USABLE_PAGES - 1;
    phys = pmm_alloc_low_page();
    if (!phys) phys = pmm_alloc_page();
    if (!phys) {
        kstack_lock_acquire(&flags);
        previous = NULL;
        current = kstack_slots;
        while (current && current != entry) {
            previous = current;
            current = current->next;
        }
        if (current) {
            if (previous) previous->next = current->next;
            else kstack_slots = current->next;
        }
        kstack_lock_release(flags);
        kfree(entry);
        return NULL;
    }

    page_virt = slot_page_addr(slot, p);
    vmm_map_page(page_virt, (uint64_t)phys, 0x003);
    memset((void *)page_virt, 0, PAGE_SIZE);
    entry->page_phys[p] = (uint64_t)phys;

    base_virt = slot_bottom_addr(slot);
    return (uint8_t *)base_virt;
}

void kstack_free(uint8_t *base) {
    int slot;
    int p;
    uint64_t expected_base;
    kstack_slot_t *entry;
    kstack_slot_t *previous;
    uint64_t flags;

    if (!kstack_initialized || !base) return;

    slot = addr_to_slot((uint64_t)base);
    if (slot < 0) return;

    kstack_lock_acquire(&flags);
    previous = NULL;
    entry = kstack_slots;
    while (entry && entry->slot < slot) {
        previous = entry;
        entry = entry->next;
    }
    if (!entry || entry->slot != slot) {
        kstack_lock_release(flags);
        return;
    }

    expected_base = slot_bottom_addr(slot);
    if ((uint64_t)base != expected_base) {
        kstack_lock_release(flags);
        return;
    }

    if (previous) previous->next = entry->next;
    else kstack_slots = entry->next;
    kstack_lock_release(flags);

    for (p = 0; p < KSTACK_USABLE_PAGES; p++) {
        if (entry->page_phys[p]) {
            vmm_unmap_page(slot_page_addr(slot, p));
            pfa_free(entry->page_phys[p]);
        }
    }

    kfree(entry);
}

void kstack_reclaim_unused(void) {
}

void kstack_memory_stats(uint64_t *slots, uint64_t *pages) {
    kstack_slot_t *entry;
    uint64_t slot_count;
    uint64_t page_count;
    uint64_t flags;
    int i;

    slot_count = 0;
    page_count = 0;
    kstack_lock_acquire(&flags);
    entry = kstack_slots;
    while (entry) {
        slot_count++;
        for (i = 0; i < KSTACK_USABLE_PAGES; i++) {
            if (entry->page_phys[i]) page_count++;
        }
        entry = entry->next;
    }
    kstack_lock_release(flags);
    if (slots) *slots = slot_count;
    if (pages) *pages = page_count;
}

int kstack_page_fault_handler(uint64_t fault_addr) {
    int slot;
    int page_idx;
    void *phys;
    uint64_t page_virt;
    uint64_t bottom;
    uint64_t guard;
    kstack_slot_t *entry;
    uint64_t flags;

    if (!kstack_initialized) return 0;
    if (fault_addr < KSTACK_REGION_START || fault_addr >= KSTACK_REGION_END) return 0;

    slot = addr_to_slot(fault_addr);
    if (slot < 0) return 0;
    kstack_lock_acquire(&flags);
    entry = kstack_find_slot_locked(slot);
    kstack_lock_release(flags);
    if (!entry) return 0;

    guard = slot_guard_addr(slot);
    page_virt = fault_addr & ~(PAGE_SIZE - 1);

    if (page_virt == guard) {
        kernel_panic("KERNEL STACK OVERFLOW", NULL);
        return 0;
    }

    bottom = slot_bottom_addr(slot);
    if (page_virt < bottom || page_virt >= bottom + KSTACK_USABLE_SIZE) return 0;
    page_idx = (int)((page_virt - bottom) / PAGE_SIZE);
    if (entry->page_phys[page_idx]) return 0;

    phys = pmm_alloc_low_page();
    if (!phys) phys = pmm_alloc_page();
    if (!phys) return 0;

    vmm_map_page(page_virt, (uint64_t)phys, 0x003);
    memset((void *)page_virt, 0, PAGE_SIZE);
    entry->page_phys[page_idx] = (uint64_t)phys;
    if (page_idx == 0) entry->bottom_mapped = 1;

    return 1;
}

int kstack_is_in_region(uint64_t addr) {
    return (addr >= KSTACK_REGION_START && addr < KSTACK_REGION_END);
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
    kstack_lock_release(lock_flags);
    if (current_slot < 0 || !entry) {
        __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
        return -1;
    }

    if (entry->bottom_mapped == 2 &&
        rsp >= slot_page_addr(current_slot, 1)) {
        if (entry->page_phys[0]) {
            vmm_unmap_page(slot_page_addr(current_slot, 0));
            pfa_free(entry->page_phys[0]);
            entry->page_phys[0] = 0;
        }
        entry->bottom_mapped = 0;
        entry->syscall_bottom = 0;
    } else if (entry->bottom_mapped == 2) {
        entry->bottom_mapped = 1;
    }

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
    kstack_slot_t *entry;

    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
    slot = addr_to_slot(rsp);
    if (slot < 0) {
        __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
        return -1;
    }

    kstack_lock_acquire(&lock_flags);
    entry = kstack_find_slot_locked(slot);
    if (!entry) {
        kstack_lock_release(lock_flags);
        __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
        return -1;
    }
    if (entry->page_phys[0]) {
        entry->bottom_mapped = 1;
        kstack_lock_release(lock_flags);
        __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
        return 0;
    }
    kstack_lock_release(lock_flags);

    phys = pmm_alloc_low_page();
    if (!phys) phys = pmm_alloc_page();
    if (!phys) {
        __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
        return -1;
    }

    kstack_lock_acquire(&lock_flags);
    entry = kstack_find_slot_locked(slot);
    if (!entry || entry->page_phys[0]) {
        kstack_lock_release(lock_flags);
        pfa_free((uint64_t)phys);
        __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
        return entry ? 0 : -1;
    }
    kstack_lock_release(lock_flags);
    page_virt = slot_page_addr(slot, 0);
    vmm_map_page(page_virt, (uint64_t)phys, 0x003);
    memset((void *)page_virt, 0, PAGE_SIZE);
    entry->page_phys[0] = (uint64_t)phys;
    entry->bottom_mapped = 1;
    entry->syscall_bottom = 1;
    __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
    return 0;
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
    if (slot >= 0 && entry && entry->syscall_bottom &&
        rsp >= slot_page_addr(slot, 1)) {
        phys = entry->page_phys[0];
        entry->page_phys[0] = 0;
        entry->bottom_mapped = 0;
        entry->syscall_bottom = 0;
    } else if (slot >= 0 && entry && entry->page_phys[0]) {
        entry->bottom_mapped = 2;
    }
    kstack_lock_release(lock_flags);
    if (phys) {
        vmm_unmap_page(slot_page_addr(slot, 0));
        pfa_free(phys);
    }
    __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}
