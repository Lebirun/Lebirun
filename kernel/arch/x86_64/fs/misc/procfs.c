#include <lebirun/mem_map.h>
#include <lebirun/vfs.h>
#include <lebirun/task.h>
#include <lebirun/about.h>
#include <lebirun/cmdline.h>
#include <lebirun/drivers/net/e1000/e1000.h>
#include <lebirun/drivers/sata/ahci.h>
#include <lebirun/rtc.h>
#include <lebirun/vring.h>
#include <lebirun/overlayfs.h>
#include <lebirun/squashfs.h>
#include <lebirun/kstack.h>
#include <lebirun/spinlock.h>
#include <lebirun/common.h>
#include <lebirun/smp.h>
#include <lebirun/creds.h>
#include <string.h>
#include <stdio.h>

extern task_t *current_task;
extern task_t *ready_queue_head;

static vfs_node_t procfs_root;
static vfs_node_t *proc_self;
static vfs_node_t *proc_self_exe;
static vfs_node_t *proc_self_cwd;
static vfs_node_t *proc_self_fd;
static vfs_node_t *proc_self_maps;
static vfs_node_t *proc_self_status;
static vfs_node_t *proc_self_cmdline;
static vfs_node_t *proc_self_environ;
static vfs_node_t *proc_self_stat;
static vfs_node_t *proc_self_statm;
static vfs_node_t *proc_self_comm;
static vfs_node_t *proc_self_limits;
static vfs_node_t *proc_self_io;
static vfs_node_t *proc_version;
static vfs_node_t *proc_uptime;
static vfs_node_t *proc_meminfo;
static vfs_node_t *proc_cpuinfo;
static vfs_node_t *proc_loadavg;
static vfs_node_t *proc_stat;
static vfs_node_t *proc_mounts;
static vfs_node_t *proc_filesystems;
static vfs_node_t *proc_cmdline;
static vfs_node_t *proc_devices;
static vfs_node_t *proc_interrupts;
static vfs_node_t *proc_vmstat;
static vfs_node_t *proc_memdetail;
static vfs_node_t *proc_kmsg;

static dirent_t proc_dirent;
static dirent_t proc_self_dirent;
static spinlock_t proc_mem_report_lock = {0};

typedef struct proc_stream {
    uint64_t offset;
    uint64_t size;
    uint64_t position;
    uint64_t copied;
    uint8_t *buffer;
} proc_stream_t;

static dirent_t *proc_task_readdir(vfs_node_t *node, uint64_t index);
static vfs_node_t *proc_task_finddir(vfs_node_t *node, const char *name);
static dirent_t *proc_task_thread_readdir(vfs_node_t *node, uint64_t index);
static vfs_node_t *proc_task_thread_finddir(vfs_node_t *node, const char *name);
static vfs_node_t *procfs_get_self(void);
static vfs_node_t *procfs_get_self_exe(void);
static vfs_node_t *procfs_get_self_cwd(void);
static vfs_node_t *procfs_get_self_fd(void);
static vfs_node_t *procfs_get_self_maps(void);
static vfs_node_t *procfs_get_self_status(void);
static vfs_node_t *procfs_get_self_cmdline(void);
static vfs_node_t *procfs_get_self_environ(void);
static vfs_node_t *procfs_get_self_stat(void);
static vfs_node_t *procfs_get_self_statm(void);
static vfs_node_t *procfs_get_self_comm(void);
static vfs_node_t *procfs_get_self_limits(void);
static vfs_node_t *procfs_get_self_io(void);
static vfs_node_t *procfs_get_version(void);
static vfs_node_t *procfs_get_uptime(void);
static vfs_node_t *procfs_get_meminfo(void);
static vfs_node_t *procfs_get_cpuinfo(void);
static vfs_node_t *procfs_get_loadavg(void);
static vfs_node_t *procfs_get_stat(void);
static vfs_node_t *procfs_get_mounts(void);
static vfs_node_t *procfs_get_filesystems(void);
static vfs_node_t *procfs_get_cmdline(void);
static vfs_node_t *procfs_get_devices(void);
static vfs_node_t *procfs_get_interrupts(void);
static vfs_node_t *procfs_get_vmstat(void);
static vfs_node_t *procfs_get_memdetail(void);
static vfs_node_t *procfs_get_kmsg(void);

static void procfs_init_node(vfs_node_t *n, const char *name, uint64_t flags, vfs_node_t *parent) {
    uint64_t type;

    memset(n, 0, sizeof(vfs_node_t));
    strcpy(n->name, name);
    n->flags = flags;
    n->parent = parent;
    n->ref_count = 1;
    type = VFS_GET_TYPE(flags);
    if (type == VFS_DIRECTORY) {
        n->mask = 0555;
    } else if (type == VFS_SYMLINK) {
        n->mask = 0777;
    } else {
        n->mask = 0444;
    }
}

static vfs_node_t *procfs_lazy_node(vfs_node_t **slot, const char *name, uint64_t flags, vfs_node_t *parent, read_type_t read, readdir_type_t readdir, finddir_type_t finddir) {
    vfs_node_t *n;

    if (*slot) return *slot;
    if (!parent) return NULL;
    n = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!n) return NULL;
    procfs_init_node(n, name, flags, parent);
    n->read = read;
    n->readdir = readdir;
    n->finddir = finddir;
    *slot = n;
    return n;
}

static task_t *procfs_get_task(vfs_node_t *node) {
    pid_t pid;
    int64_t inode;

    inode = (int64_t)node->inode;
    if (VFS_GET_TYPE(node->flags) == VFS_FILE && inode >= 100) {
        pid = (pid_t)(inode / 100);
    } else if (VFS_GET_TYPE(node->flags) == VFS_FILE && inode < 0) {
        pid = (pid_t)((inode - 99) / 100);
    } else {
        pid = (pid_t)inode;
    }
    if (pid == 0 && current_task)
        return current_task;
    return task_find(pid);
}

extern volatile uint64_t tick_count;
extern uint64_t pit_freq;
extern uint64_t pt_vmm_pt_count;
extern uint64_t pt_get_heap_pt_count(void);
extern void fb_reclaim_unused(void);
extern void console_reclaim_unused(void);
extern void console_memory_stats(uint64_t *buffers, uint64_t *bytes);
extern void slab_reclaim_empty(void);
extern void kstack_reclaim_unused(void);
extern void heap_reclaim_unused(void);

static void proc_collect_memory_report(void) {
    task_memory_collect_for_report();
    fb_reclaim_unused();
    console_reclaim_unused();
    slab_gc();
    slab_reclaim_empty();
    kstack_reclaim_unused();
    heap_reclaim_unused();
    pfa_ref_gc();
    exec_page_cache_reclaim(0);
    overlay_flush_cache();
    squashfs_flush_cache();
    slab_reclaim_empty();
    heap_reclaim_unused();
    pfa_ref_gc();
}

static void proc_stream_append(proc_stream_t *stream, const char *text,
                               uint64_t length) {
    uint64_t request_end;
    uint64_t text_start;
    uint64_t text_end;
    uint64_t copy_start;
    uint64_t copy_end;
    uint64_t copy_length;

    if (!stream || !text) return;
    text_start = stream->position;
    text_end = text_start + length;
    request_end = stream->offset + stream->size;
    if (request_end < stream->offset) request_end = UINT64_MAX;
    copy_start = text_start > stream->offset ? text_start : stream->offset;
    copy_end = text_end < request_end ? text_end : request_end;
    if (copy_end > copy_start && stream->buffer) {
        copy_length = copy_end - copy_start;
        memcpy(stream->buffer + stream->copied,
               text + copy_start - text_start, copy_length);
        stream->copied += copy_length;
    }
    stream->position = text_end;
}

