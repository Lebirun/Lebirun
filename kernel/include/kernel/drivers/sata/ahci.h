#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>
#include <stdbool.h>

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

#define PCI_CLASS_STORAGE       0x01
#define PCI_SUBCLASS_SATA       0x06
#define PCI_PROGIF_AHCI         0x01

#define AHCI_CAP            0x00
#define AHCI_GHC            0x04
#define AHCI_IS             0x08
#define AHCI_PI             0x0C
#define AHCI_VS             0x10
#define AHCI_CCC_CTL        0x14
#define AHCI_CCC_PORTS      0x18
#define AHCI_EM_LOC         0x1C
#define AHCI_EM_CTL         0x20
#define AHCI_CAP2           0x24
#define AHCI_BOHC           0x28

#define AHCI_GHC_HR         (1 << 0)
#define AHCI_GHC_IE         (1 << 1)
#define AHCI_GHC_MRSM       (1 << 2)
#define AHCI_GHC_AE         (1 << 31)

#define AHCI_PORT_BASE      0x100
#define AHCI_PORT_SIZE      0x80

#define AHCI_PxCLB          0x00
#define AHCI_PxCLBU         0x04
#define AHCI_PxFB           0x08
#define AHCI_PxFBU          0x0C
#define AHCI_PxIS           0x10
#define AHCI_PxIE           0x14
#define AHCI_PxCMD          0x18
#define AHCI_PxTFD          0x20
#define AHCI_PxSIG          0x24
#define AHCI_PxSSTS         0x28
#define AHCI_PxSCTL         0x2C
#define AHCI_PxSERR         0x30
#define AHCI_PxSACT         0x34
#define AHCI_PxCI           0x38
#define AHCI_PxSNTF         0x3C
#define AHCI_PxFBS          0x40
#define AHCI_PxDEVSLP       0x44

#define AHCI_PxIS_DHRS      (1 << 0)
#define AHCI_PxIS_PSS       (1 << 1)
#define AHCI_PxIS_DSS       (1 << 2)
#define AHCI_PxIS_SDBS      (1 << 3)
#define AHCI_PxIS_UFS       (1 << 4)
#define AHCI_PxIS_DPS       (1 << 5)
#define AHCI_PxIS_PCS       (1 << 6)
#define AHCI_PxIS_DMPS      (1 << 7)
#define AHCI_PxIS_PRCS      (1 << 22)
#define AHCI_PxIS_IPMS      (1 << 23)
#define AHCI_PxIS_OFS       (1 << 24)
#define AHCI_PxIS_INFS      (1 << 26)
#define AHCI_PxIS_IFS       (1 << 27)
#define AHCI_PxIS_HBDS      (1 << 28)
#define AHCI_PxIS_HBFS      (1 << 29)
#define AHCI_PxIS_TFES      (1 << 30)
#define AHCI_PxIS_CPDS      (1 << 31)

#define AHCI_PxIS_FATAL     (AHCI_PxIS_TFES | AHCI_PxIS_HBFS | AHCI_PxIS_HBDS | AHCI_PxIS_IFS)

#define AHCI_PxCMD_ST       (1 << 0)
#define AHCI_PxCMD_SUD      (1 << 1)
#define AHCI_PxCMD_POD      (1 << 2)
#define AHCI_PxCMD_CLO      (1 << 3)
#define AHCI_PxCMD_FRE      (1 << 4)
#define AHCI_PxCMD_CCS_MASK (0x1F << 8)
#define AHCI_PxCMD_MPSS     (1 << 13)
#define AHCI_PxCMD_FR       (1 << 14)
#define AHCI_PxCMD_CR       (1 << 15)
#define AHCI_PxCMD_CPS      (1 << 16)
#define AHCI_PxCMD_PMA      (1 << 17)
#define AHCI_PxCMD_HPCP     (1 << 18)
#define AHCI_PxCMD_MPSP     (1 << 19)
#define AHCI_PxCMD_CPD      (1 << 20)
#define AHCI_PxCMD_ESP      (1 << 21)
#define AHCI_PxCMD_FBSCP    (1 << 22)
#define AHCI_PxCMD_APSTE    (1 << 23)
#define AHCI_PxCMD_ATAPI    (1 << 24)
#define AHCI_PxCMD_DLAE     (1 << 25)
#define AHCI_PxCMD_ALPE     (1 << 26)
#define AHCI_PxCMD_ASP      (1 << 27)
#define AHCI_PxCMD_ICC_MASK (0xF << 28)

#define AHCI_PxSSTS_DET_MASK    0x0F
#define AHCI_PxSSTS_DET_NONE    0x00
#define AHCI_PxSSTS_DET_PRESENT 0x01
#define AHCI_PxSSTS_DET_READY   0x03
#define AHCI_PxSSTS_DET_OFFLINE 0x04

#define AHCI_PxSSTS_IPM_SHIFT   8
#define AHCI_PxSSTS_IPM_MASK    (0x0F << AHCI_PxSSTS_IPM_SHIFT)
#define AHCI_PxSSTS_IPM_ACTIVE  0x01

#define AHCI_PxSERR_DIAG_X      (1 << 26)

