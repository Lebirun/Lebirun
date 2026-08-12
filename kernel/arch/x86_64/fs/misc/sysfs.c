#include <lebirun/vfs.h>
#include <lebirun/common.h>
#include <lebirun/pit.h>
#include <lebirun/mem_map.h>
#include <lebirun/smp.h>
#include <string.h>
#include <stdio.h>

extern uint64_t pit_freq;

static dirent_t *sysfs_dirent_pool;
static volatile uint64_t sysfs_dirent_index;
static uint64_t sysfs_dirent_capacity;
static vfs_node_t **sysfs_cpu_dynamic_nodes;
static vfs_node_t **sysfs_cpu_dynamic_online;
static int sysfs_cpu_dynamic_capacity;

static dirent_t *sysfs_alloc_dirent(void) {
    uint64_t idx;

    if (!sysfs_dirent_pool || sysfs_dirent_capacity == 0) {
        sysfs_dirent_capacity = 6;
        sysfs_dirent_pool = (dirent_t *)kmalloc(sizeof(dirent_t) * sysfs_dirent_capacity);
        if (!sysfs_dirent_pool) {
            sysfs_dirent_capacity = 0;
            return NULL;
        }
        memset(sysfs_dirent_pool, 0, sizeof(dirent_t) * sysfs_dirent_capacity);
        sysfs_dirent_index = 0;
    }
    idx = sysfs_dirent_index;
    sysfs_dirent_index = (idx + 1) % sysfs_dirent_capacity;
    return &sysfs_dirent_pool[idx];
}

static uint64_t sysfs_read_static(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer, const char *content, uint64_t content_len) {
    uint64_t remaining;

    (void)node;

    if (offset >= content_len) return 0;
    remaining = content_len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, content + offset, size);
    return size;
}

static vfs_node_t sysfs_root;
static vfs_node_t *sysfs_devices;
static vfs_node_t *sysfs_devices_system;
static vfs_node_t *sysfs_devices_system_cpu;
static vfs_node_t *sysfs_devices_system_cpu_cpu0;
static vfs_node_t *sysfs_cpu0_online;
static vfs_node_t *sysfs_cpu0_cpufreq;
static vfs_node_t *sysfs_cpu0_cpufreq_scaling_cur_freq;
static vfs_node_t *sysfs_cpu0_cpufreq_scaling_min_freq;
static vfs_node_t *sysfs_cpu0_cpufreq_scaling_max_freq;
static vfs_node_t *sysfs_cpu0_cpufreq_cpuinfo_min_freq;
static vfs_node_t *sysfs_cpu0_cpufreq_cpuinfo_max_freq;
static vfs_node_t *sysfs_cpu0_cpufreq_scaling_governor;
static vfs_node_t *sysfs_cpu0_topology;
static vfs_node_t *sysfs_cpu0_topology_core_id;
static vfs_node_t *sysfs_cpu0_topology_physical_package_id;
static vfs_node_t *sysfs_cpu0_topology_core_siblings;
static vfs_node_t *sysfs_cpu0_topology_thread_siblings;
static vfs_node_t *sysfs_cpu_online;
static vfs_node_t *sysfs_cpu_possible;
static vfs_node_t *sysfs_cpu_present;
static vfs_node_t *sysfs_cpu_kernel_max;
static vfs_node_t *sysfs_class;
static vfs_node_t *sysfs_class_power_supply;
static vfs_node_t *sysfs_block;
static vfs_node_t *sysfs_kernel;
static vfs_node_t *sysfs_kernel_mm;
static vfs_node_t *sysfs_kernel_mm_hugepages;
static vfs_node_t *sysfs_fs;
static vfs_node_t *sysfs_bus;

static vfs_node_t **sysfs_reclaim_slots[] = {
    &sysfs_cpu0_cpufreq_scaling_cur_freq,
    &sysfs_cpu0_cpufreq_scaling_min_freq,
    &sysfs_cpu0_cpufreq_scaling_max_freq,
    &sysfs_cpu0_cpufreq_cpuinfo_min_freq,
    &sysfs_cpu0_cpufreq_cpuinfo_max_freq,
    &sysfs_cpu0_cpufreq_scaling_governor,
    &sysfs_cpu0_topology_core_id,
    &sysfs_cpu0_topology_physical_package_id,
    &sysfs_cpu0_topology_core_siblings,
    &sysfs_cpu0_topology_thread_siblings,
    &sysfs_cpu0_online,
    &sysfs_cpu_online,
    &sysfs_cpu_possible,
    &sysfs_cpu_present,
    &sysfs_cpu_kernel_max,
    &sysfs_cpu0_cpufreq,
    &sysfs_cpu0_topology,
    &sysfs_devices_system_cpu_cpu0,
    &sysfs_devices_system_cpu,
    &sysfs_devices_system,
    &sysfs_devices,
    &sysfs_class_power_supply,
    &sysfs_class,
    &sysfs_block,
    &sysfs_kernel_mm_hugepages,
    &sysfs_kernel_mm,
    &sysfs_kernel,
    &sysfs_fs,
    &sysfs_bus
};

