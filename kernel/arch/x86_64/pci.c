#include <lebirun/pci.h>
#include <lebirun/mem_map.h>
#include <lebirun/smp.h>

static uint16_t *pci_bdfs;
static uint32_t pci_total;

static uint32_t pci_read(uint32_t bdf, uint8_t off) {
    uint32_t v;

    __asm__ volatile ("outl %0, %1" : : "a"(0x80000000u | bdf << 8 | (off & 0xFC)),
                      "Nd"((uint16_t)0xCF8));
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)0xCFC));
    return v;
}

uint32_t pci_cfg_read(uint32_t bdf, uint8_t off) {
    return pci_read(bdf, off);
}

void pci_cfg_write(uint32_t bdf, uint8_t off, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(0x80000000u | bdf << 8 | (off & 0xFC)),
                      "Nd"((uint16_t)0xCF8));
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"((uint16_t)0xCFC));
}

uint8_t pci_find_cap(uint32_t bdf, uint8_t id) {
    uint8_t off;
    int hops;

    if (!(pci_read(bdf, 0x04) & 0x100000)) return 0;
    off = (uint8_t)(pci_read(bdf, 0x34) & 0xFC);
    for (hops = 0; off && hops < 48; hops++) {
        if ((pci_read(bdf, off) & 0xFF) == id) return off;
        off = (uint8_t)((pci_read(bdf, off) >> 8) & 0xFC);
    }
    return 0;
}

static uint32_t pci_msi_address(void) {
    int i;

    for (i = 0; i < cpu_count; i++)
        if (cpus[i].bsp) return 0xFEE00000u | (uint32_t)cpus[i].lapic_id << 12;
    return 0xFEE00000u;
}

int pci_msi_enable(uint32_t bdf, uint8_t vector) {
    uint32_t ctrl;
    uint8_t cap;

    cap = pci_find_cap(bdf, 0x05);
    if (!cap) return -1;
    ctrl = pci_read(bdf, cap);
    pci_cfg_write(bdf, cap + 4, pci_msi_address());
    if (ctrl & (1u << 23)) {
        pci_cfg_write(bdf, cap + 8, 0);
        pci_cfg_write(bdf, cap + 12, vector);
    } else {
        pci_cfg_write(bdf, cap + 8, vector);
    }
    pci_cfg_write(bdf, cap, (ctrl & ~(7u << 20)) | (1u << 16));
    pci_cfg_write(bdf, 0x04, pci_read(bdf, 0x04) | 0x400);
    return 0;
}

int pci_msix_table(uint32_t bdf, uint8_t *bir, uint32_t *offset) {
    uint32_t table;
    uint8_t cap;

    cap = pci_find_cap(bdf, 0x11);
    if (!cap) return -1;
    table = pci_read(bdf, cap + 4);
    *bir = (uint8_t)(table & 7);
    *offset = table & ~7u;
    return (int)((pci_read(bdf, cap) >> 16) & 0x7FF) + 1;
}

void pci_msix_set(volatile uint32_t *table, uint32_t entry, uint8_t vector) {
    table += entry * 4;
    table[3] = 1;
    table[0] = pci_msi_address();
    table[1] = 0;
    table[2] = vector;
    table[3] = 0;
}

void pci_msix_enable(uint32_t bdf) {
    uint8_t cap;

    cap = pci_find_cap(bdf, 0x11);
    if (!cap) return;
    pci_cfg_write(bdf, 0x04, pci_read(bdf, 0x04) | 0x400);
    pci_cfg_write(bdf, cap, (pci_read(bdf, cap) & ~(1u << 30)) | (1u << 31));
}

static void pci_scan(void) {
    uint16_t *list;
    uint16_t *grown;
    uint32_t cap;
    uint32_t n;
    uint32_t bdf;

    list = NULL;
    cap = 0;
    n = 0;
    for (bdf = 0; bdf < 0x10000; bdf++) {
        if ((pci_read(bdf, 0) & 0xFFFF) == 0xFFFF) {
            if (!(bdf & 7)) bdf |= 7;
            continue;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            grown = krealloc(list, cap * sizeof(*list));
            if (!grown) {
                kfree(list);
                return;
            }
            list = grown;
        }
        list[n++] = (uint16_t)bdf;
        if (!(bdf & 7) && !(pci_read(bdf, 0x0C) & 0x800000)) bdf |= 7;
    }
    if (n && (grown = krealloc(list, n * sizeof(*list)))) list = grown;
    pci_bdfs = list;
    __atomic_store_n(&pci_total, n ? n : UINT32_MAX, __ATOMIC_RELEASE);
}

int pci_get(uint32_t index, uint32_t *bdf) {
    uint32_t total;

    total = __atomic_load_n(&pci_total, __ATOMIC_ACQUIRE);
    if (!total) {
        pci_scan();
        total = __atomic_load_n(&pci_total, __ATOMIC_ACQUIRE);
    }
    if (total == UINT32_MAX || index >= total) return -1;
    *bdf = pci_bdfs[index];
    return 0;
}
