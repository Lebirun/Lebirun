#include <kernel/mem_map.h>
#include <kernel/task.h>
#include <kernel/tty.h>
#include <kernel/debug.h>
#include "launch_user.h"
#include <string.h>

#define DEFAULT_USER_CODE_ADDR 0x00400000

task_t* launch_user_binary(const uint8_t *bin_start, const uint8_t *bin_end) {
    if (!bin_start || !bin_end || bin_end <= bin_start) return NULL;
    uint32_t size = (uint32_t)(bin_end - bin_start);
    uint32_t code_addr = DEFAULT_USER_CODE_ADDR;

    vmm_map_range_alloc(code_addr, size, 0x7);
    memcpy((void*)code_addr, bin_start, size);
    vmm_map_range_alloc(code_addr, size, 0x5);

    task_t* t = create_task((void*)code_addr, TASK_READY, true);
    if (!t) {
        printf("launch_user_binary: create_task failed\n");
        return NULL;
    }
    return t;
}
