#include <kernel/mem_map.h>
#include <kernel/vfs.h>
#include <kernel/task.h>
#include <kernel/about.h>
#include <kernel/cmdline.h>
#include <kernel/drivers/net/e1000/e1000.h>
#include <kernel/rtc.h>
#include <string.h>
#include <stdio.h>

extern task_t *current_task;
extern task_t *ready_queue_head;

static vfs_node_t procfs_root;
static vfs_node_t proc_self;
static vfs_node_t proc_self_exe;
static vfs_node_t proc_self_cwd;
static vfs_node_t proc_self_fd;
static vfs_node_t proc_self_maps;
static vfs_node_t proc_self_status;
static vfs_node_t proc_self_cmdline;
static vfs_node_t proc_self_environ;
static vfs_node_t proc_self_stat;
static vfs_node_t proc_self_statm;
static vfs_node_t proc_self_comm;
static vfs_node_t proc_self_limits;
static vfs_node_t proc_self_io;
static vfs_node_t proc_version;
static vfs_node_t proc_uptime;
static vfs_node_t proc_meminfo;
static vfs_node_t proc_cpuinfo;
static vfs_node_t proc_loadavg;
static vfs_node_t proc_stat;
static vfs_node_t proc_mounts;
static vfs_node_t proc_filesystems;
static vfs_node_t proc_cmdline;
static vfs_node_t proc_devices;
static vfs_node_t proc_interrupts;
static vfs_node_t proc_vmstat;
static vfs_node_t proc_memdetail;

static dirent_t proc_dirent;
static dirent_t proc_self_dirent;

#define PROC_PID_POOL_SIZE 32
#define PROC_PID_SUBFILES 12

static vfs_node_t pid_dir_pool[PROC_PID_POOL_SIZE];
static vfs_node_t pid_file_pool[PROC_PID_POOL_SIZE][PROC_PID_SUBFILES];
static dirent_t pid_dirent;

static task_t *procfs_get_task(vfs_node_t *node) {
    pid_t pid;
    if (node->parent && VFS_GET_TYPE(node->flags) == VFS_FILE
        && VFS_GET_TYPE(node->parent->flags) == VFS_DIRECTORY
        && node->parent->inode > 0) {
        pid = (pid_t)node->parent->inode;
    } else {
        pid = (pid_t)node->inode;
    }
    if (pid == 0 && current_task)
        return current_task;
    return task_find(pid);
}

extern volatile uint32_t tick_count;
extern uint32_t pit_freq;
extern uint32_t pae_vmm_pt_count;
extern uint32_t pae_enabled;
extern uint32_t pae_get_heap_pt_count(void);

static uint32_t proc_self_status_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    char buf[2048];
    int len;
    uint32_t remaining;
    task_t *task;
    uint32_t ruid, rgid;
    
    len = 0;
    task = procfs_get_task(node);
    
    if (task) {
        ruid = task->uid;
        rgid = task->gid;
        if (!task->is_user) {
            ruid = 999;
            rgid = 999;
        }
        len = snprintf(buf, sizeof(buf),
            "Name:\t%.15s\n"
            "Umask:\t0022\n"
            "State:\t%c (%s)\n"
            "Tgid:\t%d\n"
            "Ngid:\t0\n"
            "Pid:\t%d\n"
            "PPid:\t%d\n"
            "TracerPid:\t0\n"
            "Uid:\t%u\t%u\t%u\t%u\n"
            "Gid:\t%u\t%u\t%u\t%u\n"
            "FDSize:\t64\n"
            "Groups:\t%u\n"
            "VmPeak:\t%u kB\n"
            "VmSize:\t%u kB\n"
            "VmLck:\t0 kB\n"
            "VmPin:\t0 kB\n"
            "VmHWM:\t%u kB\n"
            "VmRSS:\t%u kB\n"
            "VmData:\t%u kB\n"
            "VmStk:\t%u kB\n"
            "VmExe:\t%u kB\n"
            "VmLib:\t0 kB\n"
            "VmPTE:\t4 kB\n"
            "VmSwap:\t0 kB\n"
            "Threads:\t1\n"
            "SigQ:\t0/256\n"
            "SigPnd:\t%08x\n"
            "ShdPnd:\t00000000\n"
            "SigBlk:\t%08x\n"
            "SigIgn:\t00000000\n"
            "SigCgt:\t00000000\n"
            "CapInh:\t0000000000000000\n"
            "CapPrm:\t0000003fffffffff\n"
            "CapEff:\t0000003fffffffff\n"
            "CapBnd:\t0000003fffffffff\n"
            "CapAmb:\t0000000000000000\n"
            "Seccomp:\t0\n"
            "KernelTask:\t%d\n",
            task->name[0] ? task->name : "unknown",
            task->state == TASK_RUNNING ? 'R' : 
            task->state == TASK_BLOCKED ? 'S' : 'Z',
            task->state == TASK_RUNNING ? "running" :
            task->state == TASK_BLOCKED ? "sleeping" : "zombie",
            task->pid,
            task->pid,
            task->ppid,
            ruid, ruid, ruid, ruid,
            rgid, rgid, rgid, rgid,
            rgid,
            task->user_pages_count * 4,
            task->user_pages_count * 4,
            task->user_pages_count * 4,
            task->user_pages_count * 4,
            task->user_pages_count * 2,
            task->stack_size / 1024,
            task->user_pages_count * 2,
            task->sig_pending,
            task->sig_blocked,
            task->is_kernel_task ? 1 : 0);
    }
    
    if (offset >= (uint32_t)len) return 0;
    remaining = (uint32_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint32_t proc_self_maps_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    char buf[2048];
    int len;
    int n;
    uint32_t remaining;
    task_t *task;
    
    len = 0;
    task = procfs_get_task(node);
    
    if (task && task->user_brk > 0) {
        if (len < (int)sizeof(buf) - 1) {
            n = snprintf(buf + len, sizeof(buf) - (size_t)len,
                "00100000-%08x r-xp 00000000 00:00 0 [text]\n",
                0x00400000);
            if (n > 0) len += n;
            if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
        }
        if (len < (int)sizeof(buf) - 1) {
            n = snprintf(buf + len, sizeof(buf) - (size_t)len,
                "00400000-%08x rw-p 00000000 00:00 0 [heap]\n",
                task->user_brk);
            if (n > 0) len += n;
            if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
        }
        if (len < (int)sizeof(buf) - 1) {
            n = snprintf(buf + len, sizeof(buf) - (size_t)len,
                "007f0000-00800000 rw-p 00000000 00:00 0 [stack]\n");
            if (n > 0) len += n;
            if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
        }
    }
    
    if (offset >= (uint32_t)len) return 0;
    remaining = (uint32_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint32_t proc_self_cmdline_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    char cmdline[128];
    uint32_t len;
    uint32_t remaining;
    task_t *task;
    
    task = procfs_get_task(node);
    if (task && task->name[0]) {
        len = 0;
        while (task->name[len] && len < 15) len++;
        memcpy(cmdline, task->name, len);
        cmdline[len] = '\0';
        len++;
    } else {
        cmdline[0] = 'i'; cmdline[1] = 'n'; cmdline[2] = 'i'; cmdline[3] = 't'; cmdline[4] = '\0';
        len = 5;
    }
    
    if (offset >= len) return 0;
    remaining = len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, cmdline + offset, size);
    return size;
}

