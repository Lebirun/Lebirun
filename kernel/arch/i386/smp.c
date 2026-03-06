#include <kernel/smp.h>
#include <kernel/mem_map.h>
#include <kernel/io.h>
#include <kernel/common.h>
#include <kernel/gdt.h>
#include <kernel/idt.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define LAPIC_VIRT_BASE     0xFEE00000u
#define IOAPIC_VIRT_BASE    0xFEC00000u

#define AP_TRAMPOLINE_PHYS  0x8000u

#define RSDP_SIG "RSD PTR "

volatile uint32_t *lapic_base = NULL;
volatile uint32_t *ioapic_base = NULL;

cpu_info_t cpus[MAX_CPUS];
int cpu_count = 0;
volatile int cpus_booted = 0;

static uint32_t lapic_phys = 0xFEE00000u;
static uint32_t ioapic_phys = 0xFEC00000u;
static uint32_t ioapic_gsi_base = 0;
static uint8_t ioapic_max_redir = 0;

#define MAX_IRQ_OVERRIDES 24

typedef struct {
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
} irq_override_t;

static irq_override_t irq_overrides[MAX_IRQ_OVERRIDES];
static int irq_override_count = 0;

extern uint32_t pae_enabled;

extern uint8_t ap_tramp_start[];
extern uint8_t ap_tramp_end[];
extern volatile uint32_t ap_boot_cr3;
extern volatile uint32_t ap_boot_stack;
extern volatile uint32_t ap_boot_flag;
extern volatile uint32_t ap_pae_flag;
extern volatile uint32_t ap_boot_entry;

static void lapic_write(uint32_t reg, uint32_t val) {
    lapic_base[reg / 4] = val;
}

static uint32_t lapic_read(uint32_t reg) {
    return lapic_base[reg / 4];
}

static void ioapic_write(uint32_t reg, uint32_t val) {
    ioapic_base[0] = reg;
    ioapic_base[4] = val;
}

static uint32_t ioapic_read(uint32_t reg) {
    ioapic_base[0] = reg;
    return ioapic_base[4];
}

#define SMP_TEMP_VIRT  0xF7004000u
#define SMP_TEMP_VIRT2 0xF7005000u

extern void temp_map_raw(uint32_t temp_virt, uint32_t phys_addr);
extern void temp_unmap_raw(uint32_t temp_virt);

static void acpi_read_phys(uint32_t phys_addr, void *buf, uint32_t len) {
    uint32_t page_base;
    uint32_t offset;
    uint32_t chunk;
    uint8_t *dst;
    uint8_t *src;

    dst = (uint8_t *)buf;
    while (len > 0) {
        page_base = phys_addr & ~0xFFFu;
        offset = phys_addr & 0xFFFu;
        chunk = 0x1000 - offset;
        if (chunk > len) chunk = len;

        temp_map_raw(SMP_TEMP_VIRT, page_base);
        src = (uint8_t *)(SMP_TEMP_VIRT + offset);
        memcpy(dst, src, chunk);
        temp_unmap_raw(SMP_TEMP_VIRT);

        dst += chunk;
        phys_addr += chunk;
        len -= chunk;
    }
}

static uint32_t acpi_read32(uint32_t phys_addr) {
    uint32_t val;

    acpi_read_phys(phys_addr, &val, 4);
    return val;
}

static uint8_t *find_rsdp(void) {
    uint8_t *p;
    uint8_t sum;
    int i;

    for (p = (uint8_t *)0xC00E0000u; p < (uint8_t *)0xC0100000u; p += 16) {
        if (memcmp(p, RSDP_SIG, 8) == 0) {
            sum = 0;
            for (i = 0; i < 20; i++) sum += p[i];
            if (sum == 0) return p;
        }
    }
    return NULL;
}

static void parse_madt_phys(uint32_t madt_phys) {
    uint32_t length;
    uint32_t offset;
    uint32_t end_offset;
    uint8_t hdr[2];
    uint8_t entry_buf[12];
    uint8_t type;
    uint8_t entry_len;

    length = acpi_read32(madt_phys + 4);
    lapic_phys = acpi_read32(madt_phys + 36);
    offset = 44;
    end_offset = length;

    while (offset + 2 <= end_offset) {
        acpi_read_phys(madt_phys + offset, hdr, 2);
        type = hdr[0];
        entry_len = hdr[1];

        if (entry_len < 2) break;
        if (offset + entry_len > end_offset) break;

        if (type == 0 && entry_len >= 8 && cpu_count < MAX_CPUS) {
            acpi_read_phys(madt_phys + offset, entry_buf, 8);
            if (*(uint32_t *)(entry_buf + 4) & 0x3) {
                cpus[cpu_count].lapic_id = entry_buf[3];
                cpus[cpu_count].processor_id = entry_buf[2];
                cpus[cpu_count].active = 0;
                cpus[cpu_count].bsp = 0;
                cpu_count++;
            }
        } else if (type == 1 && entry_len >= 12) {
            acpi_read_phys(madt_phys + offset, entry_buf, 12);
            ioapic_phys = *(uint32_t *)(entry_buf + 4);
            ioapic_gsi_base = *(uint32_t *)(entry_buf + 8);
        } else if (type == 2 && entry_len >= 10 && irq_override_count < MAX_IRQ_OVERRIDES) {
            acpi_read_phys(madt_phys + offset, entry_buf, 10);
            irq_overrides[irq_override_count].source = entry_buf[3];
            irq_overrides[irq_override_count].gsi = *(uint32_t *)(entry_buf + 4);
            irq_overrides[irq_override_count].flags = *(uint16_t *)(entry_buf + 8);
            irq_override_count++;
        }

        offset += entry_len;
    }
}

