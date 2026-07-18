#include <lebirun/smp.h>
#include <lebirun/mem_map.h>
#include <lebirun/io.h>
#include <lebirun/common.h>
#include <lebirun/gdt.h>
#include <lebirun/idt.h>
#include <lebirun/kstack.h>
#include <lebirun/task.h>
#include <lebirun/pit.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define LAPIC_VIRT_BASE     (KERNEL_VMA + 0x3EE00000ULL)
#define IOAPIC_VIRT_BASE    (KERNEL_VMA + 0x3EC00000ULL)

#define AP_TRAMPOLINE_PHYS  0x8000u

#define RSDP_SIG "RSD PTR "

volatile uint32_t *lapic_base = NULL;
volatile uint32_t *ioapic_base = NULL;
uint32_t lapic_timer_reload = 0;

static cpu_info_t cpu_bootstrap[1];
cpu_info_t *cpus = cpu_bootstrap;
int cpu_count = 0;
volatile int cpus_booted = 0;
volatile int smp_percpu_irq_ready = 0;
static volatile int smp_scheduler_ready = 0;
static volatile int smp_gs_ready = 0;
static volatile int smp_tlb_flush_lock = 0;
static volatile int smp_tlb_flush_acks = 0;

_Static_assert(__builtin_offsetof(cpu_info_t, irq_cr3) == 72, "irq cr3 offset");

static uint64_t lapic_phys = 0xFEE00000u;
static uint64_t ioapic_phys = 0xFEC00000u;
static uint64_t ioapic_gsi_base = 0;
static uint8_t ioapic_max_redir = 0;

#define MAX_IRQ_OVERRIDES 24

typedef struct {
    uint8_t source;
    uint64_t gsi;
    uint16_t flags;
} irq_override_t;

static irq_override_t *irq_overrides;
static int irq_override_count = 0;

extern uint8_t ap_tramp_start[];
extern uint8_t ap_tramp_end[];
extern volatile uint64_t ap_boot_cr3;
extern volatile uint64_t ap_boot_stack;
extern volatile uint32_t ap_boot_flag;
extern volatile uint64_t ap_boot_entry;

static int smp_ensure_cpu_capacity(int needed) {
    cpu_info_t *new_cpus;

    if (needed <= 1) return 1;
    if (cpus == cpu_bootstrap) {
        new_cpus = (cpu_info_t *)kmalloc(
            (uint64_t)needed * sizeof(cpu_info_t));
        if (new_cpus)
            memcpy(new_cpus, cpu_bootstrap, sizeof(cpu_bootstrap));
    } else {
        new_cpus = (cpu_info_t *)krealloc(
            cpus, (uint64_t)needed * sizeof(cpu_info_t));
    }
    if (!new_cpus) return 0;
    memset(&new_cpus[needed - 1], 0, sizeof(cpu_info_t));
    cpus = new_cpus;
    return 1;
}

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

#define SMP_TEMP_VIRT  TEMP_SLOT(4)
#define SMP_TEMP_VIRT2 TEMP_SLOT(5)

extern void temp_map_raw(uint64_t temp_virt, uint64_t phys_addr);
extern void temp_unmap_raw(uint64_t temp_virt);

static void acpi_read_phys(uint64_t phys_addr, void *buf, uint64_t len) {
    uint64_t page_base;
    uint64_t offset;
    uint64_t chunk;
    uint8_t *dst;
    uint8_t *src;
    uint64_t saved_flags;

    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");

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

    if (saved_flags & (1 << 9)) __asm__ volatile ("sti" ::: "memory");
}

static uint64_t acpi_read32(uint64_t phys_addr) {
    uint64_t val;

    val = 0;
    acpi_read_phys(phys_addr, &val, 4);
    return val;
}

static uint8_t *find_rsdp(void) {
    uint8_t *p;
    uint8_t sum;
    int i;

    for (p = (uint8_t *)(0xE0000 + KERNEL_VMA); p < (uint8_t *)(0x100000 + KERNEL_VMA); p += 16) {
        if (memcmp(p, RSDP_SIG, 8) == 0) {
            sum = 0;
            for (i = 0; i < 20; i++) sum += p[i];
            if (sum == 0) return p;
        }
    }
    return NULL;
}

