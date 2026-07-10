#include <lebirun/kstack.h>
#include <lebirun/mem_map.h>
#include <lebirun/panic.h>
#include <lebirun/common.h>
#include <string.h>
#include <stdio.h>

extern void vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);
extern void vmm_unmap_page(uint64_t virt_addr);
extern void *pmm_alloc_page(void);
extern void *pmm_alloc_low_page(void);
extern void pfa_free(uint64_t phys_addr);
extern void *kmalloc(size_t size);
extern void kfree(void *ptr);

static uint8_t *slot_used;
static uint8_t *slot_bottom_mapped;
static uint64_t (*slot_page_phys)[KSTACK_USABLE_PAGES];
static int kstack_initialized = 0;
static int kstack_capacity = 0;

#define slot_top_phys(s)    slot_page_phys[s][KSTACK_USABLE_PAGES - 1]
#define slot_bottom_phys(s) slot_page_phys[s][0]

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

static int kstack_grow(int new_cap) {
    uint8_t *new_used;
    uint8_t *new_mapped;
    uint64_t (*new_phys)[KSTACK_USABLE_PAGES];
    uint64_t alloc_cap;

    if (new_cap > KSTACK_MAX_STACKS) new_cap = KSTACK_MAX_STACKS;
    if (new_cap <= kstack_capacity) return 0;

    alloc_cap = (uint64_t)new_cap + 1;
    new_used = (uint8_t *)kmalloc(alloc_cap * sizeof(uint8_t));
    new_mapped = (uint8_t *)kmalloc(alloc_cap * sizeof(uint8_t));
    new_phys = kmalloc(alloc_cap * KSTACK_USABLE_PAGES * sizeof(uint64_t));
    if (!new_used || !new_mapped || !new_phys) {
        if (new_used) kfree(new_used);
        if (new_mapped) kfree(new_mapped);
        if (new_phys) kfree(new_phys);
        return -1;
    }

    if (kstack_capacity > 0) {
        memcpy(new_used, slot_used, kstack_capacity * sizeof(uint8_t));
        memcpy(new_mapped, slot_bottom_mapped, kstack_capacity * sizeof(uint8_t));
        memcpy(new_phys, slot_page_phys, kstack_capacity * KSTACK_USABLE_PAGES * sizeof(uint64_t));
        kfree(slot_used);
        kfree(slot_bottom_mapped);
        kfree(slot_page_phys);
    }

    memset(new_used + kstack_capacity, 0, (alloc_cap - (uint64_t)kstack_capacity) * sizeof(uint8_t));
    memset(new_mapped + kstack_capacity, 0, (alloc_cap - (uint64_t)kstack_capacity) * sizeof(uint8_t));
    memset(new_phys + kstack_capacity, 0, (alloc_cap - (uint64_t)kstack_capacity) * KSTACK_USABLE_PAGES * sizeof(uint64_t));

    slot_used = new_used;
    slot_bottom_mapped = new_mapped;
    slot_page_phys = new_phys;
    kstack_capacity = new_cap;
    return 0;
}

void kstack_init(void) {
    kstack_capacity = 0;
    slot_used = NULL;
    slot_bottom_mapped = NULL;
    slot_page_phys = NULL;
    if (kstack_grow(KSTACK_INIT_STACKS) < 0) {
        kernel_panic("kstack_init: alloc failed", NULL);
    }
    kstack_initialized = 1;
}

uint8_t *kstack_alloc(void) {
    int i;
    int p;
    int new_cap;
    void *phys;
    uint64_t page_virt;
    uint64_t base_virt;

    if (!kstack_initialized) return NULL;

    for (i = 0; i < kstack_capacity; i++) {
        if (!slot_used[i]) break;
    }
    if (i >= kstack_capacity) {
        new_cap = kstack_capacity * 2;
        if (new_cap > KSTACK_MAX_STACKS) new_cap = KSTACK_MAX_STACKS;
        if (new_cap <= kstack_capacity) return NULL;
        if (kstack_grow(new_cap) < 0) return NULL;
    }

    p = KSTACK_USABLE_PAGES - 1;
    phys = pmm_alloc_low_page();
    if (!phys) phys = pmm_alloc_page();
    if (!phys) return NULL;

    page_virt = slot_page_addr(i, p);
    vmm_map_page(page_virt, (uint64_t)phys, 0x003);
    memset((void *)page_virt, 0, PAGE_SIZE);
    slot_page_phys[i][p] = (uint64_t)phys;

    slot_used[i] = 1;
    slot_bottom_mapped[i] = 0;

    base_virt = slot_bottom_addr(i);
    return (uint8_t *)base_virt;
}

