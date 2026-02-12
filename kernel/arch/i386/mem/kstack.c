#include <kernel/kstack.h>
#include <kernel/mem_map.h>
#include <kernel/panic.h>
#include <kernel/common.h>
#include <string.h>
#include <stdio.h>

extern uint32_t pae_enabled;
extern void vmm_map_page(uint32_t virt_addr, uint32_t phys_addr, uint32_t flags);
extern void vmm_unmap_page(uint32_t virt_addr);
extern void *pmm_alloc_page(void);
extern void *pmm_alloc_low_page(void);
extern void pfa_free(uint32_t phys_addr);

static uint8_t slot_used[KSTACK_MAX_STACKS];
static uint8_t slot_bottom_mapped[KSTACK_MAX_STACKS];
static uint32_t slot_top_phys[KSTACK_MAX_STACKS];
static uint32_t slot_bottom_phys[KSTACK_MAX_STACKS];
static int kstack_initialized = 0;

static uint32_t slot_guard_addr(int slot) {
    return KSTACK_REGION_START + (uint32_t)slot * KSTACK_SLOT_SIZE;
}

static uint32_t slot_bottom_addr(int slot) {
    return KSTACK_REGION_START + (uint32_t)slot * KSTACK_SLOT_SIZE + PAGE_SIZE;
}

static uint32_t slot_top_addr(int slot) {
    return KSTACK_REGION_START + (uint32_t)slot * KSTACK_SLOT_SIZE + 2 * PAGE_SIZE;
}

static int addr_to_slot(uint32_t addr) {
    uint32_t offset;
    if (addr < KSTACK_REGION_START || addr >= KSTACK_REGION_END) return -1;
    offset = addr - KSTACK_REGION_START;
    return (int)(offset / KSTACK_SLOT_SIZE);
}

void kstack_init(void) {
    memset(slot_used, 0, sizeof(slot_used));
    memset(slot_bottom_mapped, 0, sizeof(slot_bottom_mapped));
    memset(slot_top_phys, 0, sizeof(slot_top_phys));
    memset(slot_bottom_phys, 0, sizeof(slot_bottom_phys));
    kstack_initialized = 1;
}

uint8_t *kstack_alloc(void) {
    int i;
    void *phys;
    uint32_t top_virt;
    uint32_t base_virt;

    if (!kstack_initialized) return NULL;

    for (i = 0; i < KSTACK_MAX_STACKS; i++) {
        if (!slot_used[i]) break;
    }
    if (i >= KSTACK_MAX_STACKS) return NULL;

    phys = pmm_alloc_low_page();
    if (!phys) phys = pmm_alloc_page();
    if (!phys) return NULL;

    top_virt = slot_top_addr(i);
    vmm_map_page(top_virt, (uint32_t)phys, 0x003);
    memset((void *)top_virt, 0, PAGE_SIZE);

    slot_used[i] = 1;
    slot_bottom_mapped[i] = 0;
    slot_top_phys[i] = (uint32_t)phys;
    slot_bottom_phys[i] = 0;

    base_virt = slot_bottom_addr(i);
    return (uint8_t *)base_virt;
}

void kstack_free(uint8_t *base) {
    int slot;
    uint32_t expected_base;

    if (!kstack_initialized || !base) return;

    slot = addr_to_slot((uint32_t)base);
    if (slot < 0 || !slot_used[slot]) return;

    expected_base = slot_bottom_addr(slot);
    if ((uint32_t)base != expected_base) return;

    if (slot_top_phys[slot]) {
        vmm_unmap_page(slot_top_addr(slot));
        pfa_free(slot_top_phys[slot]);
        slot_top_phys[slot] = 0;
    }

    if (slot_bottom_mapped[slot] && slot_bottom_phys[slot]) {
        vmm_unmap_page(slot_bottom_addr(slot));
        pfa_free(slot_bottom_phys[slot]);
        slot_bottom_phys[slot] = 0;
    }

    slot_bottom_mapped[slot] = 0;
    slot_used[slot] = 0;
}

int kstack_page_fault_handler(uint32_t fault_addr) {
    int slot;
    uint32_t page_virt;
    uint32_t guard;
    uint32_t bottom;
    void *phys;

    if (!kstack_initialized) return 0;
    if (fault_addr < KSTACK_REGION_START || fault_addr >= KSTACK_REGION_END) return 0;

    slot = addr_to_slot(fault_addr);
    if (slot < 0 || !slot_used[slot]) return 0;

    guard = slot_guard_addr(slot);
    bottom = slot_bottom_addr(slot);
    page_virt = fault_addr & ~(PAGE_SIZE - 1);

    if (page_virt == guard) {
        printf("KERNEL STACK OVERFLOW: task hit guard page at 0x%08X (slot %d)\n", fault_addr, slot);
        return 0;
    }

    if (page_virt == bottom && !slot_bottom_mapped[slot]) {
        phys = pmm_alloc_low_page();
        if (!phys) phys = pmm_alloc_page();
        if (!phys) {
            printf("kstack: failed to alloc bottom page for slot %d\n", slot);
            return 0;
        }

        vmm_map_page(bottom, (uint32_t)phys, 0x003);
        memset((void *)bottom, 0, PAGE_SIZE);

        slot_bottom_mapped[slot] = 1;
        slot_bottom_phys[slot] = (uint32_t)phys;
        return 1;
    }

    return 0;
}

int kstack_is_in_region(uint32_t addr) {
    return (addr >= KSTACK_REGION_START && addr < KSTACK_REGION_END);
}