static void parse_madt_phys(uint64_t madt_phys) {
    uint64_t length;
    uint64_t offset;
    uint64_t end_offset;
    uint8_t hdr[2];
    uint8_t entry_buf[12];
    uint8_t type;
    uint8_t entry_len;
    uint32_t lapic_flags;
    uint32_t ioapic_addr32;
    uint32_t gsi_base32;
    uint32_t gsi32;
    irq_override_t *new_overrides;

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
            lapic_flags = *(uint32_t *)(entry_buf + 4);
            if ((lapic_flags & 0x3) &&
                smp_ensure_cpu_capacity(cpu_count + 1)) {
                cpus[cpu_count].lapic_id = entry_buf[3];
                cpus[cpu_count].processor_id = entry_buf[2];
                cpus[cpu_count].active = 0;
                cpus[cpu_count].bsp = 0;
                cpu_count++;
            }
        } else if (type == 1 && entry_len >= 12) {
            acpi_read_phys(madt_phys + offset, entry_buf, 12);
            ioapic_addr32 = *(uint32_t *)(entry_buf + 4);
            gsi_base32 = *(uint32_t *)(entry_buf + 8);
            ioapic_phys = (uint64_t)ioapic_addr32;
            ioapic_gsi_base = (uint64_t)gsi_base32;
        } else if (type == 2 && entry_len >= 10 &&
                   irq_override_count < MAX_IRQ_OVERRIDES) {
            new_overrides = (irq_override_t *)krealloc(
                irq_overrides,
                (uint64_t)(irq_override_count + 1) *
                    sizeof(irq_override_t));
            if (new_overrides) {
                irq_overrides = new_overrides;
                acpi_read_phys(madt_phys + offset, entry_buf, 10);
                irq_overrides[irq_override_count].source = entry_buf[3];
                gsi32 = *(uint32_t *)(entry_buf + 4);
                irq_overrides[irq_override_count].gsi = (uint64_t)gsi32;
                irq_overrides[irq_override_count].flags =
                    *(uint16_t *)(entry_buf + 8);
                irq_override_count++;
            }
        }

        offset += entry_len;
    }
}

static void find_acpi_tables(void) {
    uint8_t *rsdp;
    uint64_t rsdt_phys;
    uint64_t rsdt_len;
    int num_entries;
    int i;
    uint64_t table_phys;
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

    rsdt_phys = (uint64_t)(*(uint32_t *)(rsdp + 16));

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
    uint64_t svr;

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

uint64_t lapic_get_id(void) {
    if (!lapic_base) return 0;
    return lapic_read(LAPIC_REG_ID) >> 24;
}

void lapic_send_ipi(uint64_t apic_id, uint64_t vector) {
    lapic_write(LAPIC_REG_ICR_HI, apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LO, vector);
    while (lapic_read(LAPIC_REG_ICR_LO) & ICR_DELIVERY_STATUS)
        __asm__ volatile ("pause");
}

void ioapic_init(void) {
    uint64_t ver;

    vmm_map_page(IOAPIC_VIRT_BASE, ioapic_phys, 0x013);
    ioapic_base = (volatile uint32_t *)IOAPIC_VIRT_BASE;

    ver = ioapic_read(IOAPIC_REG_VER);
    ioapic_max_redir = (ver >> 16) & 0xFF;

    printf("SMP: IOAPIC at phys 0x%016lX, max redirection entries: %u\n",
           ioapic_phys, ioapic_max_redir);
}

static uint64_t irq_to_gsi(uint8_t irq) {
    int i;

    if (!irq_overrides) return (uint64_t)irq;
    for (i = 0; i < irq_override_count; i++) {
        if (irq_overrides[i].source == irq) {
            return irq_overrides[i].gsi;
        }
    }
    return (uint64_t)irq;
}

void ioapic_route_irq(uint8_t irq, uint8_t vector, uint64_t dest_apic_id) {
    uint64_t redtbl_lo;
    uint64_t redtbl_hi;
    uint64_t reg;
    uint64_t gsi;

    gsi = irq_to_gsi(irq);
    if (gsi > ioapic_max_redir) return;

    reg = IOAPIC_REG_REDTBL + gsi * 2;
    redtbl_lo = vector;
    redtbl_hi = dest_apic_id << 24;
    ioapic_write(reg + 1, redtbl_hi);
    ioapic_write(reg, redtbl_lo);
}

void ioapic_mask_irq(uint8_t irq) {
    uint64_t reg;
    uint64_t lo;
    uint64_t gsi;

    gsi = irq_to_gsi(irq);
    if (gsi > ioapic_max_redir) return;

    reg = IOAPIC_REG_REDTBL + gsi * 2;
    lo = ioapic_read(reg);
    lo |= (1 << 16);
    ioapic_write(reg, lo);
}

static void pic_disable(void) {
    outb(0xA1, 0xFF);
    outb(0x21, 0xFF);
}

static void delay_ms(uint64_t ms) {
    uint64_t i;
    uint64_t j;
    for (i = 0; i < ms; i++) {
        for (j = 0; j < 10000; j++) {
            __asm__ volatile ("pause");
        }
    }
}

int smp_processor_id(void) {
    uint64_t id;
    int i;

    if (!lapic_base) return 0;
    id = lapic_get_id();
    for (i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == id) return i;
    }
    return 0;
}