static void find_acpi_tables(void) {
    uint8_t *rsdp;
    uint32_t rsdt_phys;
    uint32_t rsdt_len;
    int num_entries;
    int i;
    uint32_t table_phys;
    uint8_t sig[4];

    rsdp = find_rsdp();
    if (!rsdp) {
        printf("SMP: RSDP not found, assuming single CPU\n");
        cpus[0].lapic_id = 0;
        cpus[0].processor_id = 0;
        cpus[0].bsp = 1;
        cpus[0].active = 1;
        cpu_count = 1;
        return;
    }

    rsdt_phys = *(uint32_t *)(rsdp + 16);

    rsdt_len = acpi_read32(rsdt_phys + 4);
    num_entries = (rsdt_len - 36) / 4;

    for (i = 0; i < num_entries; i++) {
        table_phys = acpi_read32(rsdt_phys + 36 + i * 4);

        acpi_read_phys(table_phys, sig, 4);

        if (memcmp(sig, "APIC", 4) == 0) {
            parse_madt_phys(table_phys);
            return;
        }
    }

    printf("SMP: MADT not found in RSDT\n");
    cpus[0].lapic_id = 0;
    cpus[0].processor_id = 0;
    cpus[0].bsp = 1;
    cpus[0].active = 1;
    cpu_count = 1;
}

void lapic_init(void) {
    uint32_t svr;

    vmm_map_page(LAPIC_VIRT_BASE, lapic_phys, 0x013);
    lapic_base = (volatile uint32_t *)LAPIC_VIRT_BASE;

    svr = lapic_read(LAPIC_REG_SVR);
    svr |= LAPIC_SVR_ENABLE;
    svr = (svr & ~0xFF) | 0xFF;
    lapic_write(LAPIC_REG_SVR, svr);

    lapic_write(LAPIC_REG_TIMER, LAPIC_TIMER_MASKED);
    lapic_write(0x350, 0x10000);
    lapic_write(0x360, 0x10000);
    lapic_write(0x370, 0x10000);

    lapic_write(LAPIC_REG_TPR, 0);
    lapic_write(LAPIC_REG_ESR, 0);
    lapic_write(LAPIC_REG_ESR, 0);
    lapic_write(LAPIC_REG_EOI, 0);
}

void lapic_eoi(void) {
    if (lapic_base) {
        lapic_write(LAPIC_REG_EOI, 0);
    }
}

uint32_t lapic_get_id(void) {
    if (!lapic_base) return 0;
    return lapic_read(LAPIC_REG_ID) >> 24;
}

void lapic_send_ipi(uint32_t apic_id, uint32_t vector) {
    lapic_write(LAPIC_REG_ICR_HI, apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LO, vector);
    while (lapic_read(LAPIC_REG_ICR_LO) & ICR_DELIVERY_STATUS)
        __asm__ volatile ("pause");
}

void ioapic_init(void) {
    uint32_t ver;

    vmm_map_page(IOAPIC_VIRT_BASE, ioapic_phys, 0x013);
    ioapic_base = (volatile uint32_t *)IOAPIC_VIRT_BASE;

    ver = ioapic_read(IOAPIC_REG_VER);
    ioapic_max_redir = (ver >> 16) & 0xFF;

    printf("SMP: IOAPIC at phys 0x%08X, max redirection entries: %u\n",
           ioapic_phys, ioapic_max_redir);
}

static uint32_t irq_to_gsi(uint8_t irq) {
    int i;

    for (i = 0; i < irq_override_count; i++) {
        if (irq_overrides[i].source == irq) {
            return irq_overrides[i].gsi;
        }
    }
    return (uint32_t)irq;
}