static void proc_stream_value(proc_stream_t *stream, const char *format,
                              uint64_t value) {
    char line[64];
    int length;

    length = snprintf(line, sizeof(line), format, value);
    if (length <= 0) return;
    if ((uint64_t)length >= sizeof(line)) length = sizeof(line) - 1;
    proc_stream_append(stream, line, (uint64_t)length);
}

static void proc_format_octal4(char *output, uint64_t value) {
    int index;

    for (index = 3; index >= 0; index--) {
        output[index] = (char)('0' + (value & 7));
        value >>= 3;
    }
    output[4] = '\0';
}

static uint64_t proc_self_status_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[2048];
    char umask_text[5];
    int len;
    uint64_t remaining;
    task_t *task;
    uint64_t ruid, rgid;
    uint64_t vm_kb;
    uint64_t data_kb;
    uint64_t rss_kb;
    uint64_t pending;
    uint64_t blocked;
    unsigned long signal_limit;

    len = 0;
    task = procfs_get_task(node);
    
    if (task) {
        ruid = task->uid;
        rgid = task->gid;
        if (!task->is_user) {
            ruid = 999;
            rgid = 999;
        }
        vm_kb = task_user_memory_bytes(task) / 1024;
        rss_kb = task->user_pages_count * 4;
        data_kb = task->user_brk > task->user_brk_start ?
                  (task->user_brk - task->user_brk_start) / 1024 : 0;
        pending = signal_pending_mask(task);
        blocked = signal_blocked_mask(task);
        signal_limit = task_rlimit_get(task, 11, 0);
        proc_format_octal4(umask_text, task->creation_mask);
        len = snprintf(buf, sizeof(buf),
            "Name:\t%.15s\n"
            "Umask:\t%s\n"
            "State:\t%c (%s)\n"
            "Tgid:\t%d\n"
            "Ngid:\t0\n"
            "Pid:\t%d\n"
            "PPid:\t%d\n"
            "TracerPid:\t0\n"
            "Uid:\t%lu\t%lu\t%lu\t%lu\n"
            "Gid:\t%lu\t%lu\t%lu\t%lu\n"
            "FDSize:\t%d\n"
            "Groups:\t%lu\n"
            "VmPeak:\t%lu kB\n"
            "VmSize:\t%lu kB\n"
            "VmHWM:\t%lu kB\n"
            "VmRSS:\t%lu kB\n"
            "VmData:\t%lu kB\n"
            "VmStk:\t%lu kB\n"
            "Threads:\t1\n"
            "SigQ:\t%u/%lu\n"
            "SigPnd:\t%016lx\n"
            "SigBlk:\t%016lx\n"
            "Seccomp:\t%d\n"
            "KernelTask:\t%d\n",
            task->name[0] ? task->name : "unknown",
            umask_text,
            task->state == TASK_RUNNING ? 'R' : 
            task->state == TASK_BLOCKED ? 'S' : 'Z',
            task->state == TASK_RUNNING ? "running" :
            task->state == TASK_BLOCKED ? "sleeping" : "zombie",
            task->pid,
            task->pid,
            task->ppid,
            ruid, task->euid, task->suid, task->fsuid,
            rgid, task->egid, task->sgid, task->fsgid,
            task->fds_capacity,
            rgid,
            vm_kb,
            vm_kb,
            task->maxrss_kb,
            rss_kb,
            data_kb,
            task->stack_size / 1024,
            signal_queue_count(task),
            signal_limit,
            pending,
            blocked,
            creds_get_syscall_filter_mode(task),
            task->is_kernel_task ? 1 : 0);
    }
    
    if (offset >= (uint64_t)len) return 0;
    remaining = (uint64_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint64_t proc_self_maps_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[2048];
    char path[VFS_MAX_PATH];
    const char *display;
    int len;
    int n;
    int i;
    uint64_t remaining;
    uint64_t end;
    task_t *task;
    
    len = 0;
    task = procfs_get_task(node);
    
    if (task) {
        for (i = 0; i < task->file_map_count &&
             len < (int)sizeof(buf) - 1; i++) {
            end = task->file_maps[i].vaddr + task->file_maps[i].memsz;
            display = "";
            if (task->file_maps[i].node &&
                vfs_get_path(task->file_maps[i].node, path,
                             sizeof(path)))
                display = path;
            else if (task->file_maps[i].map_flags & TASK_VMA_ANONYMOUS)
                display = "[anon]";
            n = snprintf(buf + len, sizeof(buf) - (size_t)len,
                "%016lx-%016lx %c%c%c%c %08lx 00:00 %lu%s%s\n",
                task->file_maps[i].vaddr, end,
                task->file_maps[i].flags & 1 ? 'r' : '-',
                task->file_maps[i].flags & 2 ? 'w' : '-',
                task->file_maps[i].flags & 4 ? 'x' : '-',
                task->file_maps[i].map_flags & TASK_VMA_SHARED ? 's' : 'p',
                task->file_maps[i].offset,
                task->file_maps[i].node ? task->file_maps[i].node->inode : 0,
                display[0] ? " " : "", display);
            if (n < 0) break;
            if (n >= (int)sizeof(buf) - len) {
                len = (int)sizeof(buf) - 1;
                break;
            }
            len += n;
        }
    }
    if (task && task->user_brk > task->user_brk_start) {
        if (len < (int)sizeof(buf) - 1) {
            n = snprintf(buf + len, sizeof(buf) - (size_t)len,
                "%016lx-%016lx rw-p 00000000 00:00 0 [heap]\n",
                task->user_brk_start, task->user_brk);
            if (n > 0) len += n;
            if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
        }
    }
    if (task && task->stack_size > 0) {
        if (len < (int)sizeof(buf) - 1) {
            n = snprintf(buf + len, sizeof(buf) - (size_t)len,
                "%016lx-%016lx rw-p 00000000 00:00 0 [stack]\n",
                USER_STACK_TOP - task->stack_size,
                (uint64_t)USER_STACK_TOP);
            if (n > 0) len += n;
            if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
        }
    }
    
    if (offset >= (uint64_t)len) return 0;
    remaining = (uint64_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint64_t proc_self_cmdline_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char cmdline[128];
    uint64_t len;
    uint64_t remaining;
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

static uint64_t proc_self_environ_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    const char *env;
    uint64_t len;
    uint64_t remaining;
    
    (void)node;
    
    env = "PATH=/bin:/usr/bin\0HOME=/\0TERM=linux\0";
    len = 38;
    
    if (offset >= len) return 0;
    remaining = len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, env + offset, size);
    return size;
}

static uint64_t proc_kmsg_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    int len;
    uint64_t remaining;

    (void)node;

    len = klog_snapshot(NULL, 0);
    if (len <= 0) return 0;
    if (offset >= (uint64_t)len) return 0;
    remaining = (uint64_t)len - offset;
    if (size > remaining) size = remaining;
    klog_snapshot_range((char *)buffer, (int)offset, (int)size);
    return size;
}

static uint64_t proc_version_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char ver[128];
    int len;
    uint64_t remaining;
    
    (void)node;
    
    len = snprintf(ver, sizeof(ver), "%s version %s (%s %s) #1 SMP PREEMPT\n",
        SYSNAME, RELEASE, KERNEL_BUILD_DATE, KERNEL_BUILD_TIME);
    
    if (offset >= (uint64_t)len) return 0;
    remaining = (uint64_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, ver + offset, size);
    return size;
}

