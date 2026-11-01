#include <lebirun/hpet.h>
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <stdint.h>

#define HPET_VIRT_BASE (KERNEL_VMA + 0x37000000ULL)
#define HPET_CAP     (0x000 / 8)
#define HPET_CONF    (0x010 / 8)
#define HPET_COUNTER (0x0F0 / 8)

extern void acpi_read_phys(uint64_t phys_addr, void *buf, uint64_t len);

static volatile uint64_t *hpet_base;
static uint64_t hpet_period_fs;
static uint64_t hpet_last;
static int hpet_wide;

void KERNEL_INIT hpet_init(uint64_t table_phys) {
    uint8_t gas[12];
    uint64_t phys;
    uint64_t cap;
    uint64_t conf;
    uint32_t spins;

    if (hpet_base) return;
    acpi_read_phys(table_phys + 40, gas, sizeof(gas));
    phys = *(uint64_t *)(gas + 4);
    if (gas[0] != 0 || !phys) return;

    vmm_map_page(HPET_VIRT_BASE, phys & ~(PAGE_SIZE - 1), 0x013);
    hpet_base = (volatile uint64_t *)(HPET_VIRT_BASE + (phys & (PAGE_SIZE - 1)));
    cap = hpet_base[HPET_CAP];
    if (!(cap >> 32) || (cap >> 32) > 100000000ULL) {
        hpet_base = NULL;
        return;
    }

    conf = hpet_base[HPET_CONF] & ~3ULL;
    hpet_base[HPET_CONF] = conf;
    hpet_base[HPET_COUNTER] = 0;
    hpet_base[HPET_CONF] = conf | 1;
    for (spins = 0; spins < 1000000 && !hpet_base[HPET_COUNTER]; spins++)
        __asm__ volatile ("pause");
    if (!hpet_base[HPET_COUNTER]) {
        hpet_base = NULL;
        return;
    }

    hpet_wide = (cap >> 13) & 1;
    __atomic_store_n(&hpet_period_fs, cap >> 32, __ATOMIC_RELEASE);
    KERNEL_INIT_LOG("HPET: %llu Hz, %u-bit, %u timers\n",
                    (unsigned long long)(1000000000000000ULL / (cap >> 32)),
                    hpet_wide ? 64 : 32, (unsigned)((cap >> 8) & 0x1F) + 1);
}

int hpet_available(void) {
    return __atomic_load_n(&hpet_period_fs, __ATOMIC_ACQUIRE) != 0;
}

uint64_t hpet_get_ns(void) {
    uint64_t now;
    uint64_t last;

    if (!hpet_available()) return 0;
    now = hpet_base[HPET_COUNTER];
    if (!hpet_wide) {
        last = __atomic_load_n(&hpet_last, __ATOMIC_RELAXED);
        now = (uint32_t)now | (last & ~0xFFFFFFFFULL);
        if (now < last) now += 1ULL << 32;
        __atomic_store_n(&hpet_last, now, __ATOMIC_RELAXED);
    }
    return (uint64_t)((__uint128_t)now * hpet_period_fs / 1000000ULL);
}