static uint64_t sysfs_cpu0_online_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char value[3];
    int cpu;

    cpu = node ? (int)node->inode : 0;
    value[0] = cpus && cpu >= 0 && cpu < cpu_count && cpus[cpu].active ?
               '1' : '0';
    value[1] = '\n';
    value[2] = '\0';
    return sysfs_read_static(node, offset, size, buffer, value, 2);
}

static uint64_t sysfs_cpu_online_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char value[128];
    int start;
    int end;
    int cpu;
    int len;
    int written;

    (void)node;
    len = 0;
    cpu = 0;
    while (cpu < cpu_count && len < (int)sizeof(value) - 2) {
        while (cpu < cpu_count && (!cpus || !cpus[cpu].active)) cpu++;
        if (cpu >= cpu_count) break;
        start = cpu;
        end = cpu;
        while (end + 1 < cpu_count && cpus && cpus[end + 1].active) end++;
        written = snprintf(value + len, sizeof(value) - (size_t)len,
                           len ? ",%d" : "%d", start);
        if (written < 0 || written >= (int)sizeof(value) - len) break;
        len += written;
        if (end != start) {
            written = snprintf(value + len, sizeof(value) - (size_t)len,
                               "-%d", end);
            if (written < 0 || written >= (int)sizeof(value) - len) break;
            len += written;
        }
        cpu = end + 1;
    }
    if (len < (int)sizeof(value) - 1) value[len++] = '\n';
    return sysfs_read_static(node, offset, size, buffer, value, (uint64_t)len);
}

static uint64_t sysfs_cpu_possible_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char value[32];
    int last;
    int len;

    last = cpu_count > 0 ? cpu_count - 1 : 0;
    len = snprintf(value, sizeof(value), "0-%d\n", last);
    return sysfs_read_static(node, offset, size, buffer, value,
                             (uint64_t)len);
}

static uint64_t sysfs_cpu_present_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    return sysfs_cpu_possible_read(node, offset, size, buffer);
}

static uint64_t sysfs_cpu_kernel_max_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char value[32];
    int last;
    int len;

    last = UINT8_MAX;
    len = snprintf(value, sizeof(value), "%d\n", last);
    return sysfs_read_static(node, offset, size, buffer, value, (uint64_t)len);
}

static uint64_t sysfs_cpufreq_cur_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[32];
    int len;
    uint64_t freq_khz;
    uint64_t remaining;

    (void)node;

    freq_khz = pit_freq * 1000;
    if (freq_khz == 0) freq_khz = 1000000;
    len = snprintf(buf, sizeof(buf), "%lu\n", freq_khz);
    if (len < 0) len = 0;
    if (offset >= (uint64_t)len) return 0;
    remaining = (uint64_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint64_t sysfs_cpufreq_min_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[32];
    int len;
    uint64_t freq_khz;
    uint64_t remaining;

    (void)node;

    freq_khz = pit_freq * 500;
    if (freq_khz == 0) freq_khz = 500000;
    len = snprintf(buf, sizeof(buf), "%lu\n", freq_khz);
    if (len < 0) len = 0;
    if (offset >= (uint64_t)len) return 0;
    remaining = (uint64_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint64_t sysfs_cpufreq_max_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    char buf[32];
    int len;
    uint64_t freq_khz;
    uint64_t remaining;

    (void)node;

    freq_khz = pit_freq * 1000;
    if (freq_khz == 0) freq_khz = 1000000;
    len = snprintf(buf, sizeof(buf), "%lu\n", freq_khz);
    if (len < 0) len = 0;
    if (offset >= (uint64_t)len) return 0;
    remaining = (uint64_t)len - offset;
    if (size > remaining) size = remaining;
    memcpy(buffer, buf + offset, size);
    return size;
}

static uint64_t sysfs_cpufreq_governor_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    return sysfs_read_static(node, offset, size, buffer, "performance\n", 12);
}

static uint64_t sysfs_topology_core_id_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    return sysfs_read_static(node, offset, size, buffer, "0\n", 2);
}

static uint64_t sysfs_topology_phys_pkg_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    return sysfs_read_static(node, offset, size, buffer, "0\n", 2);
}

static uint64_t sysfs_topology_core_siblings_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    return sysfs_read_static(node, offset, size, buffer, "1\n", 2);
}

static uint64_t sysfs_topology_thread_siblings_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    return sysfs_read_static(node, offset, size, buffer, "1\n", 2);
}

static void sysfs_init_node(vfs_node_t *n, const char *name, uint64_t flags, vfs_node_t *parent) {
    memset(n, 0, sizeof(vfs_node_t));
    n->flags = flags;
    vfs_node_set_name(n, name);
    n->parent = parent;
    n->ref_count = 1;
}

static vfs_node_t *sysfs_lazy_node(vfs_node_t **slot, const char *name, uint64_t flags, vfs_node_t *parent, read_type_t read, readdir_type_t readdir, finddir_type_t finddir) {
    vfs_node_t *n;

    if (*slot) return *slot;
    if (!parent) return NULL;
    n = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!n) return NULL;
    sysfs_init_node(n, name, flags, parent);
    n->read = read;
    n->readdir = readdir;
    n->finddir = finddir;
    *slot = n;
    return n;
}