static uint64_t proc_uptime_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[64];
    uint64_t seconds;
    int len;
    uint64_t remaining;
    
    (void)node;
    
    seconds = tick_count / pit_freq;
    len = snprintf(buf, sizeof(buf), "%lu.%02lu %lu.%02lu\n", 
        seconds, (tick_count % pit_freq) * 100 / pit_freq,
        seconds, 0UL);
    
    if (offset >= (uint64_t)len) return 0;
    remaining = (uint64_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint64_t proc_meminfo_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[768];
    uint64_t total_kb;
    uint64_t free_pages_kb;
    uint64_t used_kb;
    uint64_t mem_used_kb;
    uint64_t heap_total_kb;
    uint64_t heap_used_kb;
    uint64_t slab_kb;
    uint64_t exec_cache_kb;
    uint64_t exec_reclaim_kb;
    uint64_t avail_kb;
    int len;
    uint64_t remaining;

    (void)node;

    if (offset == 0) proc_collect_memory_report();

    total_kb = pfa_get_total_ram_kb();
    free_pages_kb = pfa_count_free() * 4;
    used_kb = total_kb > free_pages_kb ? total_kb - free_pages_kb : 0;
    heap_total_kb = kernel_heap.total_size / 1024;
    heap_used_kb = kernel_heap.used_size / 1024;
    slab_kb = slab_get_total_pages() * 4;
    exec_cache_kb = exec_page_cache_get_pages() * 4;
    exec_reclaim_kb = exec_page_cache_get_reclaimable_pages() * 4;
    avail_kb = free_pages_kb + exec_reclaim_kb;
    if (avail_kb > total_kb) avail_kb = total_kb;
    mem_used_kb = used_kb > exec_cache_kb ? used_kb - exec_cache_kb : 0;

    len = snprintf(buf, sizeof(buf),
        "MemTotal:      %8lu kB\n"
        "MemFree:       %8lu kB\n"
        "MemAvailable:  %8lu kB\n"
        "MemUsed:       %8lu kB\n"
        "MemAllUsed:    %8lu kB\n"
        "Buffers:       %8lu kB\n"
        "Cached:        %8lu kB\n"
        "SwapCached:    %8lu kB\n"
        "SwapTotal:     %8lu kB\n"
        "SwapFree:      %8lu kB\n"
        "Shmem:         %8lu kB\n"
        "SReclaimable:  %8lu kB\n"
        "HeapTotal:     %8lu kB\n"
        "HeapUsed:      %8lu kB\n"
        "Slab:          %8lu kB\n",
        total_kb,
        free_pages_kb,
        avail_kb,
        mem_used_kb,
        used_kb,
        0UL,
        exec_cache_kb,
        0UL,
        0UL,
        0UL,
        0UL,
        exec_reclaim_kb,
        heap_total_kb,
        heap_used_kb,
        slab_kb);
    
    if (offset >= (uint64_t)len) return 0;
    remaining = (uint64_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint64_t proc_cpuinfo_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char *info;
    char vendor[13];
    size_t capacity;
    uint64_t len;
    uint64_t remaining;
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    uint32_t family;
    uint32_t model;
    int written;
    int i;
    
    (void)node;
    
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx),
                     "=d"(edx) : "a"(0));
    memcpy(vendor, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);
    vendor[12] = '\0';
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx),
                     "=d"(edx) : "a"(1));
    family = (eax >> 8) & 0xF;
    if (family == 0xF) family += (eax >> 20) & 0xFF;
    model = (eax >> 4) & 0xF;
    if (family == 6 || family == 15) model |= ((eax >> 16) & 0xF) << 4;
    capacity = (size_t)(cpu_count > 0 ? cpu_count : 1) * 160;
    info = (char *)kmalloc(capacity);
    if (!info) return 0;
    len = 0;
    for (i = 0; i < (cpu_count > 0 ? cpu_count : 1); i++) {
        written = snprintf(info + len, capacity - len,
            "processor\t: %d\n"
            "vendor_id\t: %s\n"
            "cpu family\t: %u\n"
            "model\t\t: %u\n"
            "stepping\t: %u\n"
            "apicid\t\t: %lu\n\n",
            i, vendor, family, model, eax & 0xF,
            cpus && i < cpu_count ? cpus[i].lapic_id : 0);
        if (written < 0) break;
        len += (uint64_t)written;
        if (len >= capacity) {
            len = capacity;
            break;
        }
    }
    
    if (offset >= len) {
        kfree(info);
        return 0;
    }
    remaining = len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, info + offset, size);
    kfree(info);
    return size;
}