#define SATA_SIG_ATA        0x00000101
#define SATA_SIG_ATAPI      0xEB140101
#define SATA_SIG_SEMB       0xC33C0101
#define SATA_SIG_PM         0x96690101

#define FIS_TYPE_REG_H2D    0x27
#define FIS_TYPE_REG_D2H    0x34
#define FIS_TYPE_DMA_ACT    0x39
#define FIS_TYPE_DMA_SETUP  0x41
#define FIS_TYPE_DATA       0x46
#define FIS_TYPE_BIST       0x58
#define FIS_TYPE_PIO_SETUP  0x5F
#define FIS_TYPE_DEV_BITS   0xA1

#define ATA_CMD_READ_DMA_EX     0x25
#define ATA_CMD_WRITE_DMA_EX    0x35
#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_FLUSH           0xE7
#define ATA_CMD_FLUSH_EXT       0xEA
#define ATA_CMD_SMART           0xB0

#define ATA_SMART_READ_DATA     0xD0
#define ATA_SMART_READ_THRESH   0xD1
#define ATA_SMART_ENABLE        0xD8
#define ATA_SMART_DISABLE       0xD9
#define ATA_SMART_STATUS        0xDA
#define ATA_SMART_LBA_MID       0x4F
#define ATA_SMART_LBA_HI        0xC2

#define ATA_SR_BSY      0x80
#define ATA_SR_DRDY     0x40
#define ATA_SR_DF       0x20
#define ATA_SR_DSC      0x10
#define ATA_SR_DRQ      0x08
#define ATA_SR_CORR     0x04
#define ATA_SR_IDX      0x02
#define ATA_SR_ERR      0x01

#define AHCI_SECTOR_SIZE    512
#define AHCI_MAX_PORTS      4
#define AHCI_CMD_LIST_SIZE  1024
#define AHCI_CMD_SLOTS      8
#define AHCI_FIS_SIZE       256
#define AHCI_CMD_TABLE_SIZE 256
#define AHCI_PRDT_ENTRIES   8
#define AHCI_CMD_QUEUE_SIZE 32

typedef struct {
    uint8_t fis_type;
    uint8_t pmport:4;
    uint8_t reserved0:3;
    uint8_t c:1;
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t featureh;
    uint8_t countl;
    uint8_t counth;
    uint8_t icc;
    uint8_t control;
    uint8_t reserved1[4];
} __attribute__((packed)) fis_reg_h2d_t;

typedef struct {
    uint8_t fis_type;
    uint8_t pmport:4;
    uint8_t reserved0:2;
    uint8_t i:1;
    uint8_t reserved1:1;
    uint8_t status;
    uint8_t error;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t reserved2;
    uint8_t countl;
    uint8_t counth;
    uint8_t reserved3[6];
} __attribute__((packed)) fis_reg_d2h_t;

typedef struct {
    uint8_t fis_type;
    uint8_t pmport:4;
    uint8_t reserved0:4;
    uint8_t reserved1[2];
    uint64_t data[1];
} __attribute__((packed)) fis_data_t;

typedef struct {
    uint8_t fis_type;
    uint8_t pmport:4;
    uint8_t reserved0:1;
    uint8_t d:1;
    uint8_t i:1;
    uint8_t reserved1:1;
    uint8_t status;
    uint8_t error;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t reserved2;
    uint8_t countl;
    uint8_t counth;
    uint8_t reserved3;
    uint8_t e_status;
    uint16_t tc;
    uint8_t reserved4[2];
} __attribute__((packed)) fis_pio_setup_t;

typedef struct {
    uint8_t fis_type;
    uint8_t pmport:4;
    uint8_t reserved0:1;
    uint8_t d:1;
    uint8_t i:1;
    uint8_t a:1;
    uint8_t reserved1[2];
    uint64_t dma_buffer_id;
    uint64_t reserved2;
    uint64_t dma_buf_offset;
    uint64_t transfer_count;
    uint64_t reserved3;
} __attribute__((packed)) fis_dma_setup_t;

typedef struct {
    fis_dma_setup_t dsfis;
    uint8_t pad0[4];
    fis_pio_setup_t psfis;
    uint8_t pad1[12];
    fis_reg_d2h_t rfis;
    uint8_t pad2[4];
    uint8_t sdbfis[8];
    uint8_t ufis[64];
    uint8_t reserved[96];
} __attribute__((packed)) hba_fis_t;

typedef struct {
    uint8_t cfl:5;
    uint8_t a:1;
    uint8_t w:1;
    uint8_t p:1;
    uint8_t r:1;
    uint8_t b:1;
    uint8_t c:1;
    uint8_t reserved0:1;
    uint8_t pmp:4;
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved1[4];
} __attribute__((packed)) hba_cmd_header_t;

typedef struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved0;
    uint32_t dbc:22;
    uint32_t reserved1:9;
    uint32_t i:1;
} __attribute__((packed)) hba_prdt_entry_t;

typedef struct {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    hba_prdt_entry_t prdt[AHCI_PRDT_ENTRIES];
} __attribute__((packed)) hba_cmd_table_t;