static int sysfs_node_in_chain(vfs_node_t *node, vfs_node_t *active) {
    vfs_node_t *cur;

    cur = active;
    while (cur) {
        if (cur == node) return 1;
        cur = cur->parent;
    }
    return 0;
}

static int sysfs_node_has_busy_descendant(vfs_node_t *node) {
    vfs_node_t *cur;
    vfs_node_t *candidate;
    uint64_t i;
    uint64_t count;

    count = sizeof(sysfs_reclaim_slots) / sizeof(sysfs_reclaim_slots[0]);
    for (i = 0; i < count; i++) {
        candidate = *sysfs_reclaim_slots[i];
        if (!candidate || candidate->ref_count <= 1) continue;
        cur = candidate->parent;
        while (cur) {
            if (cur == node) return 1;
            cur = cur->parent;
        }
    }
    return 0;
}

static void sysfs_reclaim_lazy_node(vfs_node_t **slot, vfs_node_t *active) {
    vfs_node_t *node;

    node = *slot;
    if (!node) return;
    if (sysfs_node_in_chain(node, active)) return;
    if (node->ref_count > 1) return;
    if (sysfs_node_has_busy_descendant(node)) return;
    *slot = NULL;
    vfs_node_release_name(node);
    kfree(node);
}

void sysfs_reclaim_unused(void) {
    uint64_t i;
    uint64_t count;
    int busy;

    count = sizeof(sysfs_reclaim_slots) / sizeof(sysfs_reclaim_slots[0]);
    for (i = 0; i < count; i++) {
        sysfs_reclaim_lazy_node(sysfs_reclaim_slots[i], NULL);
    }
    busy = 0;
    for (i = 1; i < (uint64_t)sysfs_cpu_dynamic_capacity; i++) {
        if (sysfs_cpu_dynamic_online && sysfs_cpu_dynamic_online[i] &&
            sysfs_cpu_dynamic_online[i]->ref_count <= 1) {
            kfree(sysfs_cpu_dynamic_online[i]);
            sysfs_cpu_dynamic_online[i] = NULL;
        }
        if (sysfs_cpu_dynamic_nodes && sysfs_cpu_dynamic_nodes[i] &&
            (!sysfs_cpu_dynamic_online || !sysfs_cpu_dynamic_online[i]) &&
            sysfs_cpu_dynamic_nodes[i]->ref_count <= 1) {
            kfree(sysfs_cpu_dynamic_nodes[i]);
            sysfs_cpu_dynamic_nodes[i] = NULL;
        }
        if ((sysfs_cpu_dynamic_nodes && sysfs_cpu_dynamic_nodes[i]) ||
            (sysfs_cpu_dynamic_online && sysfs_cpu_dynamic_online[i]))
            busy = 1;
    }
    if (!busy && sysfs_cpu_dynamic_capacity > 0) {
        kfree(sysfs_cpu_dynamic_nodes);
        kfree(sysfs_cpu_dynamic_online);
        sysfs_cpu_dynamic_nodes = NULL;
        sysfs_cpu_dynamic_online = NULL;
        sysfs_cpu_dynamic_capacity = 0;
    }
}

static dirent_t *sysfs_cpufreq_readdir(vfs_node_t *node, uint64_t index);
static vfs_node_t *sysfs_cpufreq_finddir(vfs_node_t *node, const char *name);
static dirent_t *sysfs_topology_readdir(vfs_node_t *node, uint64_t index);
static vfs_node_t *sysfs_topology_finddir(vfs_node_t *node, const char *name);
static dirent_t *sysfs_cpu0_readdir(vfs_node_t *node, uint64_t index);
static vfs_node_t *sysfs_cpu0_finddir(vfs_node_t *node, const char *name);
static dirent_t *sysfs_cpu_readdir(vfs_node_t *node, uint64_t index);
static vfs_node_t *sysfs_cpu_finddir(vfs_node_t *node, const char *name);
static dirent_t *sysfs_system_readdir(vfs_node_t *node, uint64_t index);
static vfs_node_t *sysfs_system_finddir(vfs_node_t *node, const char *name);
static dirent_t *sysfs_devices_readdir(vfs_node_t *node, uint64_t index);
static vfs_node_t *sysfs_devices_finddir(vfs_node_t *node, const char *name);
static dirent_t *sysfs_empty_readdir(vfs_node_t *node, uint64_t index);
static vfs_node_t *sysfs_empty_finddir(vfs_node_t *node, const char *name);
static dirent_t *sysfs_class_readdir(vfs_node_t *node, uint64_t index);
static vfs_node_t *sysfs_class_finddir(vfs_node_t *node, const char *name);
static dirent_t *sysfs_kernel_readdir(vfs_node_t *node, uint64_t index);
static vfs_node_t *sysfs_kernel_finddir(vfs_node_t *node, const char *name);
static dirent_t *sysfs_kernel_mm_readdir(vfs_node_t *node, uint64_t index);
static vfs_node_t *sysfs_kernel_mm_finddir(vfs_node_t *node, const char *name);

