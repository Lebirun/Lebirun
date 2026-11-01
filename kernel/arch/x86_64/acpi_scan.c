#include <stdint.h>
#include <string.h>
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <lebirun/acpi.h>

#define ACPI_SCAN_VIRT (KERNEL_VMA + 0x37400000ULL)

static const uint8_t *KERNEL_INIT aml_device(const uint8_t *p,
                                             const uint8_t *end) {
    uint32_t n;
    uint32_t len;
    uint32_t i;

    if (end - p < 8 || p[0] != 0x5B || p[1] != 0x82) return NULL;
    n = p[2] >> 6;
    len = n ? p[2] & 0x0F : p[2] & 0x3F;
    for (i = 0; i < n; i++) len |= (uint32_t)p[3 + i] << (4 + 8 * i);
    if (len < n + 5 || len > (uint64_t)(end - p - 2)) return NULL;
    return p + 2 + len;
}

static uint32_t KERNEL_INIT aml_hex(const char *s) {
    uint32_t v;
    char c;

    v = 0;
    while ((c = *s++)) v = (v << 4) | (uint32_t)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    return v;
}

static int KERNEL_INIT aml_id_at(const uint8_t *p, const uint8_t *end,
                                 const char *id) {
    uint64_t n;
    uint32_t e;

    n = strlen(id);
    if (p[0] == 0x0D)
        return (uint64_t)(end - p) > n + 1 && memcmp(p + 1, id, n) == 0 &&
               p[n + 1] == 0;
    if (p[0] != 0x0C || n != 7 || end - p < 5) return 0;
    e = ((uint32_t)(id[0] & 0x1F) << 26) | ((uint32_t)(id[1] & 0x1F) << 21) |
        ((uint32_t)(id[2] & 0x1F) << 16) | aml_hex(id + 3);
    return p[1] == (uint8_t)(e >> 24) && p[2] == (uint8_t)(e >> 16) &&
           p[3] == (uint8_t)(e >> 8) && p[4] == (uint8_t)e;
}

static void KERNEL_INIT aml_scope(const uint8_t *p, const uint8_t *end,
                                  const char *const *ids, acpi_res_fn fn,
                                  void *ctx) {
    const uint8_t *q;
    const uint8_t *c;
    uint32_t len;
    uint32_t i;
    int match;

    match = 0;
    for (q = p; q < end && !match; q++) {
        if ((c = aml_device(q, end))) {
            q = c - 1;
            continue;
        }
        for (i = 0; ids[i] && !match; i++) match = aml_id_at(q, end, ids[i]);
    }
    if (!match) return;
    for (q = p; q + 3 <= end; q++) {
        if ((c = aml_device(q, end))) {
            q = c - 1;
            continue;
        }
        if (*q != 0x86 && *q != 0x87 && *q != 0x8E) continue;
        len = (uint32_t)q[1] | ((uint32_t)q[2] << 8);
        if (len && q + 3 + len <= end) fn(q, len + 3, ctx);
    }
}

static void KERNEL_INIT acpi_scan_table(uint64_t phys, const char *const *ids,
                                        acpi_res_fn fn, void *ctx) {
    uint32_t hdr[2];
    uint64_t off;
    uint64_t pages;
    uint64_t i;
    const uint8_t *t;
    const uint8_t *end;
    const uint8_t *q;
    const uint8_t *dev;

    if (!phys) return;
    acpi_read_phys(phys, hdr, sizeof(hdr));
    if (hdr[1] <= 36) return;
    off = phys & (PAGE_SIZE - 1);
    pages = (off + hdr[1] + PAGE_SIZE - 1) / PAGE_SIZE;
    if (ACPI_SCAN_VIRT + pages * PAGE_SIZE > TEMP_MAP_BASE) return;
    for (i = 0; i < pages; i++)
        vmm_map_page(ACPI_SCAN_VIRT + i * PAGE_SIZE,
                     (phys - off) + i * PAGE_SIZE, VMM_PTE_PRESENT);
    t = (const uint8_t *)(ACPI_SCAN_VIRT + off);
    end = t + hdr[1];
    for (q = t + 36; q < end; q++) {
        if ((dev = aml_device(q, end)))
            aml_scope(q + 3 + (q[2] >> 6), dev, ids, fn, ctx);
    }
    for (i = 0; i < pages; i++) vmm_unmap_page(ACPI_SCAN_VIRT + i * PAGE_SIZE);
}

void KERNEL_INIT acpi_scan_devices(const char *const *ids, acpi_res_fn fn,
                                   void *ctx) {
    uint64_t facp;
    uint64_t dsdt;
    uint64_t i;
    uint32_t len;

    facp = acpi_find_table(KERNEL_INIT_STRING("FACP"), 0);
    if (facp) {
        len = 0;
        dsdt = 0;
        acpi_read_phys(facp + 4, &len, 4);
        if (len >= 148) acpi_read_phys(facp + 140, &dsdt, 8);
        if (!dsdt) acpi_read_phys(facp + 40, &dsdt, 4);
        acpi_scan_table(dsdt, ids, fn, ctx);
    }
    for (i = 0; (dsdt = acpi_find_table(KERNEL_INIT_STRING("SSDT"), i)); i++)
        acpi_scan_table(dsdt, ids, fn, ctx);
}