cpu_info_t *smp_this_cpu(void) {
    cpu_info_t *cpu;
    int idx;

    if (smp_gs_ready) {
        __asm__ volatile ("movq %%gs:0, %0" : "=r"(cpu));
        return cpu;
    }
    idx = smp_processor_id();
    return &cpus[idx];
}

task_t *smp_current_task_safe(void) {
    uint64_t id;
    int i;

    if (!cpus || cpu_count <= 0)
        return NULL;
    if (!lapic_base)
        return cpus[0].running_task;
    id = lapic_get_id();
    for (i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == id)
            return cpus[i].running_task;
    }
    return NULL;
}

static void smp_set_cpu_base(cpu_info_t *cpu) {
    uint64_t base;

    base = (uint64_t)(uintptr_t)cpu;
    __asm__ volatile (
        "wrmsr"
        :
        : "c"(0xC0000101u),
          "a"((uint32_t)(base & 0xFFFFFFFFu)),
          "d"((uint32_t)(base >> 32))
        : "memory"
    );
}

int smp_is_bsp(void) {
    int idx;

    idx = smp_processor_id();
    return cpus[idx].bsp;
}

void smp_tlb_flush_all(void) {
    uint64_t my_id;
    int i;

    if (!lapic_base || cpu_count <= 1) return;

    my_id = lapic_get_id();
    for (i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == my_id) continue;
        if (!cpus[i].active) continue;
        lapic_send_ipi(cpus[i].lapic_id, IPI_TLB_FLUSH_VECTOR);
    }
}

void smp_tlb_flush_ack(void) {
    __sync_fetch_and_add(&smp_tlb_flush_acks, 1);
}

int smp_tlb_flush_all_sync(void) {
    uint64_t my_id;
    uint64_t wait;
    int i;
    int result;
    int targets;

    if (!lapic_base || cpu_count <= 1) return 0;
    if (__sync_lock_test_and_set(&smp_tlb_flush_lock, 1))
        return -1;
    my_id = lapic_get_id();
    targets = 0;
    smp_tlb_flush_acks = 0;
    __sync_synchronize();
    for (i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == my_id) continue;
        if (!cpus[i].active) continue;
        targets++;
        lapic_send_ipi(cpus[i].lapic_id, IPI_TLB_FLUSH_VECTOR);
    }
    wait = 10000000;
    while (smp_tlb_flush_acks < targets && wait > 0) {
        __asm__ volatile ("pause" ::: "memory");
        wait--;
    }
    result = smp_tlb_flush_acks == targets ? 0 : -1;
    __sync_lock_release(&smp_tlb_flush_lock);
    return result;
}

void smp_stop_other_cpus(void) {
    uint64_t my_id;
    int i;

    if (!lapic_base || cpu_count <= 1) return;
    my_id = lapic_get_id();
    for (i = 0; i < cpu_count; i++) {
        if (!cpus[i].active || cpus[i].lapic_id == my_id) continue;
        lapic_send_ipi(cpus[i].lapic_id, 255);
    }
}