static vfs_node_t *sysfs_get_devices(void);
static vfs_node_t *sysfs_get_devices_system(void);
static vfs_node_t *sysfs_get_devices_system_cpu(void);
static vfs_node_t *sysfs_get_devices_system_cpu_cpu0(void);
static vfs_node_t *sysfs_get_cpu0_online(void);
static vfs_node_t *sysfs_get_cpu0_cpufreq(void);
static vfs_node_t *sysfs_get_cpu0_cpufreq_scaling_cur_freq(void);
static vfs_node_t *sysfs_get_cpu0_cpufreq_scaling_min_freq(void);
static vfs_node_t *sysfs_get_cpu0_cpufreq_scaling_max_freq(void);
static vfs_node_t *sysfs_get_cpu0_cpufreq_cpuinfo_min_freq(void);
static vfs_node_t *sysfs_get_cpu0_cpufreq_cpuinfo_max_freq(void);
static vfs_node_t *sysfs_get_cpu0_cpufreq_scaling_governor(void);
static vfs_node_t *sysfs_get_cpu0_topology(void);
static vfs_node_t *sysfs_get_cpu0_topology_core_id(void);
static vfs_node_t *sysfs_get_cpu0_topology_physical_package_id(void);
static vfs_node_t *sysfs_get_cpu0_topology_core_siblings(void);
static vfs_node_t *sysfs_get_cpu0_topology_thread_siblings(void);
static vfs_node_t *sysfs_get_cpu_online(void);
static vfs_node_t *sysfs_get_cpu_possible(void);
static vfs_node_t *sysfs_get_cpu_present(void);
static vfs_node_t *sysfs_get_cpu_kernel_max(void);
static vfs_node_t *sysfs_get_class(void);
static vfs_node_t *sysfs_get_class_power_supply(void);
static vfs_node_t *sysfs_get_block(void);
static vfs_node_t *sysfs_get_kernel(void);
static vfs_node_t *sysfs_get_kernel_mm(void);
static vfs_node_t *sysfs_get_kernel_mm_hugepages(void);
static vfs_node_t *sysfs_get_fs(void);
static vfs_node_t *sysfs_get_bus(void);

static vfs_node_t *sysfs_get_devices(void) {
    return sysfs_lazy_node(&sysfs_devices, "devices", VFS_DIRECTORY, &sysfs_root, NULL, sysfs_devices_readdir, sysfs_devices_finddir);
}

static vfs_node_t *sysfs_get_devices_system(void) {
    return sysfs_lazy_node(&sysfs_devices_system, "system", VFS_DIRECTORY, sysfs_get_devices(), NULL, sysfs_system_readdir, sysfs_system_finddir);
}

static vfs_node_t *sysfs_get_devices_system_cpu(void) {
    return sysfs_lazy_node(&sysfs_devices_system_cpu, "cpu", VFS_DIRECTORY, sysfs_get_devices_system(), NULL, sysfs_cpu_readdir, sysfs_cpu_finddir);
}

static vfs_node_t *sysfs_get_devices_system_cpu_cpu0(void) {
    return sysfs_lazy_node(&sysfs_devices_system_cpu_cpu0, "cpu0", VFS_DIRECTORY, sysfs_get_devices_system_cpu(), NULL, sysfs_cpu0_readdir, sysfs_cpu0_finddir);
}

static vfs_node_t *sysfs_get_cpu0_online(void) {
    return sysfs_lazy_node(&sysfs_cpu0_online, "online", VFS_FILE, sysfs_get_devices_system_cpu_cpu0(), sysfs_cpu0_online_read, NULL, NULL);
}

static __attribute__((unused)) vfs_node_t *sysfs_get_cpu0_cpufreq(void) {
    return sysfs_lazy_node(&sysfs_cpu0_cpufreq, "cpufreq", VFS_DIRECTORY, sysfs_get_devices_system_cpu_cpu0(), NULL, sysfs_cpufreq_readdir, sysfs_cpufreq_finddir);
}

static vfs_node_t *sysfs_get_cpu0_cpufreq_scaling_cur_freq(void) {
    return sysfs_lazy_node(&sysfs_cpu0_cpufreq_scaling_cur_freq, "scaling_cur_freq", VFS_FILE, sysfs_get_cpu0_cpufreq(), sysfs_cpufreq_cur_read, NULL, NULL);
}

static vfs_node_t *sysfs_get_cpu0_cpufreq_scaling_min_freq(void) {
    return sysfs_lazy_node(&sysfs_cpu0_cpufreq_scaling_min_freq, "scaling_min_freq", VFS_FILE, sysfs_get_cpu0_cpufreq(), sysfs_cpufreq_min_read, NULL, NULL);
}

static vfs_node_t *sysfs_get_cpu0_cpufreq_scaling_max_freq(void) {
    return sysfs_lazy_node(&sysfs_cpu0_cpufreq_scaling_max_freq, "scaling_max_freq", VFS_FILE, sysfs_get_cpu0_cpufreq(), sysfs_cpufreq_max_read, NULL, NULL);
}