void kstack_free(uint8_t *base) {
    int slot;
    int p;
    uint64_t expected_base;

    if (!kstack_initialized || !base) return;

    slot = addr_to_slot((uint64_t)base);
    if (slot < 0 || slot >= kstack_capacity || !slot_used[slot]) return;

    expected_base = slot_bottom_addr(slot);
    if ((uint64_t)base != expected_base) return;

    for (p = 0; p < KSTACK_USABLE_PAGES; p++) {
        if (slot_page_phys[slot][p]) {
            vmm_unmap_page(slot_page_addr(slot, p));
            pfa_free(slot_page_phys[slot][p]);
            slot_page_phys[slot][p] = 0;
        }
    }

    slot_bottom_mapped[slot] = 0;
    slot_used[slot] = 0;
}

void kstack_reclaim_unused(void) {
    uint8_t *new_used;
    uint8_t *new_mapped;
    uint64_t (*new_phys)[KSTACK_USABLE_PAGES];
    uint64_t alloc_cap;
    int highest;
    int new_cap;
    int i;

    if (!kstack_initialized) return;
    if (kstack_capacity <= KSTACK_INIT_STACKS) return;

    highest = -1;
    for (i = 0; i < kstack_capacity; i++) {
        if (slot_used[i]) highest = i;
    }

    new_cap = KSTACK_INIT_STACKS;
    while (new_cap <= highest) {
        new_cap *= 2;
    }
    if (new_cap >= kstack_capacity) return;

    alloc_cap = (uint64_t)new_cap + 1;
    new_used = (uint8_t *)kmalloc(alloc_cap * sizeof(uint8_t));
    new_mapped = (uint8_t *)kmalloc(alloc_cap * sizeof(uint8_t));
    new_phys = kmalloc(alloc_cap * KSTACK_USABLE_PAGES * sizeof(uint64_t));
    if (!new_used || !new_mapped || !new_phys) {
        if (new_used) kfree(new_used);
        if (new_mapped) kfree(new_mapped);
        if (new_phys) kfree(new_phys);
        return;
    }

    memcpy(new_used, slot_used, new_cap * sizeof(uint8_t));
    memcpy(new_mapped, slot_bottom_mapped, new_cap * sizeof(uint8_t));
    memcpy(new_phys, slot_page_phys, new_cap * KSTACK_USABLE_PAGES * sizeof(uint64_t));
    memset(new_used + new_cap, 0, sizeof(uint8_t));
    memset(new_mapped + new_cap, 0, sizeof(uint8_t));
    memset(new_phys + new_cap, 0, KSTACK_USABLE_PAGES * sizeof(uint64_t));

    kfree(slot_used);
    kfree(slot_bottom_mapped);
    kfree(slot_page_phys);

    slot_used = new_used;
    slot_bottom_mapped = new_mapped;
    slot_page_phys = new_phys;
    kstack_capacity = new_cap;
}

int kstack_page_fault_handler(uint64_t fault_addr) {
    int slot;
    int page_idx;
    void *phys;
    uint64_t page_virt;
    uint64_t bottom;
    uint64_t guard;

    if (!kstack_initialized) return 0;
    if (fault_addr < KSTACK_REGION_START || fault_addr >= KSTACK_REGION_END) return 0;

    slot = addr_to_slot(fault_addr);
    if (slot < 0 || slot >= kstack_capacity || !slot_used[slot]) return 0;

    guard = slot_guard_addr(slot);
    page_virt = fault_addr & ~(PAGE_SIZE - 1);

    if (page_virt == guard) {
        kernel_panic("KERNEL STACK OVERFLOW", NULL);
        return 0;
    }

    bottom = slot_bottom_addr(slot);
    if (page_virt < bottom || page_virt >= bottom + KSTACK_USABLE_SIZE) return 0;
    page_idx = (int)((page_virt - bottom) / PAGE_SIZE);
    if (slot_page_phys[slot][page_idx]) return 0;

    phys = pmm_alloc_low_page();
    if (!phys) phys = pmm_alloc_page();
    if (!phys) return 0;

    vmm_map_page(page_virt, (uint64_t)phys, 0x003);
    memset((void *)page_virt, 0, PAGE_SIZE);
    slot_page_phys[slot][page_idx] = (uint64_t)phys;
    if (page_idx == 0) slot_bottom_mapped[slot] = 1;

    return 1;
}

int kstack_is_in_region(uint64_t addr) {
    return (addr >= KSTACK_REGION_START && addr < KSTACK_REGION_END);
}
