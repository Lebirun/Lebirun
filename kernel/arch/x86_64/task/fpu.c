#include "task_priv.h"

static uint32_t fpu_mxcsr_mask;

static uint64_t fpu_xcr0;
uint32_t fpu_state_size = FPU_STATE_SIZE;

void KERNEL_INIT fpu_cpu_init(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    uint64_t cr4;
    uint64_t xcr0;

    eax = 1;
    ecx = 0;
    __asm__ volatile ("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    if (!(ecx & (1u << 26)) || !(ecx & (1u << 28))) return;
    eax = 0xD;
    ecx = 0;
    __asm__ volatile ("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    xcr0 = eax & 7u;
    if (xcr0 != 7u) return;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4 | (1ULL << 18)) : "memory");
    __asm__ volatile ("xsetbv" : : "a"((uint32_t)xcr0), "d"(0u), "c"(0u) : "memory");
    eax = 0xD;
    ecx = 0;
    __asm__ volatile ("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    if (!fpu_xcr0 && ebx > FPU_STATE_SIZE) {
        fpu_state_size = ebx;
        fpu_xcr0 = xcr0;
    }
}

void fpu_save_area(uint8_t *area) {
    if (!area) return;
    if (fpu_xcr0)
        __asm__ volatile ("xsave64 (%0)" : : "r"(area), "a"(~0u), "d"(~0u) : "memory");
    else
        __asm__ volatile ("fxsave64 (%0)" : : "r"(area) : "memory");
}

void fpu_restore_area(uint8_t *area) {
    uint32_t mxcsr;

    if (area) {
        if (fpu_xcr0)
            __asm__ volatile ("xrstor64 (%0)" : : "r"(area), "a"(~0u), "d"(~0u) : "memory");
        else
            __asm__ volatile ("fxrstor64 (%0)" : : "r"(area) : "memory");
        return;
    }
    mxcsr = 0x1F80;
    __asm__ volatile ("fninit" ::: "memory");
    __asm__ volatile ("ldmxcsr %0" : : "m"(mxcsr) : "memory");
}

void fpu_init_default_state(void) {
    uint8_t state[FPU_STATE_SIZE] __attribute__((aligned(16)));
    uint32_t mxcsr;

    if (fpu_mxcsr_mask != 0) return;
    mxcsr = 0x1F80;
    __asm__ volatile ("fninit" ::: "memory");
    __asm__ volatile ("ldmxcsr %0" : : "m"(mxcsr) : "memory");
    __asm__ volatile ("fxsave64 (%0)" : : "r"(state) : "memory");
    memcpy(&fpu_mxcsr_mask, state + 28, sizeof(fpu_mxcsr_mask));
    if (fpu_mxcsr_mask == 0) fpu_mxcsr_mask = 0x0000FFBF;
}

void task_fpu_legacy_updated(task_t *task) {
    if (task && task->fpu_state && fpu_xcr0)
        task->fpu_state[FPU_STATE_SIZE] |= 3;
}

static void fpu_initialize_area(uint8_t *state) {
    uint16_t control_word;
    uint32_t mxcsr;

    if (!state) return;
    fpu_init_default_state();
    memset(state, 0, fpu_state_size);
    control_word = 0x037F;
    mxcsr = 0x1F80;
    memcpy(state, &control_word, sizeof(control_word));
    memcpy(state + 24, &mxcsr, sizeof(mxcsr));
    memcpy(state + 28, &fpu_mxcsr_mask, sizeof(fpu_mxcsr_mask));
    if (fpu_xcr0) state[FPU_STATE_SIZE] = 3;
}

int task_init_fpu_state(task_t *task) {
    uint8_t *state;

    if (!task) return -1;
    fpu_init_default_state();
    state = (uint8_t *)kmalloc_aligned(fpu_state_size, 64);
    if (!state) return -1;
    fpu_initialize_area(state);
    task->fpu_state = state;
    return 0;
}

uint32_t task_fpu_frame_size(task_t *task) {
    return task && task->fpu_state ? fpu_state_size : 0;
}

void task_fpu_signal_save(task_t *task, void *dst) {
    uint64_t flags;

    __asm__ volatile ("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    fpu_save_area(task->fpu_state);
    if (flags & (1u << 9)) __asm__ volatile ("sti" ::: "memory");
    memcpy(dst, task->fpu_state, fpu_state_size);
    __asm__ volatile ("cli" ::: "memory");
    fpu_initialize_area(task->fpu_state);
    fpu_restore_area(task->fpu_state);
    if (flags & (1u << 9)) __asm__ volatile ("sti" ::: "memory");
}

int task_fpu_signal_restore(task_t *task, const uint8_t *src) {
    uint64_t flags;
    uint64_t header[3];
    uint32_t mxcsr;
    int ok;

    memcpy(&mxcsr, src + 24, sizeof(mxcsr));
    ok = !(mxcsr & ~fpu_mxcsr_mask);
    if (ok && fpu_xcr0) {
        memcpy(header, src + FPU_STATE_SIZE, sizeof(header));
        ok = !(header[0] & ~fpu_xcr0) && !header[1] && !header[2];
    }
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    if (ok) memcpy(task->fpu_state, src, fpu_state_size);
    else fpu_initialize_area(task->fpu_state);
    fpu_restore_area(task->fpu_state);
    if (flags & (1u << 9)) __asm__ volatile ("sti" ::: "memory");
    return ok ? 0 : -1;
}

void task_free_fpu_state(task_t *task) {
    if (!task || !task->fpu_state) return;
    kfree_aligned(task->fpu_state);
    task->fpu_state = NULL;
}

void task_reset_fpu_state(task_t *task) {
    if (!task || !task->fpu_state) return;
    fpu_initialize_area(task->fpu_state);
    fpu_restore_area(task->fpu_state);
}

void task_write_fs_base(uint64_t base) {
    __asm__ volatile (
        "wrmsr"
        :
        : "c"(0xC0000100u),
          "a"((uint32_t)(base & 0xFFFFFFFF)),
          "d"((uint32_t)(base >> 32))
        : "memory"
    );
}
