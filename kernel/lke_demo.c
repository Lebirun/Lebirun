#include <kernel/mem_map.h>
#include <kernel/vfs.h>
#include <kernel/console.h>
#include <kernel/rng.h>
#include <kernel/rtc.h>
#include <string.h>
#include <stdio.h>

static char *demo_buf;

int lke_module_init(void) {
    uint64_t timestamp;
    uint32_t r;
    int fd;
    int n;
    int con;
    char msg[64];

    printf("[lke_demo] Module loading...\n");

    r = rng_get_u32();
    printf("[lke_demo] Random u32: %u\n", r);

    timestamp = rtc_get_time();
    printf("[lke_demo] RTC time: %lu\n", timestamp);

    demo_buf = kmalloc(256);
    if (!demo_buf) {
        printf("[lke_demo] kmalloc failed\n");
        return -1;
    }
    memset(demo_buf, 0, 256);

    fd = vfs_open_path("/etc/hostname", 0);
    if (fd >= 0) {
        n = vfs_read_fd(fd, (uint8_t *)demo_buf, 255);
        if (n > 0) {
            demo_buf[n] = '\0';
            printf("[lke_demo] /etc/hostname: %s", demo_buf);
        }
        vfs_close_fd(fd);
    } else {
        printf("[lke_demo] Could not open /etc/hostname (fd=%d)\n", fd);
    }

    con = console_get_current();
    n = snprintf(msg, sizeof(msg), "[lke_demo] Active console: %d\n", con);
    console_write_to(con, msg, n);

    printf("[lke_demo] Loaded OK (heap free: %lu bytes)\n", heap_free_space());
    return 0;
}

void lke_module_cleanup(void) {
    if (demo_buf) {
        kfree(demo_buf);
        demo_buf = 0;
    }
    printf("[lke_demo] Unloaded\n");
}