void ap_main(void);

void ap_main(void) {
    uint64_t id;
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
        gdt_init_ap(cpus[cpu_idx].gdt, cpus[cpu_idx].tss,
                    cpus[cpu_idx].kernel_stack);
    }

    if (cpu_idx >= 0)
        smp_set_cpu_base(&cpus[cpu_idx]);

    idt_load();
    lapic_write(LAPIC_REG_SVR,
                ((lapic_read(LAPIC_REG_SVR) | LAPIC_SVR_ENABLE) & ~0xFFu) | 0xFFu);
    lapic_write(LAPIC_REG_TIMER, LAPIC_TIMER_MASKED);
    lapic_write(0x350, 0x10000);
    lapic_write(0x360, 0x10000);
    lapic_write(0x370, 0x10000);
    lapic_write(LAPIC_REG_ESR, 0);
    lapic_write(LAPIC_REG_ESR, 0);
    lapic_write(LAPIC_REG_EOI, 0);
    lapic_write(LAPIC_REG_TPR, 0xFF);

    if (cpu_idx >= 0) {
        cpus[cpu_idx].active = 1;
        cpus[cpu_idx].scheduler_lock_depth = 0;
        cpus[cpu_idx].sched_saved_rflags = 0;
        cpus[cpu_idx].schedule_force = 0;
    }

    __sync_fetch_and_add(&cpus_booted, 1);

    while (!smp_scheduler_ready)
        __asm__ volatile ("pause" ::: "memory");

    lapic_write(LAPIC_REG_TIMER_DCR, 0x03);
    lapic_write(LAPIC_REG_TIMER, LAPIC_TIMER_PERIODIC | 32);
    lapic_write(LAPIC_REG_TIMER_ICR, lapic_timer_reload);
    lapic_write(LAPIC_REG_TPR, 0);
    __asm__ volatile ("sti" ::: "memory");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void smp_start_aps(void) {
    uint64_t tramp_size;
    uint64_t bsp_id;
    int i;
    volatile int timeout;
    volatile uint64_t *tramp_cr3;
    volatile uint64_t *tramp_stack;
    volatile uint32_t *tramp_flag;
    volatile uint64_t *tramp_entry;

    tramp_size = (uint64_t)(ap_tramp_end - ap_tramp_start);
    if (tramp_size > 0x1000) {
        printf("SMP: trampoline too large (%u bytes)\n", tramp_size);
        return;
    }

    vmm_map_page(AP_TRAMPOLINE_PHYS, AP_TRAMPOLINE_PHYS, 0x003);
    memcpy((void *)AP_TRAMPOLINE_PHYS, ap_tramp_start, tramp_size);

    tramp_cr3   = (volatile uint64_t *)(AP_TRAMPOLINE_PHYS +
                  ((uint8_t *)&ap_boot_cr3 - ap_tramp_start));
    tramp_stack = (volatile uint64_t *)(AP_TRAMPOLINE_PHYS +
                  ((uint8_t *)&ap_boot_stack - ap_tramp_start));
    tramp_flag  = (volatile uint32_t *)(AP_TRAMPOLINE_PHYS +
                  ((uint8_t *)&ap_boot_flag - ap_tramp_start));
    tramp_entry = (volatile uint64_t *)(AP_TRAMPOLINE_PHYS +
                  ((uint8_t *)&ap_boot_entry - ap_tramp_start));

    *tramp_entry = (uint64_t)(uintptr_t)ap_main;

    bsp_id = lapic_get_id();

    for (i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == bsp_id) {
            cpus[i].bsp = 1;
            cpus[i].active = 1;
            continue;
        }

        cpus[i].kernel_stack = kstack_alloc();
        if (!cpus[i].kernel_stack) {
            printf("SMP: failed to allocate stack for CPU %u\n", cpus[i].lapic_id);
            continue;
        }

        cpus[i].gdt = (uint8_t *)cpus[i].kernel_stack + KSTACK_GDT_OFFSET;
        cpus[i].tss = (uint8_t *)cpus[i].kernel_stack + KSTACK_TSS_OFFSET;
        task_prepare_cpu_idle(&cpus[i]);
        *tramp_stack = (uint64_t)cpus[i].kernel_stack + KSTACK_RUNTIME_SIZE;
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

void lapic_timer_init(uint64_t freq_hz) {
    uint64_t initial;
    uint64_t reload;
    uint64_t start_ticks;
    uint64_t elapsed_ticks;
    uint64_t timer_frequency;
    uint64_t flags;

    if (freq_hz == 0) freq_hz = 1000;
    lapic_write(LAPIC_REG_TIMER_DCR, 0x03);

    lapic_write(LAPIC_REG_TIMER, LAPIC_TIMER_MASKED | 32);
    lapic_write(LAPIC_REG_TIMER_ICR, 0xFFFFFFFF);

    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) :: "memory");
    __asm__ volatile ("sti" ::: "memory");
    start_ticks = pit_get_ticks();
    do {
        __asm__ volatile ("hlt" ::: "memory");
        elapsed_ticks = pit_get_ticks() - start_ticks;
    } while (elapsed_ticks < 10);
    __asm__ volatile ("cli" ::: "memory");

    initial = 0xFFFFFFFF - lapic_read(LAPIC_REG_TIMER_CCR);
    timer_frequency = pit_get_frequency();
    if (timer_frequency == 0) timer_frequency = pit_freq;
    if (timer_frequency == 0) timer_frequency = 1000;

    reload = initial * timer_frequency;
    reload /= elapsed_ticks * freq_hz;
    if (reload == 0) reload = 1;
    if (reload > 0xFFFFFFFFu) reload = 0xFFFFFFFFu;

    lapic_write(LAPIC_REG_TIMER,
                LAPIC_TIMER_PERIODIC | 32);
    lapic_timer_reload = (uint32_t)reload;
    lapic_write(LAPIC_REG_TIMER_ICR, lapic_timer_reload);

    if (flags & (1u << 9))
        __asm__ volatile ("sti" ::: "memory");
}