static uint32_t proc_self_environ_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    const char *env;
    uint32_t len;
    uint32_t remaining;
    
    (void)node;
    
    env = "PATH=/bin:/usr/bin\0HOME=/\0TERM=linux\0";
    len = 38;
    
    if (offset >= len) return 0;
    remaining = len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, env + offset, size);
    return size;
}

static uint32_t proc_version_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    char ver[128];
    int len;
    uint32_t remaining;
    
    (void)node;
    
    len = snprintf(ver, sizeof(ver), "%s version %s (%s %s) #1 SMP PREEMPT\n",
        SYSNAME, RELEASE, KERNEL_BUILD_DATE, KERNEL_BUILD_TIME);
    
    if (offset >= (uint32_t)len) return 0;
    remaining = (uint32_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, ver + offset, size);
    return size;
}

static uint32_t proc_uptime_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    char buf[64];
    uint32_t seconds;
    int len;
    uint32_t remaining;
    
    (void)node;
    
    seconds = tick_count / pit_freq;
    len = snprintf(buf, sizeof(buf), "%u.%02u %u.%02u\n", 
        seconds, (tick_count % pit_freq) * 100 / pit_freq,
        seconds, 0);
    
    if (offset >= (uint32_t)len) return 0;
    remaining = (uint32_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint32_t proc_meminfo_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    char buf[512];
    uint32_t total_kb;
    uint32_t free_pages_kb;
    int len;
    uint32_t remaining;
    
    (void)node;
    
    total_kb = pfa_get_usable_ram_kb();
    free_pages_kb = pfa_count_free() * 4;
    
    len = snprintf(buf, sizeof(buf),
        "MemTotal:      %8u kB\n"
        "MemFree:       %8u kB\n"
        "MemAvailable:  %8u kB\n"
        "Buffers:       %8u kB\n"
        "Cached:        %8u kB\n"
        "SwapCached:    %8u kB\n"
        "SwapTotal:     %8u kB\n"
        "SwapFree:      %8u kB\n"
        "Shmem:         %8u kB\n"
        "SReclaimable:  %8u kB\n",
        total_kb,
        free_pages_kb,
        free_pages_kb,
        0,
        0,
        0,
        0,
        0,
        0,
        0);
    
    if (offset >= (uint32_t)len) return 0;
    remaining = (uint32_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint32_t proc_cpuinfo_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    const char *info;
    uint32_t len;
    uint32_t remaining;
    
    (void)node;
    
    info = 
        "processor\t: 0\n"
        "vendor_id\t: GenuineIntel\n"
        "cpu family\t: 6\n"
        "model\t\t: 0\n"
        "model name\t: Intel(R) Processor\n"
        "stepping\t: 0\n"
        "cpu MHz\t\t: 1000.000\n"
        "cache size\t: 256 KB\n"
        "bogomips\t: 2000.00\n"
        "flags\t\t: fpu vme de pse tsc msr pae mce cx8\n\n";
    
    len = 0;
    while (info[len]) len++;
    
    if (offset >= len) return 0;
    remaining = len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, info + offset, size);
    return size;
}

