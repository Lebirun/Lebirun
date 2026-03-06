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
static uint32_t slot_page_phys[KSTACK_MAX_STACKS][KSTACK_USABLE_PAGES];
static int kstack_initialized = 0;

#define slot_top_phys(s)    slot_page_phys[s][KSTACK_USABLE_PAGES - 1]
#define slot_bottom_phys(s) slot_page_phys[s][0]

static uint32_t slot_guard_addr(int slot) {
    return KSTACK_REGION_START + (uint32_t)slot * KSTACK_SLOT_SIZE;
}

static uint32_t slot_bottom_addr(int slot) {
    return KSTACK_REGION_START + (uint32_t)slot * KSTACK_SLOT_SIZE + PAGE_SIZE;
}

static uint32_t slot_page_addr(int slot, int page_idx) {
    return KSTACK_REGION_START + (uint32_t)slot * KSTACK_SLOT_SIZE + (1 + page_idx) * PAGE_SIZE;
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
    memset(slot_page_phys, 0, sizeof(slot_page_phys));
    kstack_initialized = 1;
}

uint8_t *kstack_alloc(void) {
    int i;
    int p;
    void *phys;
    uint32_t page_virt;
    uint32_t base_virt;

    if (!kstack_initialized) return NULL;

    for (i = 0; i < KSTACK_MAX_STACKS; i++) {
        if (!slot_used[i]) break;
    }
    if (i >= KSTACK_MAX_STACKS) return NULL;

    for (p = 0; p < KSTACK_USABLE_PAGES; p++) {
        phys = pmm_alloc_low_page();
        if (!phys) phys = pmm_alloc_page();
        if (!phys) {
            int k;
            for (k = 0; k < p; k++) {
                vmm_unmap_page(slot_page_addr(i, k));
                pfa_free(slot_page_phys[i][k]);
                slot_page_phys[i][k] = 0;
            }
            return NULL;
        }

        page_virt = slot_page_addr(i, p);
        vmm_map_page(page_virt, (uint32_t)phys, 0x003);
        memset((void *)page_virt, 0, PAGE_SIZE);
        slot_page_phys[i][p] = (uint32_t)phys;
    }

    slot_used[i] = 1;
    slot_bottom_mapped[i] = 1;

    base_virt = slot_bottom_addr(i);
    return (uint8_t *)base_virt;
}

void kstack_free(uint8_t *base) {
    int slot;
    int p;
    uint32_t expected_base;

    if (!kstack_initialized || !base) return;

    slot = addr_to_slot((uint32_t)base);
    if (slot < 0 || !slot_used[slot]) return;

    expected_base = slot_bottom_addr(slot);
    if ((uint32_t)base != expected_base) return;

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

int kstack_page_fault_handler(uint32_t fault_addr) {
    int slot;
    uint32_t page_virt;
    uint32_t guard;

    if (!kstack_initialized) return 0;
    if (fault_addr < KSTACK_REGION_START || fault_addr >= KSTACK_REGION_END) return 0;

    slot = addr_to_slot(fault_addr);
    if (slot < 0 || !slot_used[slot]) return 0;

    guard = slot_guard_addr(slot);
    page_virt = fault_addr & ~(PAGE_SIZE - 1);

    if (page_virt == guard) {
        kernel_panic("KERNEL STACK OVERFLOW", NULL);
        return 0;
    }

    return 0;
}

int kstack_is_in_region(uint32_t addr) {
    return (addr >= KSTACK_REGION_START && addr < KSTACK_REGION_END);
}