static vfs_node_t *sysfs_get_cpu0_cpufreq_cpuinfo_min_freq(void) {
    return sysfs_lazy_node(&sysfs_cpu0_cpufreq_cpuinfo_min_freq, "cpuinfo_min_freq", VFS_FILE, sysfs_get_cpu0_cpufreq(), sysfs_cpufreq_min_read, NULL, NULL);
}

static vfs_node_t *sysfs_get_cpu0_cpufreq_cpuinfo_max_freq(void) {
    return sysfs_lazy_node(&sysfs_cpu0_cpufreq_cpuinfo_max_freq, "cpuinfo_max_freq", VFS_FILE, sysfs_get_cpu0_cpufreq(), sysfs_cpufreq_max_read, NULL, NULL);
}

static vfs_node_t *sysfs_get_cpu0_cpufreq_scaling_governor(void) {
    return sysfs_lazy_node(&sysfs_cpu0_cpufreq_scaling_governor, "scaling_governor", VFS_FILE, sysfs_get_cpu0_cpufreq(), sysfs_cpufreq_governor_read, NULL, NULL);
}

static __attribute__((unused)) vfs_node_t *sysfs_get_cpu0_topology(void) {
    return sysfs_lazy_node(&sysfs_cpu0_topology, "topology", VFS_DIRECTORY, sysfs_get_devices_system_cpu_cpu0(), NULL, sysfs_topology_readdir, sysfs_topology_finddir);
}

static vfs_node_t *sysfs_get_cpu0_topology_core_id(void) {
    return sysfs_lazy_node(&sysfs_cpu0_topology_core_id, "core_id", VFS_FILE, sysfs_get_cpu0_topology(), sysfs_topology_core_id_read, NULL, NULL);
}

static vfs_node_t *sysfs_get_cpu0_topology_physical_package_id(void) {
    return sysfs_lazy_node(&sysfs_cpu0_topology_physical_package_id, "physical_package_id", VFS_FILE, sysfs_get_cpu0_topology(), sysfs_topology_phys_pkg_read, NULL, NULL);
}

static vfs_node_t *sysfs_get_cpu0_topology_core_siblings(void) {
    return sysfs_lazy_node(&sysfs_cpu0_topology_core_siblings, "core_siblings", VFS_FILE, sysfs_get_cpu0_topology(), sysfs_topology_core_siblings_read, NULL, NULL);
}

static vfs_node_t *sysfs_get_cpu0_topology_thread_siblings(void) {
    return sysfs_lazy_node(&sysfs_cpu0_topology_thread_siblings, "thread_siblings", VFS_FILE, sysfs_get_cpu0_topology(), sysfs_topology_thread_siblings_read, NULL, NULL);
}

static vfs_node_t *sysfs_get_cpu_online(void) {
    return sysfs_lazy_node(&sysfs_cpu_online, "online", VFS_FILE, sysfs_get_devices_system_cpu(), sysfs_cpu_online_read, NULL, NULL);
}

static vfs_node_t *sysfs_get_cpu_possible(void) {
    return sysfs_lazy_node(&sysfs_cpu_possible, "possible", VFS_FILE, sysfs_get_devices_system_cpu(), sysfs_cpu_possible_read, NULL, NULL);
}

static vfs_node_t *sysfs_get_cpu_present(void) {
    return sysfs_lazy_node(&sysfs_cpu_present, "present", VFS_FILE, sysfs_get_devices_system_cpu(), sysfs_cpu_present_read, NULL, NULL);
}

static vfs_node_t *sysfs_get_cpu_kernel_max(void) {
    return sysfs_lazy_node(&sysfs_cpu_kernel_max, "kernel_max", VFS_FILE, sysfs_get_devices_system_cpu(), sysfs_cpu_kernel_max_read, NULL, NULL);
}

static vfs_node_t *sysfs_get_class(void) {
    return sysfs_lazy_node(&sysfs_class, "class", VFS_DIRECTORY, &sysfs_root, NULL, sysfs_class_readdir, sysfs_class_finddir);
}

static vfs_node_t *sysfs_get_class_power_supply(void) {
    return sysfs_lazy_node(&sysfs_class_power_supply, "power_supply", VFS_DIRECTORY, sysfs_get_class(), NULL, sysfs_empty_readdir, sysfs_empty_finddir);
}

static vfs_node_t *sysfs_get_block(void) {
    return sysfs_lazy_node(&sysfs_block, "block", VFS_DIRECTORY, &sysfs_root, NULL, sysfs_empty_readdir, sysfs_empty_finddir);
}

static vfs_node_t *sysfs_get_kernel(void) {
    return sysfs_lazy_node(&sysfs_kernel, "kernel", VFS_DIRECTORY, &sysfs_root, NULL, sysfs_kernel_readdir, sysfs_kernel_finddir);
}

static vfs_node_t *sysfs_get_kernel_mm(void) {
    return sysfs_lazy_node(&sysfs_kernel_mm, "mm", VFS_DIRECTORY, sysfs_get_kernel(), NULL, sysfs_kernel_mm_readdir, sysfs_kernel_mm_finddir);
}

