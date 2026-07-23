#include "syscall_defs.h"
#include <lebirun/inotify.h>

#define INOTIFY_BASE_FD (TASK_MAX_FDS + 0x4001)
#define INOTIFY_NONBLOCK 0x800
#define INOTIFY_CLOEXEC 0x80000
#define INOTIFY_MASK_ADD 0x20000000
#define INOTIFY_MASK_CREATE 0x10000000
#define INOTIFY_IGNORED 0x00008000
#define INOTIFY_Q_OVERFLOW 0x00004000
#define INOTIFY_QUEUE_MAX 32
#define INOTIFY_NAME_MAX 63

typedef struct {
    int wd;
    vfs_node_t *node;
    uint32_t mask;
} inotify_watch_t;

typedef struct inotify_queued_event {
    struct inotify_queued_event *next;
    int wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t name_length;
    char name[INOTIFY_NAME_MAX + 1];
} inotify_queued_event_t;

typedef struct {
    int in_use;
    pid_t owner_pid;
    int flags;
    int next_wd;
    inotify_watch_t *watches;
    int watch_count;
    int watch_capacity;
    inotify_queued_event_t *head;
    inotify_queued_event_t *tail;
    int queue_count;
} inotify_instance_t;

static inotify_instance_t *inotify_instances;
static int inotify_capacity;
static mutex_t inotify_lock;

static int inotify_grow(void) {
    int new_capacity;
    inotify_instance_t *new_instances;
    int i;

    new_capacity = inotify_capacity ? inotify_capacity * 2 : 1;
    new_instances = (inotify_instance_t *)krealloc(
        inotify_instances, new_capacity * sizeof(inotify_instance_t));
    if (!new_instances) return -1;
    for (i = inotify_capacity; i < new_capacity; i++) {
        memset(&new_instances[i], 0, sizeof(inotify_instance_t));
    }
    inotify_instances = new_instances;
    inotify_capacity = new_capacity;
    return 0;
}

static int inotify_allocate(void) {
    int index;

    for (index = 0; index < inotify_capacity; index++) {
        if (!inotify_instances[index].in_use) return index;
    }
    if (inotify_grow() < 0) return -1;
    return inotify_capacity / 2;
}

static inotify_instance_t *inotify_get(int fd) {
    int index;
    inotify_instance_t *instance;

    index = fd - INOTIFY_BASE_FD;
    if (index < 0 || index >= inotify_capacity || !current_task) return NULL;
    instance = &inotify_instances[index];
    if (!instance->in_use || instance->owner_pid != current_task->pid)
        return NULL;
    return instance;
}

static void inotify_queue(inotify_instance_t *instance, int wd, uint32_t mask,
                          const char *name) {
    inotify_queued_event_t *event;
    inotify_queued_event_t *dropped;
    uint32_t length;

    if (!instance) return;
    if (instance->queue_count >= INOTIFY_QUEUE_MAX) {
        if (instance->tail && instance->tail->mask == INOTIFY_Q_OVERFLOW)
            return;
        dropped = instance->head;
        if (!dropped) return;
        instance->head = dropped->next;
        if (!instance->head) instance->tail = NULL;
        instance->queue_count--;
        kfree(dropped);
        wd = -1;
        mask = INOTIFY_Q_OVERFLOW;
        name = NULL;
    }
    event = (inotify_queued_event_t *)kmalloc(sizeof(inotify_queued_event_t));
    if (!event) return;
    memset(event, 0, sizeof(inotify_queued_event_t));
    event->wd = wd;
    event->mask = mask;
    length = 0;
    if (name) {
        while (length < INOTIFY_NAME_MAX && name[length]) {
            event->name[length] = name[length];
            length++;
        }
        event->name[length] = '\0';
        event->name_length = (length + 1 + 3) & ~3U;
    }
    if (instance->tail) {
        instance->tail->next = event;
    } else {
        instance->head = event;
    }
    instance->tail = event;
    instance->queue_count++;
}

static int inotify_create(int flags) {
    int index;

    if (flags & ~(INOTIFY_NONBLOCK | INOTIFY_CLOEXEC)) return -EINVAL;
    mutex_lock(&inotify_lock);
    index = inotify_allocate();
    if (index < 0) {
        mutex_unlock(&inotify_lock);
        return -EMFILE;
    }
    memset(&inotify_instances[index], 0, sizeof(inotify_instance_t));
    inotify_instances[index].in_use = 1;
    inotify_instances[index].owner_pid = current_task ? current_task->pid : 0;
    inotify_instances[index].flags = flags;
    inotify_instances[index].next_wd = 1;
    mutex_unlock(&inotify_lock);
    return INOTIFY_BASE_FD + index;
}

static int sys_inotify_init(int unused, const char *unused2, int unused3) {
    (void)unused;
    (void)unused2;
    (void)unused3;
    return inotify_create(0);
}