static uint32_t proc_loadavg_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    char buf[64];
    int len;
    uint32_t remaining;
    
    (void)node;
    
    len = snprintf(buf, sizeof(buf), "0.00 0.00 0.00 1/1 %d\n",
        current_task ? current_task->pid : 1);
    
    if (offset >= (uint32_t)len) return 0;
    remaining = (uint32_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint32_t proc_self_stat_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    char buf[512];
    int len;
    uint32_t remaining;
    task_t *task;
    uint32_t utime_val;
    uint32_t stime_val;
    uint32_t starttime_val;
    uint32_t vsize_val;
    uint32_t rss_val;
    
    len = 0;
    task = procfs_get_task(node);
    
    if (task) {
        utime_val = task->utime;
        stime_val = task->stime;
        starttime_val = task->start_tick;
        vsize_val = task->user_pages_count * 4096;
        rss_val = task->user_pages_count;
        if (task->is_kernel_task && rss_val == 0) {
            vsize_val = task->kernel_stack_size;
            rss_val = task->kernel_stack_size / 4096;
        }
        len = snprintf(buf, sizeof(buf),
            "%d (%s) %c %d %d %d 0 -1 0 0 0 0 0 %u %u 0 0 20 0 1 0 %u %u %u -1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
            task->pid,
            task->name[0] ? task->name : "unknown",
            task->state == TASK_RUNNING ? 'R' : 
            task->state == TASK_BLOCKED ? 'S' : 'Z',
            task->ppid,
            task->pgid,
            task->sid,
            utime_val,
            stime_val,
            starttime_val,
            vsize_val,
            rss_val);
    }
    
    if (offset >= (uint32_t)len) return 0;
    remaining = (uint32_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint32_t proc_self_statm_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    char buf[128];
    int len;
    uint32_t pages;
    uint32_t remaining;
    task_t *task;
    
    len = 0;
    task = procfs_get_task(node);
    
    if (task) {
        pages = task->user_pages_count;
        len = snprintf(buf, sizeof(buf), "%u %u %u 1 0 %u 0\n",
            pages, pages, pages / 2, pages);
    }
    
    if (offset >= (uint32_t)len) return 0;
    remaining = (uint32_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint32_t proc_self_comm_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    char buf[32];
    int len;
    uint32_t remaining;
    task_t *task;
    
    len = 0;
    task = procfs_get_task(node);
    
    if (task) {
        len = snprintf(buf, sizeof(buf), "%s\n",
            task->name[0] ? task->name : "unknown");
    }
    
    if (offset >= (uint32_t)len) return 0;
    remaining = (uint32_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint32_t proc_self_limits_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    const char *buf;
    uint32_t len;
    uint32_t remaining;
    
    (void)node;
    
    buf = 
        "Limit                     Soft Limit           Hard Limit           Units     \n"
        "Max cpu time              unlimited            unlimited            seconds   \n"
        "Max file size             unlimited            unlimited            bytes     \n"
        "Max data size             unlimited            unlimited            bytes     \n"
        "Max stack size            8388608              unlimited            bytes     \n"
        "Max core file size        0                    unlimited            bytes     \n"
        "Max resident set          unlimited            unlimited            bytes     \n"
        "Max processes             256                  256                  processes \n"
        "Max open files            64                   64                   files     \n"
        "Max locked memory         65536                65536                bytes     \n"
        "Max address space         unlimited            unlimited            bytes     \n"
        "Max file locks            unlimited            unlimited            locks     \n"
        "Max pending signals       256                  256                  signals   \n"
        "Max msgqueue size         819200               819200               bytes     \n"
        "Max nice priority         0                    0                    \n"
        "Max realtime priority     0                    0                    \n"
        "Max realtime timeout      unlimited            unlimited            us        \n";
    
    len = 0;
    while (buf[len]) len++;
    
    if (offset >= len) return 0;
    remaining = len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint32_t proc_self_io_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    const char *buf;
    uint32_t len;
    uint32_t remaining;
    
    (void)node;
    
    buf = 
        "rchar: 0\n"
        "wchar: 0\n"
        "syscr: 0\n"
        "syscw: 0\n"
        "read_bytes: 0\n"
        "write_bytes: 0\n"
        "cancelled_write_bytes: 0\n";
    
    len = 0;
    while (buf[len]) len++;
    
    if (offset >= len) return 0;
    remaining = len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint32_t proc_stat_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    char buf[512];
    int len;
    uint32_t remaining;
    static uint32_t cached_btime = 0;
    uint32_t btime;
    extern volatile uint32_t cpu_user_ticks;
    extern volatile uint32_t cpu_system_ticks;
    extern volatile uint32_t cpu_idle_ticks;
    uint32_t user_t;
    uint32_t sys_t;
    uint32_t idle_t;
    uint32_t running;
    task_t *t;
    task_t *start;
    
    (void)node;
    
    user_t = cpu_user_ticks;
    sys_t = cpu_system_ticks;
    idle_t = cpu_idle_ticks;
    
    running = 0;
    start = ready_queue_head;
    t = start;
    if (t) {
        do {
            if (t->state == TASK_RUNNING || t->state == TASK_READY)
                running++;
            t = t->next;
        } while (t && t != start);
    }
    if (running == 0) running = 1;
    
    if (cached_btime == 0)
        cached_btime = rtc_get_time();
    btime = cached_btime;
    
    len = snprintf(buf, sizeof(buf),
        "cpu  %u 0 %u %u 0 0 0 0 0 0\n"
        "cpu0 %u 0 %u %u 0 0 0 0 0 0\n"
        "intr %u 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n"
        "ctxt %u\n"
        "btime %u\n"
        "processes %d\n"
        "procs_running %u\n"
        "procs_blocked 0\n"
        "softirq 0 0 0 0 0 0 0 0 0 0 0\n",
        user_t, sys_t, idle_t,
        user_t, sys_t, idle_t,
        tick_count,
        tick_count,
        btime,
        current_task ? current_task->pid : 1,
        running);
    
    if (offset >= (uint32_t)len) return 0;
    remaining = (uint32_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint32_t proc_mounts_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    size_t buf_size;
    char *buf;
    size_t len;
    int mount_count;
    int i;
    vfs_mount_t *mount;
    const char *device;
    const char *path;
    const char *fsname;
    const char *opts;
    size_t remaining;
    int written;
    uint32_t available;
    
    (void)node;

    mount_count = vfs_get_mount_count();
    buf_size = mount_count * (VFS_MAX_PATH * 2 + 64);
    if (buf_size < 1024) buf_size = 1024;
    buf = (char *)kmalloc(buf_size);
    if (!buf) {
        return 0;
    }

    len = 0;
    for (i = 0; i < mount_count; ++i) {
        mount = vfs_get_mount(i);
        if (!mount) {
            continue;
        }

        device = mount->device[0] ? mount->device :
            (mount->fs_type && mount->fs_type->name ? mount->fs_type->name : "unknown");
        path = mount->path[0] ? mount->path : "/";
        fsname = mount->fs_type && mount->fs_type->name ? mount->fs_type->name : "unknown";
        opts = "rw";

        if (len >= buf_size) {
            break;
        }

        remaining = buf_size - len;
        written = snprintf(buf + len, remaining, "%s %s %s %s 0 0\n",
            device, path, fsname, opts);
        if (written <= 0) {
            continue;
        }
        if ((size_t)written >= remaining) {
            len = buf_size - 1;
            break;
        }

        len += (size_t)written;
    }

    if (len == 0) {
        kfree(buf);
        return 0;
    }

    if (offset >= len) {
        kfree(buf);
        return 0;
    }

    available = (uint32_t)(len - offset);
    if (size > available) {
        size = available;
    }
    memcpy(buffer, buf + offset, size);
    kfree(buf);
    return size;
}

static uint32_t proc_filesystems_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    const char *buf;
    uint32_t len;
    uint32_t remaining;
    
    (void)node;
    
    buf = 
        "nodev\tprocfs\n"
        "nodev\tdevfs\n"
        "nodev\tramfs\n"
        "\text2\n"
        "\text4\n";
    
    len = 0;
    while (buf[len]) len++;
    
    if (offset >= len) return 0;
    remaining = len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint32_t proc_cmdline_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    const char *raw;
    char buf[CMDLINE_MAX + 32];
    uint32_t len;
    uint32_t remaining;
    
    (void)node;
    
    raw = cmdline_get();
    if (raw[0])
        len = snprintf(buf, sizeof(buf), "%s\n", raw);
    else
        len = snprintf(buf, sizeof(buf), "console=tty0 root=/dev/ram0 rw\n");
    
    if (offset >= len) return 0;
    remaining = len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint32_t proc_devices_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    const char *buf;
    uint32_t len;
    uint32_t remaining;
    
    (void)node;
    
    buf = 
        "Character devices:\n"
        "  1 mem\n"
        "  4 tty\n"
        "  5 /dev/tty\n"
        "  5 /dev/console\n"
        "  5 /dev/ptmx\n"
        " 10 misc\n"
        "136 pts\n"
        "\n"
        "Block devices:\n"
        "  8 sd\n";
    
    len = 0;
    while (buf[len]) len++;
    
    if (offset >= len) return 0;
    remaining = len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint32_t proc_interrupts_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    char buf[512];
    int len;
    uint32_t remaining;
    
    (void)node;
    
    len = snprintf(buf, sizeof(buf),
        "           CPU0\n"
        "  0:    %8u   PIT timer\n"
        "  1:    %8u   i8042 keyboard\n"
        " 14:    %8u   IDE\n"
        " 15:    %8u   IDE\n"
        "NMI:           0   Non-maskable interrupts\n"
        "LOC:           0   Local timer interrupts\n"
        "ERR:           0\n",
        tick_count, tick_count / 100, 0, 0);
    
    if (offset >= (uint32_t)len) return 0;
    remaining = (uint32_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint32_t proc_vmstat_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    extern uint32_t pfa_count_free(void);
    uint32_t free_pages;
    char buf[1024];
    int len;
    uint32_t remaining;
    
    (void)node;
    
    free_pages = pfa_count_free();
    
    len = snprintf(buf, sizeof(buf),
        "nr_free_pages %u\n"
        "nr_alloc_batch 0\n"
        "nr_inactive_anon 0\n"
        "nr_active_anon 0\n"
        "nr_inactive_file 0\n"
        "nr_active_file 0\n"
        "nr_unevictable 0\n"
        "nr_mlock 0\n"
        "nr_anon_pages 0\n"
        "nr_mapped 0\n"
        "nr_file_pages 0\n"
        "nr_dirty 0\n"
        "nr_writeback 0\n"
        "nr_slab_reclaimable 0\n"
        "nr_slab_unreclaimable 0\n"
        "nr_page_table_pages 0\n"
        "nr_kernel_stack 0\n"
        "pgpgin 0\n"
        "pgpgout 0\n"
        "pswpin 0\n"
        "pswpout 0\n"
        "pgalloc_normal 0\n"
        "pgfree 0\n"
        "pgactivate 0\n"
        "pgdeactivate 0\n"
        "pgfault 0\n"
        "pgmajfault 0\n",
        free_pages);
    
    if (offset >= (uint32_t)len) return 0;
    remaining = (uint32_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint32_t proc_memdetail_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    char buf[2048];
    int len;
    uint32_t remaining;
    uint32_t pfa_used_kb;
    uint32_t kern_kb;
    uint32_t bitmap_kb;
    uint32_t heap_committed;
    uint32_t heap_reserved;
    uint32_t heap_used;
    uint32_t heap_total;
    uint32_t slab_pages;
    uint32_t e1000_pages;
    uint32_t ahci_pages;
    uint32_t pae_pt_pages;
    uint32_t pae_heap_pt;
    uint32_t user_elf_pages;
    uint32_t user_stack_pages;
    uint32_t user_pd_pages;
    task_t *t;
    task_t *start;

    (void)node;

    pfa_used_kb = pfa_get_kernel_used_kb();
    kern_kb = pfa_get_kernel_binary_kb();
    bitmap_kb = pfa_get_bitmap_kb();

    heap_committed = demand_get_committed_pages();
    heap_reserved = demand_get_reserved_pages();
    heap_used = kernel_heap.used_size;
    heap_total = kernel_heap.total_size;

    slab_pages = slab_get_total_pages();

    e1000_pages = 2 + E1000_NUM_RX_DESC + E1000_NUM_TX_DESC;

    ahci_pages = 3;

    pae_pt_pages = 0;
    pae_heap_pt = 0;
    if (pae_enabled) {
        pae_pt_pages = pae_vmm_pt_count;
        pae_heap_pt = pae_get_heap_pt_count();
    }

    user_elf_pages = 0;
    user_stack_pages = 0;
    user_pd_pages = 0;

    start = ready_queue_head;
    t = start;
    if (t) {
        do {
            if (t->is_user && t->user_pages) {
                user_elf_pages += t->user_pages_count;
                user_pd_pages++;
            }
            t = t->next;
        } while (t && t != start);
    }

    user_stack_pages = user_pd_pages * (0x4000u / PAGE_SIZE);
    if (user_elf_pages >= user_stack_pages) {
        user_elf_pages -= user_stack_pages;
    }
    user_pd_pages = user_pd_pages * (pae_enabled ? 4 : 1);

    len = snprintf(buf, sizeof(buf),
        "PFA_AllocatedKB:    %8u\n"
        "KernelBinaryKB:     %8u\n"
        "BitmapKB:           %8u\n"
        "HeapCommitPages:    %8u\n"
        "HeapCommitKB:       %8u\n"
        "HeapReservePages:   %8u\n"
        "HeapUsedBytes:      %8u\n"
        "HeapTotalBytes:     %8u\n"
        "SlabPages:          %8u\n"
        "SlabKB:             %8u\n"
        "E1000Pages:         %8u\n"
        "E1000KB:            %8u\n"
        "AHCIPages:          %8u\n"
        "AHCIKB:             %8u\n"
        "PAE_VMMPTPages:     %8u\n"
        "PAE_VMMPTKB:        %8u\n"
        "PAE_HeapPTPages:    %8u\n"
        "PAE_HeapPTKB:       %8u\n"
        "UserELFPages:       %8u\n"
        "UserELFKB:          %8u\n"
        "UserStackPages:     %8u\n"
        "UserStackKB:        %8u\n"
        "UserPDPages:        %8u\n"
        "UserPDKB:           %8u\n",
        pfa_used_kb,
        kern_kb,
        bitmap_kb,
        heap_committed,
        heap_committed * 4,
        heap_reserved,
        heap_used,
        heap_total,
        slab_pages,
        slab_pages * 4,
        e1000_pages,
        e1000_pages * 4,
        ahci_pages,
        ahci_pages * 4,
        pae_pt_pages,
        pae_pt_pages * 4,
        pae_heap_pt,
        pae_heap_pt * 4,
        user_elf_pages,
        user_elf_pages * 4,
        user_stack_pages,
        user_stack_pages * 4,
        user_pd_pages,
        user_pd_pages * 4);

    if (offset >= (uint32_t)len) return 0;
    remaining = (uint32_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static dirent_t *procfs_readdir(vfs_node_t *node, uint32_t index) {
    task_t *t;
    uint32_t count;
    uint32_t pid_index;
    
    (void)node;
    
    static const char *entries[] = {
        "self", "version", "uptime", "meminfo", "cpuinfo", "loadavg",
        "stat", "mounts", "filesystems", "cmdline", "devices", "interrupts", "vmstat",
        "memdetail"
    };
    
    if (index < sizeof(entries)/sizeof(entries[0])) {
        strcpy(proc_dirent.name, entries[index]);
        proc_dirent.inode = index + 1;
        proc_dirent.type = (index == 0) ? VFS_DIRECTORY : VFS_FILE;
        return &proc_dirent;
    }
    
    pid_index = index - (uint32_t)(sizeof(entries)/sizeof(entries[0]));
    count = 0;
    t = all_tasks_head;
    while (t) {
        if (t->id != 0 || t->is_user) {
            if (count == pid_index) {
                snprintf(proc_dirent.name, sizeof(proc_dirent.name), "%d", t->pid);
                proc_dirent.inode = (uint32_t)t->pid;
                proc_dirent.type = VFS_DIRECTORY;
                return &proc_dirent;
            }
            count++;
        }
        t = t->all_next;
    }
    
    return NULL;
}

static dirent_t *proc_pid_readdir(vfs_node_t *node, uint32_t index) {
    static const char *entries[] = {
        "maps", "status", "cmdline", "environ",
        "stat", "statm", "comm", "limits", "io"
    };
    
    (void)node;
    
    if (index < sizeof(entries)/sizeof(entries[0])) {
        strcpy(pid_dirent.name, entries[index]);
        pid_dirent.inode = node->inode * 100 + index;
        pid_dirent.type = VFS_FILE;
        return &pid_dirent;
    }
    
    return NULL;
}

static vfs_node_t *proc_pid_finddir(vfs_node_t *node, const char *name) {
    uint32_t pid_val;
    uint32_t pool_idx;
    int file_idx;
    
    static const struct {
        const char *name;
        read_type_t read;
    } files[] = {
        { "maps",    proc_self_maps_read },
        { "status",  proc_self_status_read },
        { "cmdline", proc_self_cmdline_read },
        { "environ", proc_self_environ_read },
        { "stat",    proc_self_stat_read },
        { "statm",   proc_self_statm_read },
        { "comm",    proc_self_comm_read },
        { "limits",  proc_self_limits_read },
        { "io",      proc_self_io_read },
    };
    
    pid_val = node->inode;
    pool_idx = pid_val % PROC_PID_POOL_SIZE;
    
    for (file_idx = 0; file_idx < (int)(sizeof(files)/sizeof(files[0])); file_idx++) {
        if (strcmp(name, files[file_idx].name) == 0) {
            vfs_node_t *fnode = &pid_file_pool[pool_idx][file_idx];
            memset(fnode, 0, sizeof(vfs_node_t));
            strcpy(fnode->name, name);
            fnode->flags = VFS_FILE;
            fnode->inode = pid_val * 100 + (uint32_t)file_idx;
            fnode->read = files[file_idx].read;
            fnode->parent = node;
            fnode->ref_count = 1;
            return fnode;
        }
    }
    
    return NULL;
}

static vfs_node_t *procfs_setup_pid_dir(pid_t pid) {
    uint32_t pool_idx;
    vfs_node_t *dir;
    
    pool_idx = (uint32_t)pid % PROC_PID_POOL_SIZE;
    dir = &pid_dir_pool[pool_idx];
    memset(dir, 0, sizeof(vfs_node_t));
    snprintf(dir->name, sizeof(dir->name), "%d", pid);
    dir->flags = VFS_DIRECTORY;
    dir->inode = (uint32_t)pid;
    dir->readdir = proc_pid_readdir;
    dir->finddir = proc_pid_finddir;
    dir->parent = &procfs_root;
    dir->ref_count = 1;
    return dir;
}

static vfs_node_t *procfs_finddir(vfs_node_t *node, const char *name) {
    (void)node;
    
    if (strcmp(name, "self") == 0) return &proc_self;
    if (strcmp(name, "version") == 0) return &proc_version;
    if (strcmp(name, "uptime") == 0) return &proc_uptime;
    if (strcmp(name, "meminfo") == 0) return &proc_meminfo;
    if (strcmp(name, "cpuinfo") == 0) return &proc_cpuinfo;
    if (strcmp(name, "loadavg") == 0) return &proc_loadavg;
    if (strcmp(name, "stat") == 0) return &proc_stat;
    if (strcmp(name, "mounts") == 0) return &proc_mounts;
    if (strcmp(name, "filesystems") == 0) return &proc_filesystems;
    if (strcmp(name, "cmdline") == 0) return &proc_cmdline;
    if (strcmp(name, "devices") == 0) return &proc_devices;
    if (strcmp(name, "interrupts") == 0) return &proc_interrupts;
    if (strcmp(name, "vmstat") == 0) return &proc_vmstat;
    if (strcmp(name, "memdetail") == 0) return &proc_memdetail;
    
    {
        int pid = 0;
        const char *p = name;
        while (*p >= '0' && *p <= '9') {
            pid = pid * 10 + (*p - '0');
            p++;
        }
        if (*p == '\0' && pid > 0) {
            task_t *t = task_find((pid_t)pid);
            if (t) {
                return procfs_setup_pid_dir((pid_t)pid);
            }
        }
    }
    
    return NULL;
}

static dirent_t *proc_self_readdir(vfs_node_t *node, uint32_t index) {
    (void)node;
    
    static const char *entries[] = {
        "exe", "cwd", "fd", "maps", "status", "cmdline", "environ",
        "stat", "statm", "comm", "limits", "io"
    };
    
    if (index < sizeof(entries)/sizeof(entries[0])) {
        strcpy(proc_self_dirent.name, entries[index]);
        proc_self_dirent.inode = index + 100;
        proc_self_dirent.type = (index < 3) ? VFS_SYMLINK : VFS_FILE;
        return &proc_self_dirent;
    }
    
    return NULL;
}

static vfs_node_t *proc_self_finddir(vfs_node_t *node, const char *name) {
    (void)node;
    
    if (strcmp(name, "exe") == 0) return &proc_self_exe;
    if (strcmp(name, "cwd") == 0) return &proc_self_cwd;
    if (strcmp(name, "fd") == 0) return &proc_self_fd;
    if (strcmp(name, "maps") == 0) return &proc_self_maps;
    if (strcmp(name, "status") == 0) return &proc_self_status;
    if (strcmp(name, "cmdline") == 0) return &proc_self_cmdline;
    if (strcmp(name, "environ") == 0) return &proc_self_environ;
    if (strcmp(name, "stat") == 0) return &proc_self_stat;
    if (strcmp(name, "statm") == 0) return &proc_self_statm;
    if (strcmp(name, "comm") == 0) return &proc_self_comm;
    if (strcmp(name, "limits") == 0) return &proc_self_limits;
    if (strcmp(name, "io") == 0) return &proc_self_io;
    
    return NULL;
}

static vfs_node_t *procfs_mount(const char *device, const char *mountpoint) {
    (void)device; (void)mountpoint;
    return &procfs_root;
}

static int procfs_unmount(vfs_node_t *node) {
    (void)node;
    return 0;
}

static vfs_fs_type_t procfs_type;

void procfs_init(void) {
    procfs_type.name = "procfs";
    procfs_type.mount = procfs_mount;
    procfs_type.unmount = procfs_unmount;
    procfs_type.next = NULL;

    memset(&procfs_root, 0, sizeof(vfs_node_t));
    strcpy(procfs_root.name, "proc");
    procfs_root.flags = VFS_DIRECTORY;
    procfs_root.readdir = procfs_readdir;
    procfs_root.finddir = procfs_finddir;
    procfs_root.ref_count = 1;
    
    memset(&proc_self, 0, sizeof(vfs_node_t));
    strcpy(proc_self.name, "self");
    proc_self.flags = VFS_DIRECTORY;
    proc_self.readdir = proc_self_readdir;
    proc_self.finddir = proc_self_finddir;
    proc_self.parent = &procfs_root;
    proc_self.ref_count = 1;
    
    memset(&proc_self_exe, 0, sizeof(vfs_node_t));
    strcpy(proc_self_exe.name, "exe");
    proc_self_exe.flags = VFS_SYMLINK;
    proc_self_exe.parent = &proc_self;
    proc_self_exe.ref_count = 1;
    
    memset(&proc_self_cwd, 0, sizeof(vfs_node_t));
    strcpy(proc_self_cwd.name, "cwd");
    proc_self_cwd.flags = VFS_SYMLINK;
    proc_self_cwd.parent = &proc_self;
    proc_self_cwd.ref_count = 1;
    
    memset(&proc_self_fd, 0, sizeof(vfs_node_t));
    strcpy(proc_self_fd.name, "fd");
    proc_self_fd.flags = VFS_DIRECTORY;
    proc_self_fd.parent = &proc_self;
    proc_self_fd.ref_count = 1;
    
    memset(&proc_self_maps, 0, sizeof(vfs_node_t));
    strcpy(proc_self_maps.name, "maps");
    proc_self_maps.flags = VFS_FILE;
    proc_self_maps.read = proc_self_maps_read;
    proc_self_maps.parent = &proc_self;
    proc_self_maps.ref_count = 1;
    
    memset(&proc_self_status, 0, sizeof(vfs_node_t));
    strcpy(proc_self_status.name, "status");
    proc_self_status.flags = VFS_FILE;
    proc_self_status.read = proc_self_status_read;
    proc_self_status.parent = &proc_self;
    proc_self_status.ref_count = 1;
    
    memset(&proc_self_cmdline, 0, sizeof(vfs_node_t));
    strcpy(proc_self_cmdline.name, "cmdline");
    proc_self_cmdline.flags = VFS_FILE;
    proc_self_cmdline.read = proc_self_cmdline_read;
    proc_self_cmdline.parent = &proc_self;
    proc_self_cmdline.ref_count = 1;
    
    memset(&proc_self_environ, 0, sizeof(vfs_node_t));
    strcpy(proc_self_environ.name, "environ");
    proc_self_environ.flags = VFS_FILE;
    proc_self_environ.read = proc_self_environ_read;
    proc_self_environ.parent = &proc_self;
    proc_self_environ.ref_count = 1;
    
    memset(&proc_version, 0, sizeof(vfs_node_t));
    strcpy(proc_version.name, "version");
    proc_version.flags = VFS_FILE;
    proc_version.read = proc_version_read;
    proc_version.parent = &procfs_root;
    proc_version.ref_count = 1;
    
    memset(&proc_uptime, 0, sizeof(vfs_node_t));
    strcpy(proc_uptime.name, "uptime");
    proc_uptime.flags = VFS_FILE;
    proc_uptime.read = proc_uptime_read;
    proc_uptime.parent = &procfs_root;
    proc_uptime.ref_count = 1;
    
    memset(&proc_meminfo, 0, sizeof(vfs_node_t));
    strcpy(proc_meminfo.name, "meminfo");
    proc_meminfo.flags = VFS_FILE;
    proc_meminfo.read = proc_meminfo_read;
    proc_meminfo.parent = &procfs_root;
    proc_meminfo.ref_count = 1;
    
    memset(&proc_cpuinfo, 0, sizeof(vfs_node_t));
    strcpy(proc_cpuinfo.name, "cpuinfo");
    proc_cpuinfo.flags = VFS_FILE;
    proc_cpuinfo.read = proc_cpuinfo_read;
    proc_cpuinfo.parent = &procfs_root;
    proc_cpuinfo.ref_count = 1;
    
    memset(&proc_loadavg, 0, sizeof(vfs_node_t));
    strcpy(proc_loadavg.name, "loadavg");
    proc_loadavg.flags = VFS_FILE;
    proc_loadavg.read = proc_loadavg_read;
    proc_loadavg.parent = &procfs_root;
    proc_loadavg.ref_count = 1;
    
    memset(&proc_self_stat, 0, sizeof(vfs_node_t));
    strcpy(proc_self_stat.name, "stat");
    proc_self_stat.flags = VFS_FILE;
    proc_self_stat.read = proc_self_stat_read;
    proc_self_stat.parent = &proc_self;
    proc_self_stat.ref_count = 1;
    
    memset(&proc_self_statm, 0, sizeof(vfs_node_t));
    strcpy(proc_self_statm.name, "statm");
    proc_self_statm.flags = VFS_FILE;
    proc_self_statm.read = proc_self_statm_read;
    proc_self_statm.parent = &proc_self;
    proc_self_statm.ref_count = 1;
    
    memset(&proc_self_comm, 0, sizeof(vfs_node_t));
    strcpy(proc_self_comm.name, "comm");
    proc_self_comm.flags = VFS_FILE;
    proc_self_comm.read = proc_self_comm_read;
    proc_self_comm.parent = &proc_self;
    proc_self_comm.ref_count = 1;
    
    memset(&proc_self_limits, 0, sizeof(vfs_node_t));
    strcpy(proc_self_limits.name, "limits");
    proc_self_limits.flags = VFS_FILE;
    proc_self_limits.read = proc_self_limits_read;
    proc_self_limits.parent = &proc_self;
    proc_self_limits.ref_count = 1;
    
    memset(&proc_self_io, 0, sizeof(vfs_node_t));
    strcpy(proc_self_io.name, "io");
    proc_self_io.flags = VFS_FILE;
    proc_self_io.read = proc_self_io_read;
    proc_self_io.parent = &proc_self;
    proc_self_io.ref_count = 1;
    
    memset(&proc_stat, 0, sizeof(vfs_node_t));
    strcpy(proc_stat.name, "stat");
    proc_stat.flags = VFS_FILE;
    proc_stat.read = proc_stat_read;
    proc_stat.parent = &procfs_root;
    proc_stat.ref_count = 1;
    
    memset(&proc_mounts, 0, sizeof(vfs_node_t));
    strcpy(proc_mounts.name, "mounts");
    proc_mounts.flags = VFS_FILE;
    proc_mounts.read = proc_mounts_read;
    proc_mounts.parent = &procfs_root;
    proc_mounts.ref_count = 1;
    
    memset(&proc_filesystems, 0, sizeof(vfs_node_t));
    strcpy(proc_filesystems.name, "filesystems");
    proc_filesystems.flags = VFS_FILE;
    proc_filesystems.read = proc_filesystems_read;
    proc_filesystems.parent = &procfs_root;
    proc_filesystems.ref_count = 1;
    
    memset(&proc_cmdline, 0, sizeof(vfs_node_t));
    strcpy(proc_cmdline.name, "cmdline");
    proc_cmdline.flags = VFS_FILE;
    proc_cmdline.read = proc_cmdline_read;
    proc_cmdline.parent = &procfs_root;
    proc_cmdline.ref_count = 1;
    
    memset(&proc_devices, 0, sizeof(vfs_node_t));
    strcpy(proc_devices.name, "devices");
    proc_devices.flags = VFS_FILE;
    proc_devices.read = proc_devices_read;
    proc_devices.parent = &procfs_root;
    proc_devices.ref_count = 1;
    
    memset(&proc_interrupts, 0, sizeof(vfs_node_t));
    strcpy(proc_interrupts.name, "interrupts");
    proc_interrupts.flags = VFS_FILE;
    proc_interrupts.read = proc_interrupts_read;
    proc_interrupts.parent = &procfs_root;
    proc_interrupts.ref_count = 1;
    
    memset(&proc_vmstat, 0, sizeof(vfs_node_t));
    strcpy(proc_vmstat.name, "vmstat");
    proc_vmstat.flags = VFS_FILE;
    proc_vmstat.read = proc_vmstat_read;
    proc_vmstat.parent = &procfs_root;
    proc_vmstat.ref_count = 1;
    
    memset(&proc_memdetail, 0, sizeof(vfs_node_t));
    strcpy(proc_memdetail.name, "memdetail");
    proc_memdetail.flags = VFS_FILE;
    proc_memdetail.read = proc_memdetail_read;
    proc_memdetail.parent = &procfs_root;
    proc_memdetail.ref_count = 1;
    
    vfs_register_fs(&procfs_type);
}
