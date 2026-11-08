#ifndef LEBIRUN_PCI_H
#define LEBIRUN_PCI_H

#include <stdint.h>

#define PCI_BUS(bdf) ((uint8_t)((bdf) >> 8))
#define PCI_SLOT(bdf) ((uint8_t)(((bdf) >> 3) & 31))
#define PCI_FUNC(bdf) ((uint8_t)((bdf) & 7))

int pci_get(uint32_t index, uint32_t *bdf);

#endif