typedef enum {
    AHCI_DEV_NULL,
    AHCI_DEV_SATA,
    AHCI_DEV_SATAPI,
    AHCI_DEV_SEMB,
    AHCI_DEV_PM,
} ahci_dev_type_t;

typedef enum {
    AHCI_CMD_STATE_FREE,
    AHCI_CMD_STATE_PENDING,
    AHCI_CMD_STATE_ACTIVE,
    AHCI_CMD_STATE_COMPLETE,
    AHCI_CMD_STATE_ERROR,
} ahci_cmd_state_t;

typedef enum {
    AHCI_ERR_NONE = 0,
    AHCI_ERR_TIMEOUT,
    AHCI_ERR_TASKFILE,
    AHCI_ERR_INTERFACE,
    AHCI_ERR_DMA,
    AHCI_ERR_DEVICE,
    AHCI_ERR_RESET_FAIL,
} ahci_error_t;

struct ahci_port;

typedef void (*ahci_callback_t)(struct ahci_port *port, int slot, int status, void *context);

typedef struct {
    ahci_cmd_state_t state;
    uint8_t command;
    uint64_t lba;
    uint64_t count;
    void *buffer;
    uint64_t buf_phys;
    uint64_t buf_pages;
    ahci_callback_t callback;
    void *callback_ctx;
    volatile int result;
    volatile bool completed;
} ahci_cmd_request_t;

typedef struct ahci_port {
    bool present;
    ahci_dev_type_t type;
    uint64_t port_num;
    uint64_t hba_base;
    uint64_t port_base;
    hba_cmd_header_t *cmd_list;
    hba_fis_t *fis;
    hba_cmd_table_t *cmd_table;
    uint64_t cmd_list_phys;
    uint64_t fis_phys;
    uint64_t cmd_table_phys;
    uint64_t sector_count;
    char model[41];
    char serial[21];
    
    volatile uint64_t cmd_issued;
    volatile uint64_t cmd_running;
    ahci_cmd_request_t requests[AHCI_CMD_SLOTS];
    
    volatile uint64_t error_count;
    volatile ahci_error_t last_error;
    volatile uint64_t last_serr;
    volatile uint64_t last_tfd;
    bool use_irq;
} ahci_port_t;

typedef struct {
    bool initialized;
    uint64_t pci_bus;
    uint64_t pci_slot;
    uint64_t pci_func;
    uint64_t abar;
    uint64_t abar_virt;
    uint64_t version;
    uint64_t ports_impl;
    uint64_t num_ports;
    uint64_t num_cmd_slots;
    uint8_t irq;
    bool irq_enabled;
    ahci_port_t ports[AHCI_MAX_PORTS];
} ahci_controller_t;

typedef struct {
    uint8_t attr_id;
    uint16_t flags;
    uint8_t current;
    uint8_t worst;
    uint8_t raw[6];
    uint8_t reserved;
} __attribute__((packed)) smart_attr_t;

typedef struct {
    uint16_t version;
    smart_attr_t attrs[30];
    uint8_t offline_status;
    uint8_t self_test_status;
    uint16_t offline_time;
    uint8_t vendor_specific;
    uint8_t offline_capability;
    uint16_t smart_capability;
    uint8_t error_log_capability;
    uint8_t vendor_specific2;
    uint8_t short_test_time;
    uint8_t ext_test_time;
    uint8_t conveyance_test_time;
    uint16_t ext_test_time_word;
    uint8_t reserved[9];
    uint8_t vendor_specific3[125];
    uint8_t checksum;
} __attribute__((packed)) smart_data_t;

int ahci_init(void);
int ahci_probe(void);
int ahci_port_init(ahci_port_t *port);
int ahci_read_sectors(ahci_port_t *port, uint64_t lba, uint64_t count, void *buffer);
int ahci_write_sectors(ahci_port_t *port, uint64_t lba, uint64_t count, const void *buffer);
int ahci_identify(ahci_port_t *port);
int ahci_flush(ahci_port_t *port);
ahci_controller_t *ahci_get_controller(void);
ahci_port_t *ahci_get_port(uint64_t index);
void ahci_debug_info(void);
int ahci_test_rw(void);

int ahci_read_async(ahci_port_t *port, uint64_t lba, uint64_t count, 
                    void *buffer, ahci_callback_t callback, void *ctx);
int ahci_write_async(ahci_port_t *port, uint64_t lba, uint64_t count,
                     const void *buffer, ahci_callback_t callback, void *ctx);
void ahci_poll_completion(ahci_port_t *port);

int ahci_port_reset(ahci_port_t *port);
int ahci_port_recover(ahci_port_t *port);
ahci_error_t ahci_get_last_error(ahci_port_t *port);
const char *ahci_error_string(ahci_error_t err);

int ahci_smart_enable(ahci_port_t *port);
int ahci_smart_disable(ahci_port_t *port);
int ahci_smart_read_data(ahci_port_t *port, smart_data_t *data);
int ahci_smart_get_status(ahci_port_t *port);
void ahci_smart_print(smart_data_t *data);

void ahci_irq_handler(void *regs);
void ahci_enable_interrupts(void);
void ahci_disable_interrupts(void);

#endif
