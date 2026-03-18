#include "syscall_defs.h"

static int sys_sata_test(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    return ahci_test_rw();
}

static int sys_sata_info(int unused1, const char *unused2, int unused3) {
    (void)unused1; (void)unused2; (void)unused3;
    ahci_controller_t *ctrl = ahci_get_controller();
    if (!ctrl || !ctrl->initialized) {
        printf("SATA: No AHCI controller initialized\n");
        return -1;
    }
    ahci_debug_info();
    return (int)ctrl->num_ports;
}

static int sys_sata_smart(int port_num, const char *unused2, int unused3) {
    (void)unused2; (void)unused3;
    ahci_port_t *port = ahci_get_port((uint64_t)port_num);
    if (!port) {
        printf("SMART: No SATA drive on port %d\n", port_num);
        return -1;
    }
    
    int status = ahci_smart_get_status(port);
    if (status < 0) {
        printf("SMART: Failed to get status (enabling SMART...)\n");
        if (ahci_smart_enable(port) < 0) {
            printf("SMART: Failed to enable\n");
            return -1;
        }
        status = ahci_smart_get_status(port);
    }
    
    if (status == 0) {
        printf("SMART: Drive health OK\n");
    } else if (status == 1) {
        printf("SMART: Drive health WARNING - failure predicted!\n");
    }
    
    smart_data_t data;
    if (ahci_smart_read_data(port, &data) == 0) {
        ahci_smart_print(&data);
    }
    
    return status;
}

static int sys_sata_irq(int enable, const char *unused2, int unused3) {
    (void)unused2; (void)unused3;
    if (enable) {
        ahci_enable_interrupts();
    } else {
        ahci_disable_interrupts();
    }
    return 0;
}

static int sys_blockdev_rescan(int devname_ptr, const char *unused2, int unused3) {
    (void)unused2; (void)unused3;
    return devfs_rescan_partitions((const char *)(uintptr_t)devname_ptr);
}

void syscalls_sata_init(void) {
    syscall_table[SYSCALL_SATA_TEST] = sys_sata_test;
    syscall_table[SYSCALL_SATA_INFO] = sys_sata_info;
    syscall_table[SYSCALL_SATA_SMART] = sys_sata_smart;
    syscall_table[SYSCALL_SATA_IRQ] = sys_sata_irq;
    syscall_table[SYSCALL_BLOCKDEV_RESCAN] = sys_blockdev_rescan;
}