static vfs_node_t *sysfs_get_kernel_mm_hugepages(void) {
    return sysfs_lazy_node(&sysfs_kernel_mm_hugepages, "hugepages", VFS_DIRECTORY, sysfs_get_kernel_mm(), NULL, sysfs_empty_readdir, sysfs_empty_finddir);
}

static vfs_node_t *sysfs_get_fs(void) {
    return sysfs_lazy_node(&sysfs_fs, "fs", VFS_DIRECTORY, &sysfs_root, NULL, sysfs_empty_readdir, sysfs_empty_finddir);
}

static vfs_node_t *sysfs_get_bus(void) {
    return sysfs_lazy_node(&sysfs_bus, "bus", VFS_DIRECTORY, &sysfs_root, NULL, sysfs_empty_readdir, sysfs_empty_finddir);
}

static dirent_t *sysfs_cpufreq_readdir(vfs_node_t *node, uint64_t index) {
    dirent_t *d;
    static const char *names[] = {
        "scaling_cur_freq", "scaling_min_freq", "scaling_max_freq",
        "cpuinfo_min_freq", "cpuinfo_max_freq", "scaling_governor"
    };

    (void)node;

    if (index >= 6) return NULL;
    d = sysfs_alloc_dirent();
    if (!d) return NULL;
    strcpy(d->name, names[index]);
    d->inode = 200 + index;
    d->type = VFS_FILE;
    return d;
}

static vfs_node_t *sysfs_cpufreq_finddir(vfs_node_t *node, const char *name) {
    (void)node;

    if (strcmp(name, "scaling_cur_freq") == 0) return sysfs_get_cpu0_cpufreq_scaling_cur_freq();
    if (strcmp(name, "scaling_min_freq") == 0) return sysfs_get_cpu0_cpufreq_scaling_min_freq();
    if (strcmp(name, "scaling_max_freq") == 0) return sysfs_get_cpu0_cpufreq_scaling_max_freq();
    if (strcmp(name, "cpuinfo_min_freq") == 0) return sysfs_get_cpu0_cpufreq_cpuinfo_min_freq();
    if (strcmp(name, "cpuinfo_max_freq") == 0) return sysfs_get_cpu0_cpufreq_cpuinfo_max_freq();
    if (strcmp(name, "scaling_governor") == 0) return sysfs_get_cpu0_cpufreq_scaling_governor();
    return NULL;
}

static dirent_t *sysfs_topology_readdir(vfs_node_t *node, uint64_t index) {
    dirent_t *d;
    static const char *names[] = {
        "core_id", "physical_package_id", "core_siblings", "thread_siblings"
    };

    (void)node;

    if (index >= 4) return NULL;
    d = sysfs_alloc_dirent();
    if (!d) return NULL;
    strcpy(d->name, names[index]);
    d->inode = 300 + index;
    d->type = VFS_FILE;
    return d;
}

static vfs_node_t *sysfs_topology_finddir(vfs_node_t *node, const char *name) {
    (void)node;

    if (strcmp(name, "core_id") == 0) return sysfs_get_cpu0_topology_core_id();
    if (strcmp(name, "physical_package_id") == 0) return sysfs_get_cpu0_topology_physical_package_id();
    if (strcmp(name, "core_siblings") == 0) return sysfs_get_cpu0_topology_core_siblings();
    if (strcmp(name, "thread_siblings") == 0) return sysfs_get_cpu0_topology_thread_siblings();
    return NULL;
}

static dirent_t *sysfs_cpu0_readdir(vfs_node_t *node, uint64_t index) {
    dirent_t *d;

    (void)node;

    d = sysfs_alloc_dirent();
    if (!d) return NULL;
    if (index == 0) {
        strcpy(d->name, "online");
        d->inode = 50;
        d->type = VFS_FILE;
        return d;
    }
    return NULL;
}

static vfs_node_t *sysfs_cpu0_finddir(vfs_node_t *node, const char *name) {
    (void)node;

    if (strcmp(name, "online") == 0) return sysfs_get_cpu0_online();
    return NULL;
}

static dirent_t *sysfs_dynamic_cpu_readdir(vfs_node_t *node,
                                            uint64_t index) {
    dirent_t *entry;

    (void)node;
    if (index != 0) return NULL;
    entry = sysfs_alloc_dirent();
    if (!entry) return NULL;
    strcpy(entry->name, "online");
    entry->inode = 1;
    entry->type = VFS_FILE;
    return entry;
}

static vfs_node_t *sysfs_get_dynamic_cpu_online(int cpu) {
    vfs_node_t *node;

    if (!sysfs_cpu_dynamic_online || cpu <= 0 ||
        cpu >= sysfs_cpu_dynamic_capacity) return NULL;
    node = sysfs_cpu_dynamic_online[cpu];
    if (node) return node;
    node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) return NULL;
    sysfs_init_node(node, "online", VFS_FILE,
                    sysfs_cpu_dynamic_nodes[cpu]);
    node->inode = (uint64_t)cpu;
    node->read = sysfs_cpu0_online_read;
    sysfs_cpu_dynamic_online[cpu] = node;
    return node;
}

