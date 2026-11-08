#include <lebirun/pci.h>
#include <lebirun/mem_map.h>

static uint16_t *pci_bdfs;
static uint32_t pci_total;

static uint32_t pci_read(uint32_t bdf, uint8_t off) {
    uint32_t v;

    __asm__ volatile ("outl %0, %1" : : "a"(0x80000000u | bdf << 8 | off),
                      "Nd"((uint16_t)0xCF8));
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)0xCFC));
    return v;
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