void ioapic_route_irq(uint8_t irq, uint8_t vector, uint32_t dest_apic_id) {
    uint32_t redtbl_lo;
    uint32_t redtbl_hi;
    uint32_t reg;
    uint32_t gsi;

    gsi = irq_to_gsi(irq);
    if (gsi > ioapic_max_redir) return;

    reg = IOAPIC_REG_REDTBL + gsi * 2;
    redtbl_lo = vector;
    redtbl_hi = dest_apic_id << 24;
    ioapic_write(reg + 1, redtbl_hi);
    ioapic_write(reg, redtbl_lo);
}

static void pic_disable(void) {
    outb(0xA1, 0xFF);
    outb(0x21, 0xFF);
}

static void delay_ms(uint32_t ms) {
    uint32_t i;
    uint32_t j;
    for (i = 0; i < ms; i++) {
        for (j = 0; j < 10000; j++) {
            __asm__ volatile ("pause");
        }
    }
}

int smp_processor_id(void) {
    uint32_t id;
    int i;

    if (!lapic_base) return 0;
    id = lapic_get_id();
    for (i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == id) return i;
    }
    return 0;
}

cpu_info_t *smp_this_cpu(void) {
    int idx;

    idx = smp_processor_id();
    return &cpus[idx];
}

int smp_is_bsp(void) {
    int idx;

    idx = smp_processor_id();
    return cpus[idx].bsp;
}

void smp_tlb_flush_all(void) {
    uint32_t my_id;
    int i;

    if (!lapic_base || cpu_count <= 1) return;

    my_id = lapic_get_id();
    for (i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == my_id) continue;
        if (!cpus[i].active) continue;
        if (!cpus[i].current_task) continue;
        lapic_send_ipi(cpus[i].lapic_id, IPI_TLB_FLUSH_VECTOR);
    }
}

void ap_main(void);

void ap_main(void) {
    uint32_t id;
    int cpu_idx;
    int i;

    id = lapic_get_id();

    cpu_idx = -1;
    for (i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == id) {
            cpu_idx = i;
            break;
        }
    }

    if (cpu_idx >= 0 && cpus[cpu_idx].gdt && cpus[cpu_idx].tss) {
        gdt_init_ap(cpus[cpu_idx].gdt, cpus[cpu_idx].tss);
    }

    idt_load();
    lapic_init();
    lapic_write(LAPIC_REG_TPR, 0xFF);

    if (cpu_idx >= 0) {
        cpus[cpu_idx].active = 1;
        cpus[cpu_idx].current_task = NULL;
        cpus[cpu_idx].scheduler_lock_depth = 0;
        cpus[cpu_idx].sched_saved_eflags = 0;
        cpus[cpu_idx].schedule_force = 0;
    }

    __sync_fetch_and_add(&cpus_booted, 1);

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void smp_start_aps(void) {
    uint32_t tramp_size;
    uint32_t bsp_id;
    int i;
    volatile int timeout;
    volatile uint32_t *tramp_cr3;
    volatile uint32_t *tramp_stack;
    volatile uint32_t *tramp_flag;
    volatile uint32_t *tramp_pae;
    volatile uint32_t *tramp_entry;

    tramp_size = (uint32_t)(ap_tramp_end - ap_tramp_start);
    if (tramp_size > 0x1000) {
        printf("SMP: trampoline too large (%u bytes)\n", tramp_size);
        return;
    }

    vmm_map_page(AP_TRAMPOLINE_PHYS, AP_TRAMPOLINE_PHYS, 0x003);
    memcpy((void *)AP_TRAMPOLINE_PHYS, ap_tramp_start, tramp_size);

    tramp_cr3   = (volatile uint32_t *)(AP_TRAMPOLINE_PHYS +
                  ((uint8_t *)&ap_boot_cr3 - ap_tramp_start));
    tramp_stack = (volatile uint32_t *)(AP_TRAMPOLINE_PHYS +
                  ((uint8_t *)&ap_boot_stack - ap_tramp_start));
    tramp_flag  = (volatile uint32_t *)(AP_TRAMPOLINE_PHYS +
                  ((uint8_t *)&ap_boot_flag - ap_tramp_start));
    tramp_pae   = (volatile uint32_t *)(AP_TRAMPOLINE_PHYS +
                  ((uint8_t *)&ap_pae_flag - ap_tramp_start));
    tramp_entry = (volatile uint32_t *)(AP_TRAMPOLINE_PHYS +
                  ((uint8_t *)&ap_boot_entry - ap_tramp_start));

    *tramp_entry = (uint32_t)ap_main;

    *tramp_pae = pae_enabled ? 1 : 0;

    bsp_id = lapic_get_id();

    for (i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == bsp_id) {
            cpus[i].bsp = 1;
            cpus[i].active = 1;
            continue;
        }

        cpus[i].kernel_stack = kmalloc_aligned(8192, 16);
        if (!cpus[i].kernel_stack) {
            printf("SMP: failed to allocate stack for CPU %u\n", cpus[i].lapic_id);
            continue;
        }

        cpus[i].gdt = (uint32_t *)kmalloc_aligned(64, 8);
        cpus[i].tss = kmalloc_aligned(104, 8);
        if (!cpus[i].gdt || !cpus[i].tss) {
            printf("SMP: failed to allocate GDT/TSS for CPU %u\n", cpus[i].lapic_id);
            continue;
        }

        *tramp_stack = (uint32_t)cpus[i].kernel_stack + 8192;
        *tramp_cr3 = read_cr3();
        *tramp_flag = 0;

        lapic_send_ipi(cpus[i].lapic_id, ICR_INIT | ICR_LEVEL_ASSERT);
        delay_ms(10);
        lapic_send_ipi(cpus[i].lapic_id, ICR_INIT | ICR_LEVEL_DEASSERT);
        delay_ms(10);

        lapic_send_ipi(cpus[i].lapic_id,
                       ICR_STARTUP | (AP_TRAMPOLINE_PHYS >> 12));
        delay_ms(1);

        if (!*tramp_flag) {
            lapic_send_ipi(cpus[i].lapic_id,
                           ICR_STARTUP | (AP_TRAMPOLINE_PHYS >> 12));
            delay_ms(1);
        }

        timeout = 100000;
        while (!*tramp_flag && --timeout > 0) {
            __asm__ volatile ("pause");
        }

        if (*tramp_flag) {
            printf("SMP: CPU %u (LAPIC ID %u) started\n",
                   i, cpus[i].lapic_id);
        } else {
            printf("SMP: CPU %u (LAPIC ID %u) failed to start\n",
                   i, cpus[i].lapic_id);
        }
    }
}