void smp_enable_scheduling(void) {
    __sync_synchronize();
    smp_scheduler_ready = 1;
}

int smp_scheduling_enabled(void) {
    return smp_scheduler_ready != 0;
}

void smp_init(void) {
    uint64_t bsp_apic_id;
    int i;
    int found;

    find_acpi_tables();

    if (cpu_count == 0) {
        cpus[0].lapic_id = 0;
        cpus[0].processor_id = 0;
        cpus[0].bsp = 1;
        cpus[0].active = 1;
        cpu_count = 1;
    }

    printf("SMP: Found %d CPU(s)\n", cpu_count);

    lapic_init();

    bsp_apic_id = lapic_get_id();
    found = 0;
    for (i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == bsp_apic_id) {
            cpus[i].bsp = 1;
            cpus[i].active = 1;
            found = 1;
        }
    }

    if (!found && cpu_count < MAX_CPUS &&
        smp_ensure_cpu_capacity(cpu_count + 1)) {
        cpus[cpu_count].lapic_id = bsp_apic_id;
        cpus[cpu_count].processor_id = 0;
        cpus[cpu_count].bsp = 1;
        cpus[cpu_count].active = 1;
        cpu_count++;
    }

    for (i = 0; i < cpu_count; i++)
        cpus[i].self = &cpus[i];
    smp_set_cpu_base(smp_this_cpu());
    smp_gs_ready = 1;

    ioapic_init();
    pic_disable();

    ioapic_route_irq(0, 32, bsp_apic_id);
    ioapic_route_irq(1, 33, bsp_apic_id);
    {
        uint64_t gsi0;
        uint64_t gsi1;
        uint64_t gsi_i;
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
            while (cpus_booted < expected_aps && --ap_timeout > 0)
                __asm__ volatile ("pause");
            if (cpus_booted < expected_aps) {
                printf("SMP: Warning: only %d of %d APs finished init\n",
                       cpus_booted, expected_aps);
            }
        }
    }
    __sync_synchronize();
    smp_percpu_irq_ready = 1;
    printf("SMP: %d CPU(s) active\n", cpus_booted + 1);
}
