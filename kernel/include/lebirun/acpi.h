#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

typedef void (*acpi_res_fn)(const uint8_t *res, uint32_t len, void *ctx);

void acpi_read_phys(uint64_t phys_addr, void *buf, uint64_t len);
uint64_t acpi_find_table(const char *sig, uint64_t index);
void acpi_scan_devices(const char *const *ids, acpi_res_fn fn, void *ctx);

#endif