static vfs_node_t *sysfs_dynamic_cpu_finddir(vfs_node_t *node,
                                              const char *name) {
    int cpu;

    if (!node || strcmp(name, "online") != 0) return NULL;
    cpu = (int)node->inode;
    return sysfs_get_dynamic_cpu_online(cpu);
}

static int sysfs_ensure_dynamic_cpus(void) {
    vfs_node_t **new_nodes;
    vfs_node_t **new_online;
    size_t bytes;
    size_t old_bytes;

    if (sysfs_cpu_dynamic_capacity >= cpu_count) return 0;
    bytes = (size_t)cpu_count * sizeof(vfs_node_t *);
    new_nodes = (vfs_node_t **)kmalloc(bytes);
    if (!new_nodes) return -1;
    new_online = (vfs_node_t **)kmalloc(bytes);
    if (!new_online) {
        kfree(new_nodes);
        return -1;
    }
    memset(new_nodes, 0, bytes);
    memset(new_online, 0, bytes);
    if (sysfs_cpu_dynamic_capacity > 0) {
        old_bytes = (size_t)sysfs_cpu_dynamic_capacity *
                    sizeof(vfs_node_t *);
        memcpy(new_nodes, sysfs_cpu_dynamic_nodes, old_bytes);
        memcpy(new_online, sysfs_cpu_dynamic_online, old_bytes);
    }
    kfree(sysfs_cpu_dynamic_nodes);
    kfree(sysfs_cpu_dynamic_online);
    sysfs_cpu_dynamic_nodes = new_nodes;
    sysfs_cpu_dynamic_online = new_online;
    sysfs_cpu_dynamic_capacity = cpu_count;
    return 0;
}

static vfs_node_t *sysfs_get_dynamic_cpu(int cpu) {
    char name[16];
    vfs_node_t *node;

    if (cpu == 0) return sysfs_get_devices_system_cpu_cpu0();
    if (cpu < 0 || cpu >= cpu_count || sysfs_ensure_dynamic_cpus() != 0)
        return NULL;
    node = sysfs_cpu_dynamic_nodes[cpu];
    if (node) return node;
    snprintf(name, sizeof(name), "cpu%d", cpu);
    node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) return NULL;
    sysfs_init_node(node, name, VFS_DIRECTORY,
                    sysfs_get_devices_system_cpu());
    node->inode = (uint64_t)cpu;
    node->readdir = sysfs_dynamic_cpu_readdir;
    node->finddir = sysfs_dynamic_cpu_finddir;
    sysfs_cpu_dynamic_nodes[cpu] = node;
    return node;
}

static dirent_t *sysfs_cpu_readdir(vfs_node_t *node, uint64_t index) {
    dirent_t *d;

    (void)node;

    d = sysfs_alloc_dirent();
    if (!d) return NULL;
    if (index < (uint64_t)cpu_count) {
        snprintf(d->name, sizeof(d->name), "cpu%lu", index);
        d->inode = 40 + index;
        d->type = VFS_DIRECTORY;
        return d;
    }
    switch (index - (uint64_t)cpu_count) {
    case 0:
        strcpy(d->name, "online");
        d->inode = 41;
        d->type = VFS_FILE;
        return d;
    case 1:
        strcpy(d->name, "possible");
        d->inode = 42;
        d->type = VFS_FILE;
        return d;
    case 2:
        strcpy(d->name, "present");
        d->inode = 43;
        d->type = VFS_FILE;
        return d;
    case 3:
        strcpy(d->name, "kernel_max");
        d->inode = 44;
        d->type = VFS_FILE;
        return d;
    default:
        break;
    }
    return NULL;
}

static vfs_node_t *sysfs_cpu_finddir(vfs_node_t *node, const char *name) {
    int cpu;
    int position;

    (void)node;

    if (name[0] == 'c' && name[1] == 'p' && name[2] == 'u') {
        cpu = 0;
        position = 3;
        if (name[position] < '0' || name[position] > '9') return NULL;
        while (name[position] >= '0' && name[position] <= '9') {
            cpu = cpu * 10 + name[position] - '0';
            position++;
        }
        if (name[position] != '\0') return NULL;
        return sysfs_get_dynamic_cpu(cpu);
    }
    if (strcmp(name, "online") == 0) return sysfs_get_cpu_online();
    if (strcmp(name, "possible") == 0) return sysfs_get_cpu_possible();
    if (strcmp(name, "present") == 0) return sysfs_get_cpu_present();
    if (strcmp(name, "kernel_max") == 0) return sysfs_get_cpu_kernel_max();
    return NULL;
}

static dirent_t *sysfs_system_readdir(vfs_node_t *node, uint64_t index) {
    dirent_t *d;

    (void)node;

    if (index == 0) {
        d = sysfs_alloc_dirent();
        if (!d) return NULL;
        strcpy(d->name, "cpu");
        d->inode = 30;
        d->type = VFS_DIRECTORY;
        return d;
    }
    return NULL;
}

