#ifndef KERNEL_SMP_H
#define KERNEL_SMP_H

#include <stdint.h>

#define MAX_CPUS 16

#define LAPIC_REG_ID        0x020
#define LAPIC_REG_VER       0x030
#define LAPIC_REG_TPR       0x080
#define LAPIC_REG_EOI       0x0B0
#define LAPIC_REG_SVR       0x0F0
#define LAPIC_REG_ESR       0x280
#define LAPIC_REG_ICR_LO    0x300
#define LAPIC_REG_ICR_HI    0x310
#define LAPIC_REG_TIMER     0x320
#define LAPIC_REG_TIMER_ICR 0x380
#define LAPIC_REG_TIMER_CCR 0x390
#define LAPIC_REG_TIMER_DCR 0x3E0

#define LAPIC_SVR_ENABLE    0x100
#define LAPIC_TIMER_PERIODIC 0x20000
#define LAPIC_TIMER_MASKED   0x10000

#define IOAPIC_REG_ID       0x00
#define IOAPIC_REG_VER      0x01
#define IOAPIC_REG_REDTBL   0x10

#define ICR_INIT            0x00000500
#define ICR_STARTUP          0x00000600
#define ICR_LEVEL_ASSERT    0x00004000
#define ICR_LEVEL_DEASSERT  0x00000000
#define ICR_DELIVERY_STATUS 0x00001000

typedef struct task task_t;

typedef struct {
    uint32_t lapic_id;
    uint32_t processor_id;
    int active;
    int bsp;
    uint32_t *gdt;
    void *tss;
    void *kernel_stack;
    task_t *current_task;
    int scheduler_lock_depth;
    uint32_t sched_saved_eflags;
    volatile int schedule_force;
} cpu_info_t;

extern volatile uint32_t *lapic_base;
extern volatile uint32_t *ioapic_base;
extern cpu_info_t cpus[MAX_CPUS];
extern int cpu_count;
extern volatile int cpus_booted;

void smp_init(void);
void lapic_init(void);
void lapic_eoi(void);
uint32_t lapic_get_id(void);
void lapic_send_ipi(uint32_t apic_id, uint32_t vector);
void ioapic_init(void);
void ioapic_route_irq(uint8_t irq, uint8_t vector, uint32_t dest_apic_id);
void smp_start_aps(void);
int smp_processor_id(void);
void lapic_timer_init(uint32_t freq_hz);
cpu_info_t *smp_this_cpu(void);
int smp_is_bsp(void);
void smp_tlb_flush_all(void);

#define IPI_TLB_FLUSH_VECTOR 49

#endif