void lapic_timer_init(uint32_t freq_hz) {
    uint32_t initial;

    lapic_write(LAPIC_REG_TIMER_DCR, 0x03);

    lapic_write(LAPIC_REG_TIMER, LAPIC_TIMER_MASKED | 32);
    lapic_write(LAPIC_REG_TIMER_ICR, 0xFFFFFFFF);

    delay_ms(10);
    initial = 0xFFFFFFFF - lapic_read(LAPIC_REG_TIMER_CCR);

    lapic_write(LAPIC_REG_TIMER,
                LAPIC_TIMER_PERIODIC | 32);
    lapic_write(LAPIC_REG_TIMER_ICR, initial * 100 / freq_hz);

    (void)freq_hz;
}

void smp_init(void) {
    uint32_t bsp_apic_id;
    int i;
    int found;

    printf("[SMP] find_acpi_tables...\n");
    find_acpi_tables();

    if (cpu_count == 0) {
        cpus[0].lapic_id = 0;
        cpus[0].processor_id = 0;
        cpus[0].bsp = 1;
        cpus[0].active = 1;
        cpu_count = 1;
    }

    printf("SMP: Found %d CPU(s)\n", cpu_count);

    printf("[SMP] lapic_init...\n");
    lapic_init();

    bsp_apic_id = lapic_get_id();
    found = 0;
    for (i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == bsp_apic_id) {
            cpus[i].bsp = 1;
            cpus[i].active = 1;
            found = 1;
        }
        printf("  CPU %d: LAPIC ID %u%s\n", i, cpus[i].lapic_id,
               cpus[i].bsp ? " (BSP)" : "");
    }

    if (!found && cpu_count < MAX_CPUS) {
        cpus[cpu_count].lapic_id = bsp_apic_id;
        cpus[cpu_count].processor_id = 0;
        cpus[cpu_count].bsp = 1;
        cpus[cpu_count].active = 1;
        cpu_count++;
    }

    printf("[SMP] ioapic_init...\n");
    ioapic_init();
    printf("[SMP] pic_disable...\n");
    pic_disable();

    ioapic_route_irq(0, 32, bsp_apic_id);
    ioapic_route_irq(1, 33, bsp_apic_id);
    {
        uint32_t gsi0;
        uint32_t gsi1;
        uint32_t gsi_i;
        gsi0 = irq_to_gsi(0);
        gsi1 = irq_to_gsi(1);
        for (i = 2; i < 16; i++) {
            gsi_i = irq_to_gsi((uint8_t)i);
            if (gsi_i == gsi0 || gsi_i == gsi1)
                continue;
            ioapic_route_irq(i, 32 + i, bsp_apic_id);
        }
    }

    if (cpu_count > 1) {
        smp_start_aps();

        {
            int expected_aps;
            volatile int ap_timeout;

            expected_aps = cpu_count - 1;
            ap_timeout = 1000000;
            while (cpus_booted < expected_aps && --ap_timeout > 0) {
                __asm__ volatile ("pause");
            }
            if (cpus_booted < expected_aps) {
                printf("SMP: Warning: only %d of %d APs finished init\n",
                       cpus_booted, expected_aps);
            }
        }
    }

    printf("SMP: %d CPU(s) active\n", cpus_booted + 1);
}