static int sys_inotify_init1(int flags, const char *unused, int unused2) {
    (void)unused;
    (void)unused2;
    return inotify_create(flags);
}

static int sys_inotify_add_watch(int fd, const char *pathname, uint64_t mask) {
    inotify_instance_t *instance;
    inotify_watch_t *new_watches;
    vfs_node_t *node;
    char path[VFS_MAX_PATH];
    int new_capacity;
    int i;
    int wd;

    if (copy_string_from_user(path, pathname, sizeof(path)) < 0) return -EFAULT;
    node = vfs_lookup(path);
    if (!node) return -ENOENT;
    mutex_lock(&inotify_lock);
    instance = inotify_get(fd);
    if (!instance) {
        mutex_unlock(&inotify_lock);
        vfs_release(node);
        return -EBADF;
    }
    for (i = 0; i < instance->watch_count; i++) {
        if (instance->watches[i].node != node) continue;
        if (mask & INOTIFY_MASK_CREATE) {
            mutex_unlock(&inotify_lock);
            vfs_release(node);
            return -EEXIST;
        }
        if (mask & INOTIFY_MASK_ADD) {
            instance->watches[i].mask |= (uint32_t)mask;
        } else {
            instance->watches[i].mask = (uint32_t)mask;
        }
        wd = instance->watches[i].wd;
        mutex_unlock(&inotify_lock);
        vfs_release(node);
        return wd;
    }
    if (instance->watch_count == instance->watch_capacity) {
        new_capacity = instance->watch_capacity ?
                       instance->watch_capacity * 2 : 1;
        new_watches = (inotify_watch_t *)krealloc(
            instance->watches, new_capacity * sizeof(inotify_watch_t));
        if (!new_watches) {
            mutex_unlock(&inotify_lock);
            vfs_release(node);
            return -ENOMEM;
        }
        instance->watches = new_watches;
        instance->watch_capacity = new_capacity;
    }
    wd = instance->next_wd++;
    instance->watches[instance->watch_count].wd = wd;
    instance->watches[instance->watch_count].node = node;
    instance->watches[instance->watch_count].mask = (uint32_t)mask;
    instance->watch_count++;
    vfs_open(node, 0);
    mutex_unlock(&inotify_lock);
    vfs_release(node);
    return wd;
}

static int sys_inotify_rm_watch(int fd, const char *wd_ptr, int unused) {
    inotify_instance_t *instance;
    vfs_node_t *node;
    int wd;
    int i;

    (void)unused;
    wd = (int)(uintptr_t)wd_ptr;
    mutex_lock(&inotify_lock);
    instance = inotify_get(fd);
    if (!instance) {
        mutex_unlock(&inotify_lock);
        return -EBADF;
    }
    for (i = 0; i < instance->watch_count; i++) {
        if (instance->watches[i].wd != wd) continue;
        node = instance->watches[i].node;
        inotify_queue(instance, wd, INOTIFY_IGNORED, NULL);
        if (i + 1 < instance->watch_count) {
            memmove(&instance->watches[i], &instance->watches[i + 1],
                    (instance->watch_count - i - 1) * sizeof(inotify_watch_t));
        }
        instance->watch_count--;
        if (instance->watch_count == 0) {
            kfree(instance->watches);
            instance->watches = NULL;
            instance->watch_capacity = 0;
        }
        mutex_unlock(&inotify_lock);
        vfs_close(node);
        return 0;
    }
    mutex_unlock(&inotify_lock);
    return -EINVAL;
}

int inotify_is_fd(int fd) {
    int index;
    int result;

    index = fd - INOTIFY_BASE_FD;
    result = 0;
    mutex_lock(&inotify_lock);
    if (index >= 0 && index < inotify_capacity && inotify_instances &&
        inotify_instances[index].in_use && current_task &&
        inotify_instances[index].owner_pid == current_task->pid) {
        result = 1;
    }
    mutex_unlock(&inotify_lock);
    return result;
}

int inotify_poll_fd(int fd) {
    inotify_instance_t *instance;
    int ready;

    mutex_lock(&inotify_lock);
    instance = inotify_get(fd);
    ready = instance && instance->head ? 1 : 0;
    mutex_unlock(&inotify_lock);
    return instance ? ready : -EBADF;
}

