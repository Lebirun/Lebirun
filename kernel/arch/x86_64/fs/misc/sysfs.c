#include <lebirun/vfs.h>
#include <lebirun/common.h>
#include <lebirun/pit.h>
#include <lebirun/mem_map.h>
#include <string.h>
#include <stdio.h>

extern uint64_t pit_freq;

static dirent_t *sysfs_dirent_pool;
static volatile uint64_t sysfs_dirent_index;
static uint64_t sysfs_dirent_capacity;

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
    return sysfs_read_static(node, offset, size, buffer, "1\n", 2);
}

static uint64_t sysfs_cpu_online_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    return sysfs_read_static(node, offset, size, buffer, "0-0\n", 4);
}

static uint64_t sysfs_cpu_possible_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    return sysfs_read_static(node, offset, size, buffer, "0-0\n", 4);
}

static uint64_t sysfs_cpu_present_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    return sysfs_read_static(node, offset, size, buffer, "0-0\n", 4);
}

static uint64_t sysfs_cpu_kernel_max_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    return sysfs_read_static(node, offset, size, buffer, "0\n", 2);
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
    strcpy(n->name, name);
    n->flags = flags;
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
    kfree(node);
}

void sysfs_reclaim_unused(void) {
    uint64_t i;
    uint64_t count;

    count = sizeof(sysfs_reclaim_slots) / sizeof(sysfs_reclaim_slots[0]);
    for (i = 0; i < count; i++) {
        sysfs_reclaim_lazy_node(sysfs_reclaim_slots[i], NULL);
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

static vfs_node_t *sysfs_get_cpu0_cpufreq(void) {
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

static vfs_node_t *sysfs_get_cpu0_topology(void) {
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
    switch (index) {
    case 0:
        strcpy(d->name, "online");
        d->inode = 50;
        d->type = VFS_FILE;
        return d;
    case 1:
        strcpy(d->name, "cpufreq");
        d->inode = 51;
        d->type = VFS_DIRECTORY;
        return d;
    case 2:
        strcpy(d->name, "topology");
        d->inode = 52;
        d->type = VFS_DIRECTORY;
        return d;
    default:
        break;
    }
    return NULL;
}

static vfs_node_t *sysfs_cpu0_finddir(vfs_node_t *node, const char *name) {
    (void)node;

    if (strcmp(name, "online") == 0) return sysfs_get_cpu0_online();
    if (strcmp(name, "cpufreq") == 0) return sysfs_get_cpu0_cpufreq();
    if (strcmp(name, "topology") == 0) return sysfs_get_cpu0_topology();
    return NULL;
}

static dirent_t *sysfs_cpu_readdir(vfs_node_t *node, uint64_t index) {
    dirent_t *d;

    (void)node;

    d = sysfs_alloc_dirent();
    if (!d) return NULL;
    switch (index) {
    case 0:
        strcpy(d->name, "cpu0");
        d->inode = 40;
        d->type = VFS_DIRECTORY;
        return d;
    case 1:
        strcpy(d->name, "online");
        d->inode = 41;
        d->type = VFS_FILE;
        return d;
    case 2:
        strcpy(d->name, "possible");
        d->inode = 42;
        d->type = VFS_FILE;
        return d;
    case 3:
        strcpy(d->name, "present");
        d->inode = 43;
        d->type = VFS_FILE;
        return d;
    case 4:
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
    (void)node;

    if (strcmp(name, "cpu0") == 0) return sysfs_get_devices_system_cpu_cpu0();
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
