#ifndef E1000_H
#define E1000_H

#include <stdint.h>
#include <kernel/drivers/net/net_types.h>

#define E1000_VENDOR_ID 0x8086
#define E1000_DEVICE_82540EM 0x100E
#define E1000_DEVICE_82545EM 0x100F
#define E1000_DEVICE_82574L  0x10D3

#define E1000_CTRL     0x0000
#define E1000_STATUS   0x0008
#define E1000_EECD     0x0010
#define E1000_EERD     0x0014
#define E1000_CTRL_EXT 0x0018
#define E1000_ICR      0x00C0
#define E1000_ITR      0x00C4
#define E1000_ICS      0x00C8
#define E1000_IMS      0x00D0
#define E1000_IMC      0x00D8
#define E1000_RCTL     0x0100
#define E1000_TCTL     0x0400
#define E1000_TIPG     0x0410
#define E1000_RDBAL    0x2800
#define E1000_RDBAH    0x2804
#define E1000_RDLEN    0x2808
#define E1000_RDH      0x2810
#define E1000_RDT      0x2818
#define E1000_TDBAL    0x3800
#define E1000_TDBAH    0x3804
#define E1000_TDLEN    0x3808
#define E1000_TDH      0x3810
#define E1000_TDT      0x3818
#define E1000_MTA      0x5200
#define E1000_RAL      0x5400
#define E1000_RAH      0x5404

#define E1000_CTRL_FD     (1 << 0)
#define E1000_CTRL_ASDE   (1 << 5)
#define E1000_CTRL_SLU    (1 << 6)
#define E1000_CTRL_ILOS   (1 << 7)
#define E1000_CTRL_RST    (1 << 26)
#define E1000_CTRL_VME    (1 << 30)
#define E1000_CTRL_PHY_RST (1 << 31)

#define E1000_STATUS_FD   (1 << 0)
#define E1000_STATUS_LU   (1 << 1)
#define E1000_STATUS_TXOFF (1 << 4)
#define E1000_STATUS_SPEED_MASK (3 << 6)
#define E1000_STATUS_SPEED_10   (0 << 6)
#define E1000_STATUS_SPEED_100  (1 << 6)
#define E1000_STATUS_SPEED_1000 (2 << 6)

#define E1000_RCTL_EN     (1 << 1)
#define E1000_RCTL_SBP    (1 << 2)
#define E1000_RCTL_UPE    (1 << 3)
#define E1000_RCTL_MPE    (1 << 4)
#define E1000_RCTL_LPE    (1 << 5)
#define E1000_RCTL_LBM_NONE (0 << 6)
#define E1000_RCTL_LBM_MAC  (1 << 6)
#define E1000_RCTL_RDMTS_HALF (0 << 8)
#define E1000_RCTL_MO_36    (0 << 12)
#define E1000_RCTL_BAM    (1 << 15)
#define E1000_RCTL_BSIZE_2048 (0 << 16)
#define E1000_RCTL_BSIZE_1024 (1 << 16)
#define E1000_RCTL_BSIZE_512  (2 << 16)
#define E1000_RCTL_BSIZE_256  (3 << 16)
#define E1000_RCTL_SECRC  (1 << 26)

#define E1000_TCTL_EN     (1 << 1)
#define E1000_TCTL_PSP    (1 << 3)
#define E1000_TCTL_CT_SHIFT 4
#define E1000_TCTL_COLD_SHIFT 12
#define E1000_TCTL_RTLC   (1 << 24)

#define E1000_ICR_TXDW    (1 << 0)
#define E1000_ICR_TXQE    (1 << 1)
#define E1000_ICR_LSC     (1 << 2)
#define E1000_ICR_RXSEQ   (1 << 3)
#define E1000_ICR_RXDMT0  (1 << 4)
#define E1000_ICR_RXO     (1 << 6)
#define E1000_ICR_RXT0    (1 << 7)

#define E1000_IMS_TXDW    (1 << 0)
#define E1000_IMS_TXQE    (1 << 1)
#define E1000_IMS_LSC     (1 << 2)
#define E1000_IMS_RXSEQ   (1 << 3)
#define E1000_IMS_RXDMT0  (1 << 4)
#define E1000_IMS_RXO     (1 << 6)
#define E1000_IMS_RXT0    (1 << 7)

#define E1000_TIPG_IPGT   10
#define E1000_TIPG_IPGR1  (10 << 10)
#define E1000_TIPG_IPGR2  (10 << 20)

#define E1000_NUM_RX_DESC 64
#define E1000_NUM_TX_DESC 16
#define E1000_RX_BUFFER_SIZE 2048

#define E1000_TXD_CMD_EOP  (1 << 0)
#define E1000_TXD_CMD_IFCS (1 << 1)
#define E1000_TXD_CMD_IC   (1 << 2)
#define E1000_TXD_CMD_RS   (1 << 3)
#define E1000_TXD_CMD_DEXT (1 << 5)
#define E1000_TXD_CMD_VLE  (1 << 6)
#define E1000_TXD_CMD_IDE  (1 << 7)

#define E1000_TXD_STAT_DD  (1 << 0)
#define E1000_TXD_STAT_EC  (1 << 1)
#define E1000_TXD_STAT_LC  (1 << 2)

#define E1000_RXD_STAT_DD  (1 << 0)
#define E1000_RXD_STAT_EOP (1 << 1)

#define E1000_RXD_ERR_CE   (1 << 0)
#define E1000_RXD_ERR_SE   (1 << 1)
#define E1000_RXD_ERR_SEQ  (1 << 2)
#define E1000_RXD_ERR_CXE  (1 << 4)
#define E1000_RXD_ERR_TCPE (1 << 5)
#define E1000_RXD_ERR_IPE  (1 << 6)
#define E1000_RXD_ERR_RXE  (1 << 7)

typedef struct {
    uint64_t buffer_addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct {
    uint64_t buffer_addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

typedef struct {
    uint8_t pci_bus;
    uint8_t pci_slot;
    uint8_t pci_func;
    uint16_t device_id;
    uint32_t bar0;
    uint64_t bar0_virt;
    uint32_t bar_type;
    uint32_t io_base;
    uint8_t irq;
    mac_addr_t mac;
    e1000_rx_desc_t *rx_descs;
    e1000_tx_desc_t *tx_descs;
    uint64_t rx_descs_phys;
    uint64_t tx_descs_phys;
    uint8_t *rx_buffers[E1000_NUM_RX_DESC];
    uint64_t rx_buffers_phys[E1000_NUM_RX_DESC];
    uint8_t *tx_buffers[E1000_NUM_TX_DESC];
    uint64_t tx_buffers_phys[E1000_NUM_TX_DESC];
    uint32_t rx_cur;
    uint32_t tx_cur;
    uint8_t link_up;
    uint8_t initialized;
    uint32_t packets_rx;
    uint32_t packets_tx;
    uint32_t bytes_rx;
    uint32_t bytes_tx;
    uint32_t errors_rx;
    uint32_t errors_tx;
    netif_t *netif;
} e1000_device_t;

int e1000_init(void);
int e1000_probe(void);
void e1000_enable_interrupts(e1000_device_t *dev);
void e1000_disable_interrupts(e1000_device_t *dev);
int e1000_send(netif_t *netif, uint8_t *data, uint64_t len);
int e1000_poll(netif_t *netif);
void e1000_irq_handler(void *regs);
void e1000_read_mac(e1000_device_t *dev);
void e1000_print_status(e1000_device_t *dev);
e1000_device_t *e1000_get_device(void);

#endif