static vfs_node_t *sysfs_system_finddir(vfs_node_t *node, const char *name) {
    (void)node;

    if (strcmp(name, "cpu") == 0) return sysfs_get_devices_system_cpu();
    return NULL;
}

static dirent_t *sysfs_devices_readdir(vfs_node_t *node, uint64_t index) {
    dirent_t *d;

    (void)node;

    if (index == 0) {
        d = sysfs_alloc_dirent();
        if (!d) return NULL;
        strcpy(d->name, "system");
        d->inode = 20;
        d->type = VFS_DIRECTORY;
        return d;
    }
    return NULL;
}

static vfs_node_t *sysfs_devices_finddir(vfs_node_t *node, const char *name) {
    (void)node;

    if (strcmp(name, "system") == 0) return sysfs_get_devices_system();
    return NULL;
}

static dirent_t *sysfs_empty_readdir(vfs_node_t *node, uint64_t index) {
    (void)node;
    (void)index;
    return NULL;
}

static vfs_node_t *sysfs_empty_finddir(vfs_node_t *node, const char *name) {
    (void)node;
    (void)name;
    return NULL;
}

static dirent_t *sysfs_class_readdir(vfs_node_t *node, uint64_t index) {
    dirent_t *d;

    (void)node;

    if (index == 0) {
        d = sysfs_alloc_dirent();
        if (!d) return NULL;
        strcpy(d->name, "power_supply");
        d->inode = 500;
        d->type = VFS_DIRECTORY;
        return d;
    }
    return NULL;
}

static vfs_node_t *sysfs_class_finddir(vfs_node_t *node, const char *name) {
    (void)node;

    if (strcmp(name, "power_supply") == 0) return sysfs_get_class_power_supply();
    return NULL;
}

static dirent_t *sysfs_kernel_readdir(vfs_node_t *node, uint64_t index) {
    dirent_t *d;

    (void)node;

    if (index == 0) {
        d = sysfs_alloc_dirent();
        if (!d) return NULL;
        strcpy(d->name, "mm");
        d->inode = 600;
        d->type = VFS_DIRECTORY;
        return d;
    }
    return NULL;
}

static vfs_node_t *sysfs_kernel_finddir(vfs_node_t *node, const char *name) {
    (void)node;

    if (strcmp(name, "mm") == 0) return sysfs_get_kernel_mm();
    return NULL;
}

static dirent_t *sysfs_kernel_mm_readdir(vfs_node_t *node, uint64_t index) {
    dirent_t *d;

    (void)node;

    if (index == 0) {
        d = sysfs_alloc_dirent();
        if (!d) return NULL;
        strcpy(d->name, "hugepages");
        d->inode = 610;
        d->type = VFS_DIRECTORY;
        return d;
    }
    return NULL;
}

static vfs_node_t *sysfs_kernel_mm_finddir(vfs_node_t *node, const char *name) {
    (void)node;

    if (strcmp(name, "hugepages") == 0) return sysfs_get_kernel_mm_hugepages();
    return NULL;
}

static dirent_t *sysfs_root_readdir(vfs_node_t *node, uint64_t index) {
    dirent_t *d;
    static const char *names[] = {
        "devices", "class", "block", "kernel", "fs", "bus"
    };
    static const uint64_t inodes[] = { 10, 11, 12, 13, 14, 15 };

    (void)node;

    if (index >= 6) return NULL;
    d = sysfs_alloc_dirent();
    if (!d) return NULL;
    strcpy(d->name, names[index]);
    d->inode = inodes[index];
    d->type = VFS_DIRECTORY;
    return d;
}

static vfs_node_t *sysfs_root_finddir(vfs_node_t *node, const char *name) {
    (void)node;

    if (strcmp(name, "devices") == 0) return sysfs_get_devices();
    if (strcmp(name, "class") == 0) return sysfs_get_class();
    if (strcmp(name, "block") == 0) return sysfs_get_block();
    if (strcmp(name, "kernel") == 0) return sysfs_get_kernel();
    if (strcmp(name, "fs") == 0) return sysfs_get_fs();
    if (strcmp(name, "bus") == 0) return sysfs_get_bus();
    return NULL;
}

static vfs_node_t *sysfs_mount(const char *device, const char *mountpoint) {
    (void)device;
    (void)mountpoint;
    return &sysfs_root;
}

static int sysfs_unmount(vfs_node_t *node) {
    (void)node;
    return 0;
}

static vfs_fs_type_t sysfs_type;

void KERNEL_INIT sysfs_init(void) {
    sysfs_dirent_index = 0;
    sysfs_dirent_capacity = 0;
    sysfs_dirent_pool = NULL;

    sysfs_type.name = "sysfs";
    sysfs_type.mount = sysfs_mount;
    sysfs_type.unmount = sysfs_unmount;
    sysfs_type.next = NULL;

    sysfs_init_node(&sysfs_root, "sys", VFS_DIRECTORY, NULL);
    sysfs_root.readdir = sysfs_root_readdir;
    sysfs_root.finddir = sysfs_root_finddir;

    vfs_register_fs(&sysfs_type);
}
