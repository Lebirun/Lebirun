#ifndef KSTACK_H
#define KSTACK_H

#include <stdint.h>

#define KSTACK_REGION_START  0xFFFFFFFFD8000000ULL
#define KSTACK_PAGES_PER_SLOT 3
#define KSTACK_GUARD_PAGES    1
#define KSTACK_USABLE_PAGES   2
#define KSTACK_SLOT_SIZE     ((uint64_t)KSTACK_PAGES_PER_SLOT * 0x1000ULL)
#define KSTACK_USABLE_SIZE   (KSTACK_USABLE_PAGES * PAGE_SIZE)
#define KSTACK_IDLE_RESERVE  0x200ULL
#define KSTACK_RUNTIME_SIZE  (KSTACK_USABLE_SIZE - KSTACK_IDLE_RESERVE)
#define KSTACK_GDT_OFFSET    (KSTACK_RUNTIME_SIZE + 0xE0ULL)
#define KSTACK_TSS_OFFSET    (KSTACK_GDT_OFFSET + 0x50ULL)
#define KSTACK_MAX_STACKS    4096
#define KSTACK_REGION_SIZE   (KSTACK_MAX_STACKS * KSTACK_SLOT_SIZE)
#define KSTACK_REGION_END    (KSTACK_REGION_START + KSTACK_REGION_SIZE)

void kstack_init(void);
uint8_t *kstack_alloc(void);
void kstack_free(uint8_t *base);
void kstack_reclaim_unused(void);
int kstack_page_fault_handler(uint64_t fault_addr);
int kstack_is_in_region(uint64_t addr);
int kstack_prepare_syscall(void);
void kstack_finish_syscall(void);

#endif
