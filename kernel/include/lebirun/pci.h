#ifndef LEBIRUN_PCI_H
#define LEBIRUN_PCI_H

#include <stdint.h>

#define PCI_BUS(bdf) ((uint8_t)((bdf) >> 8))
#define PCI_SLOT(bdf) ((uint8_t)(((bdf) >> 3) & 31))
#define PCI_FUNC(bdf) ((uint8_t)((bdf) & 7))

int pci_get(uint32_t index, uint32_t *bdf);
uint32_t pci_cfg_read(uint32_t bdf, uint8_t off);
void pci_cfg_write(uint32_t bdf, uint8_t off, uint32_t value);
uint8_t pci_find_cap(uint32_t bdf, uint8_t id);
int pci_msi_enable(uint32_t bdf, uint8_t vector);
int pci_msix_table(uint32_t bdf, uint8_t *bir, uint32_t *offset);
void pci_msix_set(volatile uint32_t *table, uint32_t entry, uint8_t vector);
void pci_msix_enable(uint32_t bdf);

#endif