int inotify_read_fd(int fd, void *buffer, int length) {
    inotify_instance_t *instance;
    inotify_queued_event_t *event;
    uint8_t output[16 + INOTIFY_NAME_MAX + 4];
    uint32_t output_size;
    int flags;

retry_read:
    mutex_lock(&inotify_lock);
    instance = inotify_get(fd);
    if (!instance) {
        mutex_unlock(&inotify_lock);
        return -EBADF;
    }
    event = instance->head;
    if (!event) {
        flags = instance->flags;
        mutex_unlock(&inotify_lock);
        if (flags & INOTIFY_NONBLOCK) return -EAGAIN;
        if (signal_pending_mask(current_task)) return -EINTR;
        sleep_ticks(1);
        goto retry_read;
    }
    output_size = 16 + event->name_length;
    if (length < (int)output_size) {
        mutex_unlock(&inotify_lock);
        return -EINVAL;
    }
    if (!user_access_ok(buffer, output_size, UACCESS_WRITE)) {
        mutex_unlock(&inotify_lock);
        return -EFAULT;
    }
    memset(output, 0, sizeof(output));
    memcpy(output, &event->wd, sizeof(event->wd));
    memcpy(output + 4, &event->mask, sizeof(event->mask));
    memcpy(output + 8, &event->cookie, sizeof(event->cookie));
    memcpy(output + 12, &event->name_length, sizeof(event->name_length));
    if (event->name_length) memcpy(output + 16, event->name,
                                   event->name_length);
    instance->head = event->next;
    if (!instance->head) instance->tail = NULL;
    instance->queue_count--;
    mutex_unlock(&inotify_lock);
    if (copy_to_user(buffer, output, output_size) < 0) {
        kfree(event);
        return -EFAULT;
    }
    kfree(event);
    return (int)output_size;
}

int inotify_close_fd(int fd) {
    inotify_instance_t *instance;
    inotify_queued_event_t *event;
    inotify_queued_event_t *next;
    inotify_watch_t *watches;
    int watch_count;
    int i;
    int any_in_use;

    watches = NULL;
    watch_count = 0;
    mutex_lock(&inotify_lock);
    instance = inotify_get(fd);
    if (!instance) {
        mutex_unlock(&inotify_lock);
        return -EBADF;
    }
    watches = instance->watches;
    watch_count = instance->watch_count;
    event = instance->head;
    while (event) {
        next = event->next;
        kfree(event);
        event = next;
    }
    memset(instance, 0, sizeof(inotify_instance_t));
    any_in_use = 0;
    for (i = 0; i < inotify_capacity; i++) {
        if (inotify_instances[i].in_use) {
            any_in_use = 1;
            break;
        }
    }
    if (!any_in_use) {
        kfree(inotify_instances);
        inotify_instances = NULL;
        inotify_capacity = 0;
    }
    mutex_unlock(&inotify_lock);
    for (i = 0; i < watch_count; i++) vfs_close(watches[i].node);
    kfree(watches);
    return 0;
}

void inotify_close_task(pid_t pid) {
    inotify_queued_event_t *event;
    inotify_queued_event_t *next;
    inotify_watch_t *watches;
    int watch_count;
    int index;
    int i;
    int any_in_use;

    for (;;) {
        watches = NULL;
        watch_count = 0;
        event = NULL;
        index = -1;
        mutex_lock(&inotify_lock);
        for (i = 0; i < inotify_capacity; i++) {
            if (inotify_instances[i].in_use &&
                inotify_instances[i].owner_pid == pid) {
                index = i;
                break;
            }
        }
        if (index >= 0) {
            watches = inotify_instances[index].watches;
            watch_count = inotify_instances[index].watch_count;
            event = inotify_instances[index].head;
            memset(&inotify_instances[index], 0,
                   sizeof(inotify_instance_t));
        }
        any_in_use = 0;
        for (i = 0; i < inotify_capacity; i++) {
            if (inotify_instances[i].in_use) {
                any_in_use = 1;
                break;
            }
        }
        if (!any_in_use) {
            kfree(inotify_instances);
            inotify_instances = NULL;
            inotify_capacity = 0;
        }
        mutex_unlock(&inotify_lock);
        while (event) {
            next = event->next;
            kfree(event);
            event = next;
        }
        for (i = 0; i < watch_count; i++) vfs_close(watches[i].node);
        kfree(watches);
        if (index < 0) return;
    }
}

void inotify_notify(vfs_node_t *node, uint32_t mask, const char *name) {
    inotify_instance_t *instance;
    inotify_watch_t *watch;
    int i;
    int j;

    if (!node) return;
    mutex_lock(&inotify_lock);
    for (i = 0; i < inotify_capacity; i++) {
        instance = &inotify_instances[i];
        if (!instance->in_use) continue;
        for (j = 0; j < instance->watch_count; j++) {
            watch = &instance->watches[j];
            if (watch->node == node && (watch->mask & mask)) {
                inotify_queue(instance, watch->wd, mask, name);
            }
        }
    }
    mutex_unlock(&inotify_lock);
}

void syscalls_inotify_init(void) {
    inotify_instances = NULL;
    inotify_capacity = 0;
    mutex_init(&inotify_lock);
    syscall_table[SYSCALL_INOTIFY_INIT] = sys_inotify_init;
    syscall_table[SYSCALL_INOTIFY_INIT1] = sys_inotify_init1;
    syscall_table[SYSCALL_INOTIFY_ADD_WATCH] = sys_inotify_add_watch;
    syscall_table[SYSCALL_INOTIFY_RM_WATCH] = sys_inotify_rm_watch;
}
