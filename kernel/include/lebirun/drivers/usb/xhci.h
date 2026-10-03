#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

#define PCI_CLASS_SERIAL_BUS    0x0C
#define PCI_SUBCLASS_USB        0x03
#define PCI_PROGIF_XHCI         0x30

#define XHCI_CAP_CAPLENGTH  0x00
#define XHCI_CAP_VERSION    0x02
#define XHCI_CAP_HCSP1      0x04
#define XHCI_CAP_HCSP2      0x1C
#define XHCI_CAP_HCCP1      0x10
#define XHCI_CAP_DBOFF      0x14
#define XHCI_CAP_RTSOFF     0x18

#define XHCI_OP_USBCMD      0x00
#define XHCI_OP_USBSTS      0x04
#define XHCI_OP_DNCTRL      0x14
#define XHCI_OP_CRCR        0x18
#define XHCI_OP_DCBAAP      0x30
#define XHCI_OP_CONFIG      0x38
#define XHCI_OP_PORTSC      0x400
#define XHCI_PORTSC_STRIDE  0x10

#define XHCI_USBCMD_RS      (1u << 0)
#define XHCI_USBCMD_HCRST   (1u << 1)
#define XHCI_USBCMD_INTE    (1u << 2)

#define XHCI_USBSTS_HALTED  (1u << 0)
#define XHCI_USBSTS_CNR     (1u << 11)

#define XHCI_PORTSC_CCS     (1u << 0)
#define XHCI_PORTSC_PED     (1u << 1)
#define XHCI_PORTSC_PR      (1u << 4)
#define XHCI_PORTSC_PP      (1u << 9)
#define XHCI_PORTSC_SPEED_SHIFT 10
#define XHCI_PORTSC_SPEED_MASK  0xFu
#define XHCI_PORTSC_PRC     (1u << 21)

#define XHCI_RT_IMAN        0x00
#define XHCI_RT_IMOD        0x04
#define XHCI_RT_ERSTSZ      0x08
#define XHCI_RT_ERSTBA      0x10
#define XHCI_RT_ERDP        0x18
#define XHCI_RT_IR_SET_SIZE 0x20

#define XHCI_TRB_TYPE_LINK  6u
#define XHCI_LINK_TC        (1u << 1)

#define XHCI_EXT_CAP_USBLEGSUP 1u
#define XHCI_LEGSUP_BIOS_OWNED (1u << 16)
#define XHCI_LEGSUP_OS_OWNED   (1u << 24)

int xhci_init(void);

#endif