static uint64_t proc_loadavg_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[64];
    int len;
    uint64_t remaining;
    
    (void)node;
    
    len = snprintf(buf, sizeof(buf), "0.00 0.00 0.00 1/1 %d\n",
        current_task ? current_task->pid : 1);
    
    if (offset >= (uint64_t)len) return 0;
    remaining = (uint64_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint64_t proc_self_stat_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[512];
    int len;
    uint64_t remaining;
    task_t *task;
    uint64_t utime_val;
    uint64_t stime_val;
    uint64_t starttime_val;
    uint64_t vsize_val;
    uint64_t rss_val;
    
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
            "%d (%s) %c %d %d %d 0 -1 0 0 0 0 0 %lu %lu 0 0 20 0 1 0 %lu %lu %lu -1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
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
    
    if (offset >= (uint64_t)len) return 0;
    remaining = (uint64_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint64_t proc_self_statm_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[128];
    int len;
    uint64_t pages;
    uint64_t remaining;
    task_t *task;
    
    len = 0;
    task = procfs_get_task(node);
    
    if (task) {
        pages = task->user_pages_count;
        len = snprintf(buf, sizeof(buf), "%lu %lu %lu 1 0 %lu 0\n",
            pages, pages, pages / 2, pages);
    }
    
    if (offset >= (uint64_t)len) return 0;
    remaining = (uint64_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint64_t proc_self_comm_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[32];
    int len;
    uint64_t remaining;
    task_t *task;
    
    len = 0;
    task = procfs_get_task(node);
    
    if (task) {
        len = snprintf(buf, sizeof(buf), "%s\n",
            task->name[0] ? task->name : "unknown");
    }
    
    if (offset >= (uint64_t)len) return 0;
    remaining = (uint64_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint64_t proc_self_limits_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    static const char *names[] = {
        "Max cpu time", "Max file size", "Max data size",
        "Max stack size", "Max core file size", "Max resident set",
        "Max processes", "Max open files", "Max locked memory",
        "Max address space", "Max file locks", "Max pending signals",
        "Max msgqueue size", "Max nice priority",
        "Max realtime priority", "Max realtime timeout"
    };
    static const char *units[] = {
        "seconds", "bytes", "bytes", "bytes", "bytes", "bytes",
        "processes", "files", "bytes", "bytes", "locks", "signals",
        "bytes", "", "", "us"
    };
    char buf[2048];
    char soft_text[32];
    char hard_text[32];
    uint64_t len;
    uint64_t remaining;
    unsigned long soft;
    unsigned long hard;
    task_t *task;
    int written;
    int resource;

    task = procfs_get_task(node);
    if (!task) return 0;
    written = snprintf(buf, sizeof(buf),
        "Limit                     Soft Limit           Hard Limit           Units     \n");
    if (written < 0) return 0;
    len = (uint64_t)written;
    for (resource = 0; resource < 16 && len < sizeof(buf); resource++) {
        soft = task_rlimit_get(task, resource, 0);
        hard = task_rlimit_get(task, resource, 1);
        if (soft == (unsigned long)-1)
            strcpy(soft_text, "unlimited");
        else
            snprintf(soft_text, sizeof(soft_text), "%lu", soft);
        if (hard == (unsigned long)-1)
            strcpy(hard_text, "unlimited");
        else
            snprintf(hard_text, sizeof(hard_text), "%lu", hard);
        written = snprintf(buf + len, sizeof(buf) - len,
            "%-25s %-20s %-20s %s\n", names[resource], soft_text,
            hard_text, units[resource]);
        if (written < 0) break;
        if ((uint64_t)written >= sizeof(buf) - len) {
            len = sizeof(buf) - 1;
            break;
        }
        len += (uint64_t)written;
    }
    
    if (offset >= len) return 0;
    remaining = len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint64_t proc_self_io_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[256];
    uint64_t len;
    uint64_t remaining;
    task_t *task;
    int written;
    
    task = procfs_get_task(node);
    if (!task) return 0;
    written = snprintf(buf, sizeof(buf),
        "rchar: %lu\n"
        "wchar: %lu\n"
        "syscr: %lu\n"
        "syscw: %lu\n"
        "read_bytes: %lu\n"
        "write_bytes: %lu\n"
        "cancelled_write_bytes: 0\n",
        task->read_characters, task->written_characters,
        task->read_calls, task->write_calls,
        task->file_read_bytes, task->file_write_bytes);
    if (written < 0) return 0;
    len = (uint64_t)written;
    
    if (offset >= len) return 0;
    remaining = len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint64_t proc_stat_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char *buf;
    size_t capacity;
    int len;
    int written;
    int i;
    int process_count;
    int blocked;
    uint64_t remaining;
    static uint64_t cached_btime = 0;
    uint64_t btime;
    extern volatile uint64_t cpu_user_ticks;
    extern volatile uint64_t cpu_system_ticks;
    extern volatile uint64_t cpu_idle_ticks;
    uint64_t user_t;
    uint64_t sys_t;
    uint64_t idle_t;
    uint64_t running;
    task_t *t;
    task_t *start;
    extern volatile uint64_t task_context_switches;
    
    (void)node;
    
    user_t = cpu_user_ticks;
    sys_t = cpu_system_ticks;
    idle_t = cpu_idle_ticks;
    
    running = 0;
    blocked = 0;
    start = ready_queue_head;
    t = start;
    if (t) {
        do {
            if (t->state == TASK_RUNNING || t->state == TASK_READY)
                running++;
            if (t->state == TASK_BLOCKED) blocked++;
            t = t->next;
        } while (t && t != start);
    }
    if (running == 0) running = 1;
    
    if (cached_btime == 0)
        cached_btime = rtc_get_time();
    btime = cached_btime;
    
    process_count = 0;
    task_get_cached_stats(&process_count, NULL, NULL, NULL);
    capacity = (size_t)(cpu_count + 10) * 96;
    buf = (char *)kmalloc(capacity);
    if (!buf) return 0;
    len = snprintf(buf, capacity,
        "cpu  %lu 0 %lu %lu 0 0 0 0 0 0\n",
        user_t, sys_t, idle_t);
    for (i = 0; i < cpu_count && len < (int)capacity; i++) {
        written = snprintf(buf + len, capacity - (size_t)len,
            "cpu%d %lu 0 %lu %lu 0 0 0 0 0 0\n", i,
            cpus ? cpus[i].user_ticks : 0,
            cpus ? cpus[i].system_ticks : 0,
            cpus ? cpus[i].idle_ticks : 0);
        if (written < 0) break;
        len += written;
    }
    if (len < (int)capacity) {
        written = snprintf(buf + len, capacity - (size_t)len,
        "intr %lu 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n"
        "ctxt %lu\n"
        "btime %lu\n"
        "processes %d\n"
        "procs_running %lu\n"
        "procs_blocked %d\n"
        "softirq 0 0 0 0 0 0 0 0 0 0 0\n",
        tick_count,
        task_context_switches,
        btime,
        process_count,
        running,
        blocked);
        if (written > 0) len += written;
    }
    if (len > (int)capacity) len = (int)capacity;
    
    if (offset >= (uint64_t)len) {
        kfree(buf);
        return 0;
    }
    remaining = (uint64_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    kfree(buf);
    return size;
}

static uint64_t proc_mounts_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
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
    uint64_t available;
    
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

    available = (uint64_t)(len - offset);
    if (size > available) {
        size = available;
    }
    memcpy(buffer, buf + offset, size);
    kfree(buf);
    return size;
}

static uint64_t proc_filesystems_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    const char *buf;
    uint64_t len;
    uint64_t remaining;
    
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

static uint64_t proc_cmdline_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    const char *raw;
    const char *source;
    uint64_t len;
    uint64_t total;
    uint64_t copied;
    uint64_t chunk;
    
    (void)node;
    
    raw = cmdline_get();
    source = raw[0] ? raw : "console=tty0 root=/dev/ram0 rw";
    len = strlen(source);
    total = len + 1;
    if (offset >= total || size == 0) return 0;
    if (size > total - offset) size = total - offset;
    copied = 0;
    if (offset < len) {
        chunk = len - offset;
        if (chunk > size) chunk = size;
        memcpy(buffer, source + offset, chunk);
        copied = chunk;
    }
    if (copied < size) buffer[copied++] = '\n';
    return copied;
}

static uint64_t proc_devices_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    const char *buf;
    uint64_t len;
    uint64_t remaining;
    
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

static uint64_t proc_interrupts_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[512];
    int len;
    uint64_t remaining;
    
    (void)node;
    
    len = snprintf(buf, sizeof(buf),
        "           CPU0\n"
        "  0:    %8lu   PIT timer\n"
        "  1:    %8lu   i8042 keyboard\n"
        " 14:    %8lu   IDE\n"
        " 15:    %8lu   IDE\n"
        "NMI:           0   Non-maskable interrupts\n"
        "LOC:           0   Local timer interrupts\n"
        "ERR:           0\n",
        tick_count, tick_count / 100, 0UL, 0UL);
    
    if (offset >= (uint64_t)len) return 0;
    remaining = (uint64_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint64_t proc_vmstat_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    extern uint64_t pfa_count_free(void);
    uint64_t free_pages;
    char buf[1024];
    int len;
    uint64_t remaining;
    
    (void)node;
    
    free_pages = pfa_count_free();
    
    len = snprintf(buf, sizeof(buf),
        "nr_free_pages %lu\n"
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
    
    if (offset >= (uint64_t)len) return 0;
    remaining = (uint64_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint64_t proc_memdetail_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    proc_stream_t stream;
    uint64_t pfa_used_kb;
    uint64_t kern_kb;
    uint64_t bitmap_kb;
    uint64_t mem_all_used_kb;
    uint64_t free_pages_kb;
    uint64_t kernel_reclaimed_pages;
    uint64_t demand_bitmap_bytes;
    uint64_t demand_bitmap_extension_pages;
    uint64_t heap_committed;
    uint64_t heap_reserved;
    uint64_t heap_used;
    uint64_t heap_total;
    uint64_t slab_pages;
    uint64_t e1000_pages;
    uint64_t ahci_pages;
    uint64_t pt_pt_pages;
    uint64_t pt_heap_pt;
    uint64_t user_stack_pages;
    uint64_t user_pd_pages;
    uint64_t exec_cache_pages;
    uint64_t exec_reclaim_pages;
    uint64_t active_elf_pages;
    uint64_t current_elf_pages;
    uint64_t dead_elf_pages;
    uint64_t overlay_nodes;
    uint64_t overlay_capacity;
    uint64_t overlay_bytes;
    uint64_t sqfs_nodes;
    uint64_t sqfs_capacity;
    uint64_t sqfs_bytes;
    uint64_t sqfs_data_bytes;
    uint64_t sqfs_decomp_failures;
    uint64_t sqfs_decomp_oversize;
    uint64_t sqfs_decomp_padded;
    uint64_t sqfs_module_pages;
    uint64_t ref_active_nodes;
    uint64_t ref_free_nodes;
    uint64_t early_heap_total_kb;
    uint64_t early_heap_used_kb;
    uint64_t console_buffers;
    uint64_t console_bytes;
    uint64_t kstack_slots;
    uint64_t kstack_pages;
    uint64_t current_exec_reclaimed;
    uint64_t partial_exec_reclaimed;
    uint64_t profile_start;
    uint64_t profile_offset;
    uint64_t profile_size;
    uint64_t request_end;
    task_mem_stats_t task_stats;

    (void)node;

    if (offset == 0) proc_collect_memory_report();

    spin_lock(&proc_mem_report_lock);
    task_get_memory_stats(&task_stats);
    overlay_cache_stats(&overlay_nodes, &overlay_capacity, &overlay_bytes);
    squashfs_cache_stats(&sqfs_nodes, &sqfs_capacity, &sqfs_bytes, &sqfs_data_bytes);
    squashfs_decomp_stats(&sqfs_decomp_failures, &sqfs_decomp_oversize, &sqfs_decomp_padded);
    console_memory_stats(&console_buffers, &console_bytes);
    kstack_memory_stats(&kstack_slots, &kstack_pages);
    ref_active_nodes = pfa_ref_active_nodes();
    ref_free_nodes = pfa_ref_free_nodes();

    pfa_used_kb = pfa_get_kernel_used_kb();
    kern_kb = pfa_get_kernel_binary_kb();
    bitmap_kb = pfa_get_bitmap_kb();
    free_pages_kb = pfa_count_free() * 4;
    if (pfa_get_total_ram_kb() > free_pages_kb)
        mem_all_used_kb = pfa_get_total_ram_kb() - free_pages_kb;
    else
        mem_all_used_kb = 0;
    kernel_reclaimed_pages = pfa_get_kernel_reclaimed_pages();
    early_heap_total_kb = heap_get_early_total() / 1024;
    early_heap_used_kb = heap_get_early_used() / 1024;

    heap_committed = demand_get_committed_pages();
    heap_reserved = demand_get_reserved_pages();
    demand_bitmap_bytes = demand_get_bitmap_bytes();
    demand_bitmap_extension_pages = demand_get_bitmap_extension_pages();
    heap_used = kernel_heap.used_size;
    heap_total = kernel_heap.total_size;

    slab_pages = slab_get_total_pages();

    e1000_pages = e1000_get_allocated_pages();

    ahci_pages = ahci_get_allocated_pages();
    sqfs_module_pages = squashfs_get_module_pages();

    pt_pt_pages = 0;
    pt_heap_pt = 0;
    pt_pt_pages = pt_vmm_pt_count;
    pt_heap_pt = pt_get_heap_pt_count();

    user_stack_pages = 0;
    user_pd_pages = 0;
    exec_cache_pages = exec_page_cache_get_pages();
    exec_reclaim_pages = exec_page_cache_get_reclaimable_pages();

    user_stack_pages = task_stats.active_stack_pages;
    user_pd_pages = task_stats.active_pd_pages;
    active_elf_pages = 0;
    current_elf_pages = 0;
    dead_elf_pages = task_stats.dead_file_pages;
    active_elf_pages = task_stats.active_file_pages;
    current_elf_pages = task_stats.current_file_pages;
    current_exec_reclaimed = task_memory_report_current_exec_reclaimed();
    partial_exec_reclaimed = task_memory_report_partial_exec_reclaimed();
    spin_unlock(&proc_mem_report_lock);

    stream.offset = offset;
    stream.size = size;
    stream.position = 0;
    stream.copied = 0;
    stream.buffer = buffer;

#define MEMDETAIL_VALUE(format, value) \
    proc_stream_value(&stream, (format), (uint64_t)(value))

    MEMDETAIL_VALUE("MemAllUsedKB:       %8lu\n", mem_all_used_kb);
    MEMDETAIL_VALUE("PFANetAllocatedKB:  %8lu\n", pfa_used_kb);
    MEMDETAIL_VALUE("KernelImageKB:      %8lu\n", kern_kb);
    MEMDETAIL_VALUE("KernelReclaimedPages:%7lu\n", kernel_reclaimed_pages);
    MEMDETAIL_VALUE("KernelReclaimedKB:  %8lu\n", kernel_reclaimed_pages * 4);
    MEMDETAIL_VALUE("BitmapKB:           %8lu\n", bitmap_kb);
    MEMDETAIL_VALUE("DemandBitmapBytes:  %8lu\n", demand_bitmap_bytes);
    MEMDETAIL_VALUE("DemandBitmapExtPages:%6lu\n", demand_bitmap_extension_pages);
    MEMDETAIL_VALUE("DemandBitmapExtKB:  %8lu\n", demand_bitmap_extension_pages * 4);
    MEMDETAIL_VALUE("EarlyHeapTotalKB:   %8lu\n", early_heap_total_kb);
    MEMDETAIL_VALUE("EarlyHeapUsedKB:    %8lu\n", early_heap_used_kb);
    MEMDETAIL_VALUE("HeapCommitPages:    %8lu\n", heap_committed);
    MEMDETAIL_VALUE("HeapCommitKB:       %8lu\n", heap_committed * 4);
    MEMDETAIL_VALUE("HeapReservePages:   %8lu\n", heap_reserved);
    MEMDETAIL_VALUE("HeapAllocatedBytes: %8lu\n", heap_used);
    MEMDETAIL_VALUE("HeapVirtualSpanBytes:%7lu\n", heap_total);
    MEMDETAIL_VALUE("TaskCount:          %8lu\n", task_stats.task_count);
    MEMDETAIL_VALUE("TaskStructBytes:    %8lu\n", task_stats.task_struct_bytes);
    MEMDETAIL_VALUE("TaskFPUBytes:       %8lu\n", task_stats.task_fpu_bytes);
    MEMDETAIL_VALUE("TaskFDBytes:        %8lu\n", task_stats.task_fd_bytes);
    MEMDETAIL_VALUE("TaskPageArrayBytes: %8lu\n", task_stats.task_page_array_bytes);
    MEMDETAIL_VALUE("TaskFileMapBytes:   %8lu\n", task_stats.task_file_map_bytes);
    MEMDETAIL_VALUE("KernelStackSlots:   %8lu\n", kstack_slots);
    MEMDETAIL_VALUE("KernelStackPages:   %8lu\n", kstack_pages);
    MEMDETAIL_VALUE("KernelStackKB:      %8lu\n", kstack_pages * 4);
    MEMDETAIL_VALUE("SlabPages:          %8lu\n", slab_pages);
    MEMDETAIL_VALUE("SlabKB:             %8lu\n", slab_pages * 4);
    MEMDETAIL_VALUE("E1000Pages:         %8lu\n", e1000_pages);
    MEMDETAIL_VALUE("E1000KB:            %8lu\n", e1000_pages * 4);
    MEMDETAIL_VALUE("AHCIPages:          %8lu\n", ahci_pages);
    MEMDETAIL_VALUE("AHCIKB:             %8lu\n", ahci_pages * 4);
    MEMDETAIL_VALUE("PT_VMMPTPages:     %8lu\n", pt_pt_pages);
    MEMDETAIL_VALUE("PT_VMMPTKB:        %8lu\n", pt_pt_pages * 4);
    MEMDETAIL_VALUE("PT_HeapPTPages:    %8lu\n", pt_heap_pt);
    MEMDETAIL_VALUE("PT_HeapPTKB:       %8lu\n", pt_heap_pt * 4);
    MEMDETAIL_VALUE("UserELFPages:       %8lu\n", active_elf_pages);
    MEMDETAIL_VALUE("UserELFKB:          %8lu\n", active_elf_pages * 4);
    MEMDETAIL_VALUE("UserHeapPages:      %8lu\n", task_stats.active_heap_pages);
    MEMDETAIL_VALUE("UserHeapKB:         %8lu\n", task_stats.active_heap_pages * 4);
    MEMDETAIL_VALUE("UserMmapPages:      %8lu\n", task_stats.active_mmap_pages);
    MEMDETAIL_VALUE("UserMmapKB:         %8lu\n", task_stats.active_mmap_pages * 4);
    MEMDETAIL_VALUE("UserStackPages:     %8lu\n", user_stack_pages);
    MEMDETAIL_VALUE("UserStackKB:        %8lu\n", user_stack_pages * 4);
    MEMDETAIL_VALUE("UserPDPages:        %8lu\n", user_pd_pages);
    MEMDETAIL_VALUE("UserPDKB:           %8lu\n", user_pd_pages * 4);
    MEMDETAIL_VALUE("UserPTPages:        %8lu\n", task_stats.active_user_pt_pages);
    MEMDETAIL_VALUE("UserPTKB:           %8lu\n", task_stats.active_user_pt_pages * 4);
    MEMDETAIL_VALUE("ActiveUserPages:    %8lu\n", task_stats.active_user_pages);
    MEMDETAIL_VALUE("ActiveUserKB:       %8lu\n", task_stats.active_user_pages * 4);
    MEMDETAIL_VALUE("CurrentUserPages:   %8lu\n", task_stats.current_user_pages);
    MEMDETAIL_VALUE("CurrentUserKB:      %8lu\n", task_stats.current_user_pages * 4);
    MEMDETAIL_VALUE("CurrentUserPTPages: %8lu\n", task_stats.current_user_pt_pages);
    MEMDETAIL_VALUE("CurrentUserPTKB:    %8lu\n", task_stats.current_user_pt_pages * 4);
    MEMDETAIL_VALUE("CurrentELFPages:    %8lu\n", current_elf_pages);
    MEMDETAIL_VALUE("CurrentHeapPages:   %8lu\n", task_stats.current_heap_pages);
    MEMDETAIL_VALUE("CurrentHeapKB:      %8lu\n", task_stats.current_heap_pages * 4);
    MEMDETAIL_VALUE("CurrentMmapPages:   %8lu\n", task_stats.current_mmap_pages);
    MEMDETAIL_VALUE("CurrentMmapKB:      %8lu\n", task_stats.current_mmap_pages * 4);
    MEMDETAIL_VALUE("CurrentStackPages:  %8lu\n", task_stats.current_stack_pages);
    MEMDETAIL_VALUE("CurrentStackKB:     %8lu\n", task_stats.current_stack_pages * 4);
    MEMDETAIL_VALUE("DeadUserPages:      %8lu\n", task_stats.dead_user_pages);
    MEMDETAIL_VALUE("DeadUserKB:         %8lu\n", task_stats.dead_user_pages * 4);
    MEMDETAIL_VALUE("DeadELFPages:       %8lu\n", dead_elf_pages);
    MEMDETAIL_VALUE("DeadELFKB:          %8lu\n", dead_elf_pages * 4);
    MEMDETAIL_VALUE("DeadHeapPages:      %8lu\n", task_stats.dead_heap_pages);
    MEMDETAIL_VALUE("DeadHeapKB:         %8lu\n", task_stats.dead_heap_pages * 4);
    MEMDETAIL_VALUE("DeadMmapPages:      %8lu\n", task_stats.dead_mmap_pages);
    MEMDETAIL_VALUE("DeadMmapKB:         %8lu\n", task_stats.dead_mmap_pages * 4);
    MEMDETAIL_VALUE("DeadStackPages:     %8lu\n", task_stats.dead_stack_pages);
    MEMDETAIL_VALUE("DeadStackKB:        %8lu\n", task_stats.dead_stack_pages * 4);
    MEMDETAIL_VALUE("DeadPDPages:        %8lu\n", task_stats.dead_pd_pages);
    MEMDETAIL_VALUE("DeadPDKB:           %8lu\n", task_stats.dead_pd_pages * 4);
    MEMDETAIL_VALUE("DeadUserPTPages:    %8lu\n", task_stats.dead_user_pt_pages);
    MEMDETAIL_VALUE("DeadUserPTKB:       %8lu\n", task_stats.dead_user_pt_pages * 4);
    MEMDETAIL_VALUE("DeadExecOldPages:   %8lu\n", task_stats.dead_exec_old_pages);
    MEMDETAIL_VALUE("DeadExecOldKB:      %8lu\n", task_stats.dead_exec_old_pages * 4);
    MEMDETAIL_VALUE("ExecCleanupEntries: %8lu\n", task_stats.exec_cleanup_entries);
    MEMDETAIL_VALUE("ExecCleanupPages:   %8lu\n", task_stats.exec_cleanup_user_pages);
    MEMDETAIL_VALUE("ExecCleanupKB:      %8lu\n", task_stats.exec_cleanup_user_pages * 4);
    MEMDETAIL_VALUE("ExecCachePages:     %8lu\n", exec_cache_pages);
    MEMDETAIL_VALUE("ExecCacheKB:        %8lu\n", exec_cache_pages * 4);
    MEMDETAIL_VALUE("ExecReclaimPages:   %8lu\n", exec_reclaim_pages);
    MEMDETAIL_VALUE("ExecReclaimKB:      %8lu\n", exec_reclaim_pages * 4);
    MEMDETAIL_VALUE("ExecNonReclaimPages:%8lu\n", task_stats.exec_nonreclaim_pages);
    MEMDETAIL_VALUE("ExecNonReclaimKB:   %8lu\n", task_stats.exec_nonreclaim_pages * 4);
    MEMDETAIL_VALUE("CurrentExecReclaimed:%7lu\n", current_exec_reclaimed);
    MEMDETAIL_VALUE("PartialExecReclaimed:%7lu\n", partial_exec_reclaimed);
    MEMDETAIL_VALUE("OverlayCacheNodes:  %8lu\n", overlay_nodes);
    MEMDETAIL_VALUE("OverlayCacheCap:    %8lu\n", overlay_capacity);
    MEMDETAIL_VALUE("OverlayCacheBytes:  %8lu\n", overlay_bytes);
    MEMDETAIL_VALUE("SquashCacheNodes:   %8lu\n", sqfs_nodes);
    MEMDETAIL_VALUE("SquashCacheCap:     %8lu\n", sqfs_capacity);
    MEMDETAIL_VALUE("SquashCacheBytes:   %8lu\n", sqfs_bytes);
    MEMDETAIL_VALUE("SquashCacheData:    %8lu\n", sqfs_data_bytes);
    MEMDETAIL_VALUE("SquashModulePages:  %8lu\n", sqfs_module_pages);
    MEMDETAIL_VALUE("SquashModuleKB:     %8lu\n", sqfs_module_pages * 4);
    MEMDETAIL_VALUE("SquashDecompFail:   %8lu\n", sqfs_decomp_failures);
    MEMDETAIL_VALUE("SquashDecompOver:   %8lu\n", sqfs_decomp_oversize);
    MEMDETAIL_VALUE("SquashDecompPadded: %8lu\n", sqfs_decomp_padded);
    MEMDETAIL_VALUE("ConsoleBuffers:     %8lu\n", console_buffers);
    MEMDETAIL_VALUE("ConsoleBytes:       %8lu\n", console_bytes);
    MEMDETAIL_VALUE("PFARefActiveNodes:  %8lu\n", ref_active_nodes);
    MEMDETAIL_VALUE("PFARefFreeNodes:    %8lu\n", ref_free_nodes);

#undef MEMDETAIL_VALUE

    proc_stream_append(&stream, "HeapProfile:\n", 13);
    profile_start = stream.position;
    request_end = offset + size;
    if (request_end < offset) request_end = UINT64_MAX;
    if (stream.copied < size && request_end > profile_start && buffer) {
        profile_offset = offset > profile_start ? offset - profile_start : 0;
        profile_size = size - stream.copied;
        stream.copied += heap_profile_read(profile_offset, profile_size,
                                           buffer + stream.copied);
    }
    return stream.copied;

}

static dirent_t *procfs_readdir(vfs_node_t *node, uint64_t index) {
    static const char *entries[] = {
        "self", "version", "uptime", "meminfo", "cpuinfo", "loadavg",
        "stat", "mounts", "filesystems", "cmdline", "devices", "interrupts", "vmstat",
        "memdetail", "kmsg"
    };
    task_t *t;
    uint64_t count;
    uint64_t pid_index;

    (void)node;

    if (index < sizeof(entries)/sizeof(entries[0])) {
        strcpy(proc_dirent.name, entries[index]);
        proc_dirent.inode = index + 1;
        proc_dirent.type = (index == 0) ? VFS_DIRECTORY : VFS_FILE;
        return &proc_dirent;
    }
    
    pid_index = index - (uint64_t)(sizeof(entries)/sizeof(entries[0]));
    count = 0;
    lock_scheduler();
    t = all_tasks_head;
    while (t) {
        if ((t->id != 0 || t->is_user) &&
            !(t->state == TASK_DEAD && t->waited)) {
            if (count == pid_index) {
                snprintf(proc_dirent.name, sizeof(proc_dirent.name), "%d", t->pid);
                proc_dirent.inode = (uint64_t)t->pid;
                proc_dirent.type = VFS_DIRECTORY;
                unlock_scheduler();
                return &proc_dirent;
            }
            count++;
        }
        t = t->all_next;
    }
    unlock_scheduler();
    
    return NULL;
}

static int procfs_pid_visible(pid_t pid) {
    task_t *task;
    int visible;

    visible = 0;
    lock_scheduler();
    task = all_tasks_head;
    while (task) {
        if (task->pid == pid) {
            visible = !(task->state == TASK_DEAD && task->waited);
            break;
        }
        task = task->all_next;
    }
    unlock_scheduler();
    return visible;
}

static dirent_t *proc_pid_readdir(vfs_node_t *node, uint64_t index) {
    static dirent_t pid_dirent;
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
    
    if (index == sizeof(entries)/sizeof(entries[0])) {
        strcpy(pid_dirent.name, "task");
        pid_dirent.inode = node->inode * 100 + 50;
        pid_dirent.type = VFS_DIRECTORY;
        return &pid_dirent;
    }
    
    return NULL;
}

static vfs_node_t *proc_pid_finddir(vfs_node_t *node, const char *name) {
    uint64_t pid_val;
    int file_idx;
    vfs_node_t *tdir;
    vfs_node_t *fnode;
    
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
    
    for (file_idx = 0; file_idx < (int)(sizeof(files)/sizeof(files[0])); file_idx++) {
        if (strcmp(name, files[file_idx].name) == 0) {
            fnode = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
            if (!fnode) return NULL;
            memset(fnode, 0, sizeof(vfs_node_t));
            strcpy(fnode->name, name);
            fnode->flags = VFS_FILE | VFS_DYNAMIC;
            fnode->inode = pid_val * 100 + (uint64_t)file_idx;
            fnode->read = files[file_idx].read;
            fnode->parent = &procfs_root;
            fnode->mask = 0444;
            return fnode;
        }
    }
    
    if (strcmp(name, "task") == 0) {
        tdir = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
        if (!tdir) return NULL;
        memset(tdir, 0, sizeof(vfs_node_t));
        strcpy(tdir->name, "task");
        tdir->flags = VFS_DIRECTORY | VFS_DYNAMIC;
        tdir->inode = pid_val;
        tdir->readdir = proc_task_readdir;
        tdir->finddir = proc_task_finddir;
        tdir->parent = &procfs_root;
        tdir->mask = 0555;
        return tdir;
    }
    
    return NULL;
}

static dirent_t *proc_task_readdir(vfs_node_t *node, uint64_t index) {
    static dirent_t task_dirent;
    if (index == 0) {
        snprintf(task_dirent.name, sizeof(task_dirent.name), "%d", (int)node->inode);
        task_dirent.inode = node->inode;
        task_dirent.type = VFS_DIRECTORY;
        return &task_dirent;
    }
    return NULL;
}

static vfs_node_t *proc_task_finddir(vfs_node_t *node, const char *name) {
    int pid;
    const char *p;
    vfs_node_t *tdir;

    pid = 0;
    p = name;
    while (*p >= '0' && *p <= '9') {
        pid = pid * 10 + (*p - '0');
        p++;
    }
    if (*p != '\0' || pid <= 0)
        return NULL;
    if ((uint64_t)pid != node->inode)
        return NULL;

    tdir = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!tdir) return NULL;
    memset(tdir, 0, sizeof(vfs_node_t));
    snprintf(tdir->name, sizeof(tdir->name), "%d", pid);
    tdir->flags = VFS_DIRECTORY | VFS_DYNAMIC;
    tdir->inode = (uint64_t)pid;
    tdir->readdir = proc_task_thread_readdir;
    tdir->finddir = proc_task_thread_finddir;
    tdir->parent = &procfs_root;
    tdir->mask = 0555;
    return tdir;
}

static dirent_t *proc_task_thread_readdir(vfs_node_t *node, uint64_t index) {
    static dirent_t task_thread_dirent;
    static const char *entries[] = {
        "maps", "status", "cmdline", "environ",
        "stat", "statm", "comm", "limits", "io"
    };

    (void)node;

    if (index < sizeof(entries)/sizeof(entries[0])) {
        strcpy(task_thread_dirent.name, entries[index]);
        task_thread_dirent.inode = node->inode * 100 + index;
        task_thread_dirent.type = VFS_FILE;
        return &task_thread_dirent;
    }

    return NULL;
}

static vfs_node_t *proc_task_thread_finddir(vfs_node_t *node, const char *name) {
    uint64_t pid_val;
    int file_idx;
    vfs_node_t *fnode;

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

    for (file_idx = 0; file_idx < (int)(sizeof(files)/sizeof(files[0])); file_idx++) {
        if (strcmp(name, files[file_idx].name) == 0) {
            fnode = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
            if (!fnode) return NULL;
            memset(fnode, 0, sizeof(vfs_node_t));
            strcpy(fnode->name, name);
            fnode->flags = VFS_FILE | VFS_DYNAMIC;
            fnode->inode = pid_val * 100 + (uint64_t)file_idx;
            fnode->read = files[file_idx].read;
            fnode->parent = &procfs_root;
            fnode->mask = 0444;
            return fnode;
        }
    }

    return NULL;
}

static vfs_node_t *procfs_setup_pid_dir(pid_t pid) {
    vfs_node_t *dir;
    uint64_t raw_inode;

    raw_inode = (uint64_t)(int64_t)pid;
    dir = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!dir) return NULL;
    memset(dir, 0, sizeof(vfs_node_t));
    snprintf(dir->name, sizeof(dir->name), "%d", pid);
    dir->flags = VFS_DIRECTORY | VFS_DYNAMIC;
    dir->inode = raw_inode;
    dir->readdir = proc_pid_readdir;
    dir->finddir = proc_pid_finddir;
    dir->parent = &procfs_root;
    dir->mask = 0555;
    return dir;
}

static vfs_node_t *procfs_finddir(vfs_node_t *node, const char *name) {
    (void)node;
    
    if (strcmp(name, "self") == 0) return procfs_get_self();
    if (strcmp(name, "version") == 0) return procfs_get_version();
    if (strcmp(name, "uptime") == 0) return procfs_get_uptime();
    if (strcmp(name, "meminfo") == 0) return procfs_get_meminfo();
    if (strcmp(name, "cpuinfo") == 0) return procfs_get_cpuinfo();
    if (strcmp(name, "loadavg") == 0) return procfs_get_loadavg();
    if (strcmp(name, "stat") == 0) return procfs_get_stat();
    if (strcmp(name, "mounts") == 0) return procfs_get_mounts();
    if (strcmp(name, "filesystems") == 0) return procfs_get_filesystems();
    if (strcmp(name, "cmdline") == 0) return procfs_get_cmdline();
    if (strcmp(name, "devices") == 0) return procfs_get_devices();
    if (strcmp(name, "interrupts") == 0) return procfs_get_interrupts();
    if (strcmp(name, "vmstat") == 0) return procfs_get_vmstat();
    if (strcmp(name, "memdetail") == 0) return procfs_get_memdetail();
    if (strcmp(name, "kmsg") == 0) return procfs_get_kmsg();
    
    {
        int pid = 0;
        int neg = 0;
        const char *p = name;

        if (*p == '-') {
            neg = 1;
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            pid = pid * 10 + (*p - '0');
            p++;
        }
        if (neg) pid = -pid;
        if (*p == '\0' && pid != 0) {
            if (procfs_pid_visible((pid_t)pid)) {
                return procfs_setup_pid_dir((pid_t)pid);
            }
        }
    }
    
    return NULL;
}

static dirent_t *proc_self_readdir(vfs_node_t *node, uint64_t index) {
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
    
    if (strcmp(name, "exe") == 0) return procfs_get_self_exe();
    if (strcmp(name, "cwd") == 0) return procfs_get_self_cwd();
    if (strcmp(name, "fd") == 0) return procfs_get_self_fd();
    if (strcmp(name, "maps") == 0) return procfs_get_self_maps();
    if (strcmp(name, "status") == 0) return procfs_get_self_status();
    if (strcmp(name, "cmdline") == 0) return procfs_get_self_cmdline();
    if (strcmp(name, "environ") == 0) return procfs_get_self_environ();
    if (strcmp(name, "stat") == 0) return procfs_get_self_stat();
    if (strcmp(name, "statm") == 0) return procfs_get_self_statm();
    if (strcmp(name, "comm") == 0) return procfs_get_self_comm();
    if (strcmp(name, "limits") == 0) return procfs_get_self_limits();
    if (strcmp(name, "io") == 0) return procfs_get_self_io();
    
    return NULL;
}

static vfs_node_t *procfs_get_self(void) {
    return procfs_lazy_node(&proc_self, "self", VFS_DIRECTORY, &procfs_root, NULL, proc_self_readdir, proc_self_finddir);
}

static vfs_node_t *procfs_get_self_exe(void) {
    return procfs_lazy_node(&proc_self_exe, "exe", VFS_SYMLINK, procfs_get_self(), NULL, NULL, NULL);
}

static vfs_node_t *procfs_get_self_cwd(void) {
    return procfs_lazy_node(&proc_self_cwd, "cwd", VFS_SYMLINK, procfs_get_self(), NULL, NULL, NULL);
}

static vfs_node_t *procfs_get_self_fd(void) {
    return procfs_lazy_node(&proc_self_fd, "fd", VFS_DIRECTORY, procfs_get_self(), NULL, NULL, NULL);
}

static vfs_node_t *procfs_get_self_maps(void) {
    return procfs_lazy_node(&proc_self_maps, "maps", VFS_FILE, procfs_get_self(), proc_self_maps_read, NULL, NULL);
}

static vfs_node_t *procfs_get_self_status(void) {
    return procfs_lazy_node(&proc_self_status, "status", VFS_FILE, procfs_get_self(), proc_self_status_read, NULL, NULL);
}

static vfs_node_t *procfs_get_self_cmdline(void) {
    return procfs_lazy_node(&proc_self_cmdline, "cmdline", VFS_FILE, procfs_get_self(), proc_self_cmdline_read, NULL, NULL);
}

static vfs_node_t *procfs_get_self_environ(void) {
    return procfs_lazy_node(&proc_self_environ, "environ", VFS_FILE, procfs_get_self(), proc_self_environ_read, NULL, NULL);
}

static vfs_node_t *procfs_get_self_stat(void) {
    return procfs_lazy_node(&proc_self_stat, "stat", VFS_FILE, procfs_get_self(), proc_self_stat_read, NULL, NULL);
}

static vfs_node_t *procfs_get_self_statm(void) {
    return procfs_lazy_node(&proc_self_statm, "statm", VFS_FILE, procfs_get_self(), proc_self_statm_read, NULL, NULL);
}

static vfs_node_t *procfs_get_self_comm(void) {
    return procfs_lazy_node(&proc_self_comm, "comm", VFS_FILE, procfs_get_self(), proc_self_comm_read, NULL, NULL);
}

static vfs_node_t *procfs_get_self_limits(void) {
    return procfs_lazy_node(&proc_self_limits, "limits", VFS_FILE, procfs_get_self(), proc_self_limits_read, NULL, NULL);
}

static vfs_node_t *procfs_get_self_io(void) {
    return procfs_lazy_node(&proc_self_io, "io", VFS_FILE, procfs_get_self(), proc_self_io_read, NULL, NULL);
}

static vfs_node_t *procfs_get_version(void) {
    return procfs_lazy_node(&proc_version, "version", VFS_FILE, &procfs_root, proc_version_read, NULL, NULL);
}

static vfs_node_t *procfs_get_uptime(void) {
    return procfs_lazy_node(&proc_uptime, "uptime", VFS_FILE, &procfs_root, proc_uptime_read, NULL, NULL);
}

static vfs_node_t *procfs_get_meminfo(void) {
    return procfs_lazy_node(&proc_meminfo, "meminfo", VFS_FILE, &procfs_root, proc_meminfo_read, NULL, NULL);
}

static vfs_node_t *procfs_get_cpuinfo(void) {
    return procfs_lazy_node(&proc_cpuinfo, "cpuinfo", VFS_FILE, &procfs_root, proc_cpuinfo_read, NULL, NULL);
}

static vfs_node_t *procfs_get_loadavg(void) {
    return procfs_lazy_node(&proc_loadavg, "loadavg", VFS_FILE, &procfs_root, proc_loadavg_read, NULL, NULL);
}

static vfs_node_t *procfs_get_stat(void) {
    return procfs_lazy_node(&proc_stat, "stat", VFS_FILE, &procfs_root, proc_stat_read, NULL, NULL);
}

static vfs_node_t *procfs_get_mounts(void) {
    return procfs_lazy_node(&proc_mounts, "mounts", VFS_FILE, &procfs_root, proc_mounts_read, NULL, NULL);
}

static vfs_node_t *procfs_get_filesystems(void) {
    return procfs_lazy_node(&proc_filesystems, "filesystems", VFS_FILE, &procfs_root, proc_filesystems_read, NULL, NULL);
}

static vfs_node_t *procfs_get_cmdline(void) {
    return procfs_lazy_node(&proc_cmdline, "cmdline", VFS_FILE, &procfs_root, proc_cmdline_read, NULL, NULL);
}

static vfs_node_t *procfs_get_devices(void) {
    return procfs_lazy_node(&proc_devices, "devices", VFS_FILE, &procfs_root, proc_devices_read, NULL, NULL);
}

static vfs_node_t *procfs_get_interrupts(void) {
    return procfs_lazy_node(&proc_interrupts, "interrupts", VFS_FILE, &procfs_root, proc_interrupts_read, NULL, NULL);
}

static vfs_node_t *procfs_get_vmstat(void) {
    return procfs_lazy_node(&proc_vmstat, "vmstat", VFS_FILE, &procfs_root, proc_vmstat_read, NULL, NULL);
}

static vfs_node_t *procfs_get_memdetail(void) {
    return procfs_lazy_node(&proc_memdetail, "memdetail", VFS_FILE, &procfs_root, proc_memdetail_read, NULL, NULL);
}

static vfs_node_t *procfs_get_kmsg(void) {
    vfs_node_t *node;

    node = procfs_lazy_node(&proc_kmsg, "kmsg", VFS_FILE, &procfs_root, proc_kmsg_read, NULL, NULL);
    if (node) node->mask = 0400;
    return node;
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

void KERNEL_INIT procfs_init(void) {
    procfs_type.name = "procfs";
    procfs_type.mount = procfs_mount;
    procfs_type.unmount = procfs_unmount;
    procfs_type.next = NULL;

    memset(&procfs_root, 0, sizeof(vfs_node_t));
    strcpy(procfs_root.name, "proc");
    procfs_root.flags = VFS_DIRECTORY;
    procfs_root.mask = 0555;
    procfs_root.readdir = procfs_readdir;
    procfs_root.finddir = procfs_finddir;
    procfs_root.ref_count = 1;

    vfs_register_fs(&procfs_type);
}
