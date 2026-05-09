#include <lebirun/reclaim.h>
#include <lebirun/mem_map.h>
#include <lebirun/console.h>
#include <lebirun/framebuffer.h>

void kernel_reclaim_once(int pressure) {
    (void)pressure;
    console_reclaim_unused();
    fb_reclaim_unused();
    slab_reclaim_empty();
    heap_reclaim_unused();
}
