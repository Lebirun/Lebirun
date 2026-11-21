#include "task_priv.h"

#define FD_POS_MARK (1ULL << 62)

typedef struct fd_table_head {
    int refs;
    struct fd_table_head *retired;
} fd_table_head_t;

typedef struct {
    int id;
    uint64_t offset;
} fd_pos_t;

spinlock_t task_fd_array_lock = {0};
static spinlock_t fd_pos_lock = {0};
static fd_pos_t *fd_pos;
static int fd_pos_count;
static int fd_pos_capacity;
static int fd_pos_live = 4;
static int fd_pos_seq = 1;

static fd_table_head_t *fd_head(task_fd_t *fds) {
    return (fd_table_head_t *)fds - 1;
}

static task_fd_t *fd_table_alloc(int cap) {
    fd_table_head_t *head;

    head = (fd_table_head_t *)kmalloc(sizeof(*head) +
                                      (uint64_t)cap * sizeof(task_fd_t));
    if (!head) return NULL;
    head->refs = 1;
    head->retired = NULL;
    memset(head + 1, 0, (uint64_t)cap * sizeof(task_fd_t));
    return (task_fd_t *)(head + 1);
}

static void fd_table_free(task_fd_t *fds) {
    fd_table_head_t *head;
    fd_table_head_t *next;

    for (head = fd_head(fds); head; head = next) {
        next = head->retired;
        kfree(head);
    }
}

static void fd_table_replace_locked(task_t *task, task_fd_t *old_fds,
                                    task_fd_t *new_fds, int cap) {
    task_t *t;

    if (fd_head(old_fds)->refs <= 1) {
        task->fds = new_fds;
        task->fds_capacity = cap;
        return;
    }
    lock_scheduler();
    for (t = all_tasks_head; t && task_ptr_valid(t); t = t->all_next) {
        if (t->fds != old_fds) continue;
        t->fds = new_fds;
        t->fds_capacity = cap;
    }
    unlock_scheduler();
}

void task_init_fds(task_t *task) {
    if (!task) return;
    task->fds = fd_table_alloc(TASK_INIT_FDS);
    if (!task->fds) {
        task->fds_capacity = 0;
        return;
    }
    task->fds_capacity = TASK_INIT_FDS;
    task->fds[0].in_use = 1;
    task->fds[0].type = FD_TYPE_STDIN;
    task->fds[0].ref_count = 1;
    task->fds[1].in_use = 1;
    task->fds[1].type = FD_TYPE_STDOUT;
    task->fds[1].ref_count = 1;
    task->fds[2].in_use = 1;
    task->fds[2].type = FD_TYPE_STDERR;
    task->fds[2].ref_count = 1;
}

static task_fd_t *fd_table_dup(task_t *parent, int *cap_out) {
    task_fd_t *fds;
    task_fd_t *src;
    int cap;
    int count;
    int i;

    cap = parent->fds_capacity;
    if (cap < TASK_INIT_FDS) cap = TASK_INIT_FDS;
    fds = fd_table_alloc(cap);
    if (!fds) return NULL;
    spin_lock(&task_fd_array_lock);
    src = parent->fds;
    count = src ? parent->fds_capacity : 0;
    if (count > cap) count = cap;
    if (count) memcpy(fds, src, (uint64_t)count * sizeof(task_fd_t));
    spin_unlock(&task_fd_array_lock);
    for (i = 0; i < count; i++) {
        if (!fds[i].in_use) continue;
        if (fds[i].type == FD_TYPE_FILE && fds[i].node) {
            task_fd_position_share(&src[i], &fds[i]);
            vfs_open((vfs_node_t *)fds[i].node, 0);
        }
        if (FD_TYPE_IS_PTY(fds[i].type))
            pty_retain_endpoint((int)(uintptr_t)fds[i].private_data);
        if (FD_TYPE_IS_PIPE(fds[i].type) && fds[i].private_data)
            pipe_retain_reference((pipe_t *)fds[i].private_data, fds[i].type);
        if (fds[i].type == FD_TYPE_SOCKET) socket_retain_task_fd(&fds[i]);
    }
    *cap_out = cap;
    return fds;
}

int task_fd_copy(task_t *child, task_t *parent) {
    child->fds = fd_table_dup(parent, &child->fds_capacity);
    return child->fds ? 0 : -1;
}

void task_fd_unshare(task_t *task) {
    task_fd_t *fds;
    task_fd_t *old;
    int old_cap;
    int refs;
    int cap;

    spin_lock(&task_fd_array_lock);
    old = task->fds;
    if (old && fd_head(old)->refs > 1) fd_head(old)->refs++;
    else old = NULL;
    spin_unlock(&task_fd_array_lock);
    if (!old) return;
    fds = fd_table_dup(task, &cap);
    spin_lock(&task_fd_array_lock);
    old = task->fds;
    old_cap = task->fds_capacity;
    refs = fd_head(old)->refs -= fds ? 2 : 1;
    if (fds) {
        task->fds = fds;
        task->fds_capacity = cap;
    }
    spin_unlock(&task_fd_array_lock);
    if (!fds || refs) return;
    fd_head(old)->refs = 1;
    task->fds = old;
    task->fds_capacity = old_cap;
    socket_close_task(task);
    task_fd_close_all(task);
    spin_lock(&task_fd_array_lock);
    task->fds = fds;
    task->fds_capacity = cap;
    spin_unlock(&task_fd_array_lock);
    fd_table_free(old);
}

void task_fd_share(task_t *child, task_t *parent) {
    child->fds = parent->fds;
    child->fds_capacity = parent->fds_capacity;
    if (child->fds) fd_head(child->fds)->refs++;
}

void task_fd_detach(task_t *task) {
    if (!task) return;
    spin_lock(&task_fd_array_lock);
    if (task->fds && fd_head(task->fds)->refs > 1) {
        fd_head(task->fds)->refs--;
        task->fds = NULL;
        task->fds_capacity = 0;
    }
    spin_unlock(&task_fd_array_lock);
}

void task_fd_free_table(task_t *task) {
    task_fd_t *fds;

    if (!task) return;
    spin_lock(&task_fd_array_lock);
    fds = task->fds;
    task->fds = NULL;
    task->fds_capacity = 0;
    spin_unlock(&task_fd_array_lock);
    if (fds) fd_table_free(fds);
}

int task_fd_alloc(task_t *task, int start) {
    int i;

    if (!task || start < 0) return -1;
    for (;;) {
        spin_lock(&task_fd_array_lock);
        for (i = start; task->fds && i < task->fds_capacity; i++) {
            if (task->fds[i].in_use) continue;
            memset(&task->fds[i], 0, sizeof(task_fd_t));
            task->fds[i].in_use = 1;
            task->fds[i].ref_count = 1;
            spin_unlock(&task_fd_array_lock);
            return i;
        }
        i = task->fds_capacity;
        spin_unlock(&task_fd_array_lock);
        if (task_fd_ensure_capacity(task, start > i ? start : i) != 0)
            return -1;
    }
}

int task_fd_ensure_capacity(task_t *task, int min_fd) {
    task_fd_t *new_fds;
    task_fd_t *old_fds;
    unsigned long limit;
    int old_cap;
    int new_cap;

    if (!task || !task->fds) return -1;
    if (min_fd < 0) return -1;
    limit = task_rlimit_get(task, 7, 0);
    if (limit > 0x7FFFFFFFUL) limit = 0x7FFFFFFFUL;
    if ((unsigned long)min_fd >= limit) return -1;
    for (;;) {
        old_fds = task->fds;
        old_cap = task->fds_capacity;
        if (!old_fds) return -1;
        if (min_fd < old_cap) return 0;
        new_cap = old_cap;
        if (new_cap < TASK_INIT_FDS) new_cap = TASK_INIT_FDS;
        while (new_cap <= min_fd) {
            if ((unsigned long)new_cap >= limit) return -1;
            if (new_cap > 0x3FFFFFFF)
                new_cap = min_fd + 1;
            else
                new_cap *= 2;
            if ((unsigned long)new_cap > limit) new_cap = (int)limit;
        }
        new_fds = fd_table_alloc(new_cap);
        if (!new_fds) return -1;
        spin_lock(&task_fd_array_lock);
        if (task->fds == old_fds && task->fds_capacity == old_cap) break;
        spin_unlock(&task_fd_array_lock);
        fd_table_free(new_fds);
    }
    memcpy(new_fds, old_fds, (uint64_t)old_cap * sizeof(task_fd_t));
    fd_head(new_fds)->refs = fd_head(old_fds)->refs;
    fd_table_replace_locked(task, old_fds, new_fds, new_cap);
    if (fd_head(old_fds)->refs > 1) {
        fd_head(new_fds)->retired = fd_head(old_fds);
        old_fds = NULL;
    }
    spin_unlock(&task_fd_array_lock);
    if (old_fds) fd_table_free(old_fds);
    return 0;
}

void task_fd_reclaim_unused(task_t *task) {
    task_fd_t *new_fds;
    task_fd_t *old_fds;
    int new_cap;
    int rounded_cap;
    int i;

    if (!task || !task->fds) return;
    if (fd_head(task->fds)->refs > 1) return;
    if (task->fds_capacity <= TASK_INIT_FDS * 4) return;
    new_cap = TASK_INIT_FDS;
    for (i = task->fds_capacity - 1; i >= TASK_INIT_FDS; i--) {
        if (task->fds[i].in_use) {
            new_cap = i + 1;
            break;
        }
    }
    if (new_cap >= task->fds_capacity) return;
    if (new_cap > task->fds_capacity / 2) return;
    rounded_cap = TASK_INIT_FDS;
    while (rounded_cap < new_cap) {
        rounded_cap *= 2;
    }
    new_cap = rounded_cap;
    if (new_cap >= task->fds_capacity) return;
    new_fds = fd_table_alloc(new_cap);
    if (!new_fds) return;
    spin_lock(&task_fd_array_lock);
    old_fds = task->fds;
    if (fd_head(old_fds)->refs > 1) {
        spin_unlock(&task_fd_array_lock);
        fd_table_free(new_fds);
        return;
    }
    memcpy(new_fds, old_fds, (uint64_t)new_cap * sizeof(task_fd_t));
    task->fds = new_fds;
    task->fds_capacity = new_cap;
    spin_unlock(&task_fd_array_lock);
    fd_table_free(old_fds);
}

void task_fd_free(task_t *task, int fd) {
    spin_lock(&task_fd_array_lock);
    if (task && task->fds && fd >= 0 && fd < task->fds_capacity)
        memset(&task->fds[fd], 0, sizeof(task_fd_t));
    spin_unlock(&task_fd_array_lock);
    task_fd_reclaim_unused(task);
}

task_fd_t *task_fd_get(task_t *task, int fd) {
    if (!task || !task->fds || fd < 0 || fd >= task->fds_capacity) return NULL;
    if (!task->fds[fd].in_use) return NULL;
    return &task->fds[fd];
}

static int fd_pos_find(int id) {
    int low;
    int high;
    int mid;

    low = 0;
    high = fd_pos_count;
    while (low < high) {
        mid = (low + high) / 2;
        if (fd_pos[mid].id < id) low = mid + 1;
        else high = mid;
    }
    return low;
}

static fd_pos_t *fd_pos_lookup(int id) {
    int i;

    i = fd_pos_find(id);
    return i < fd_pos_count && fd_pos[i].id == id ? &fd_pos[i] : NULL;
}

static void fd_pos_sweep_locked(void) {
    task_t *t;
    task_fd_t *fds;
    fd_pos_t *entry;
    int count;
    int i;

    spin_lock(&task_fd_array_lock);
    lock_scheduler();
    for (t = all_tasks_head; t && task_ptr_valid(t); t = t->all_next) {
        fds = t->fds;
        for (i = 0; fds && i < t->fds_capacity; i++) {
            if (!fds[i].in_use || fds[i].type != FD_TYPE_FILE ||
                fds[i].ref_count <= 1) continue;
            entry = fd_pos_lookup(fds[i].ref_count);
            if (entry) entry->offset |= FD_POS_MARK;
        }
    }
    unlock_scheduler();
    spin_unlock(&task_fd_array_lock);
    for (i = count = 0; i < fd_pos_count; i++) {
        if (!(fd_pos[i].offset & FD_POS_MARK)) continue;
        fd_pos[count].id = fd_pos[i].id;
        fd_pos[count++].offset = fd_pos[i].offset & ~FD_POS_MARK;
    }
    fd_pos_count = count;
    fd_pos_live = count > 4 ? count : 4;
}

static int fd_pos_group(task_fd_t *source) {
    fd_pos_t *grown;
    fd_pos_t *old;
    int capacity;
    int id;
    int i;

    for (;;) {
        spin_lock(&fd_pos_lock);
        if (source->ref_count > 1) {
            id = source->ref_count;
            spin_unlock(&fd_pos_lock);
            return id;
        }
        if (fd_pos_count >= fd_pos_capacity &&
            fd_pos_count >= fd_pos_live * 2)
            fd_pos_sweep_locked();
        if (fd_pos_count < fd_pos_capacity) break;
        capacity = fd_pos_capacity;
        spin_unlock(&fd_pos_lock);
        grown = (fd_pos_t *)kmalloc((uint64_t)(capacity ? capacity * 2 : 4) *
                                    sizeof(fd_pos_t));
        if (!grown) return 1;
        spin_lock(&fd_pos_lock);
        if (fd_pos_capacity == capacity) {
            if (fd_pos_count)
                memcpy(grown, fd_pos, (uint64_t)fd_pos_count * sizeof(fd_pos_t));
            old = fd_pos;
            fd_pos = grown;
            grown = old;
            fd_pos_capacity = capacity ? capacity * 2 : 4;
        }
        spin_unlock(&fd_pos_lock);
        if (grown) kfree(grown);
    }
    do {
        fd_pos_seq = fd_pos_seq < 0x7FFFFFFF ? fd_pos_seq + 1 : 2;
    } while (fd_pos_lookup(fd_pos_seq));
    i = fd_pos_find(fd_pos_seq);
    memmove(&fd_pos[i + 1], &fd_pos[i],
            (uint64_t)(fd_pos_count - i) * sizeof(fd_pos_t));
    fd_pos[i].id = fd_pos_seq;
    fd_pos[i].offset = source->offset;
    fd_pos_count++;
    source->ref_count = fd_pos_seq;
    id = fd_pos_seq;
    spin_unlock(&fd_pos_lock);
    return id;
}

static uint64_t fd_position_raw(task_fd_t *fd) {
    fd_pos_t *entry;
    uint64_t value;

    if (!fd) return 0;
    value = fd->offset;
    if (fd->type != FD_TYPE_FILE || fd->ref_count <= 1) return value;
    spin_lock(&fd_pos_lock);
    entry = fd_pos_lookup(fd->ref_count);
    if (entry) value = entry->offset;
    spin_unlock(&fd_pos_lock);
    return value;
}

void task_fd_position_share(task_fd_t *source, task_fd_t *copy) {
    if (!source || !copy) return;
    if (source->type != FD_TYPE_FILE || copy->type != FD_TYPE_FILE) return;
    copy->ref_count = fd_pos_group(source);
    copy->offset = fd_position_raw(source);
}

uint64_t task_fd_position_get(task_fd_t *fd) {
    return fd_position_raw(fd) & ~TASK_FD_POSITION_EOF;
}

void task_fd_position_set(task_fd_t *fd, uint64_t value) {
    fd_pos_t *entry;

    if (!fd) return;
    fd->offset = value;
    if (fd->type != FD_TYPE_FILE || fd->ref_count <= 1) return;
    spin_lock(&fd_pos_lock);
    entry = fd_pos_lookup(fd->ref_count);
    if (entry) entry->offset = value;
    spin_unlock(&fd_pos_lock);
}

void task_fd_position_add(task_fd_t *fd, uint64_t value) {
    task_fd_position_set(fd, task_fd_position_get(fd) + value);
}

int task_fd_position_is_eof(task_fd_t *fd) {
    return (fd_position_raw(fd) & TASK_FD_POSITION_EOF) != 0;
}

void task_fd_position_mark_eof(task_fd_t *fd) {
    if (!fd) return;
    task_fd_position_set(fd, task_fd_position_get(fd) |
                         TASK_FD_POSITION_EOF);
}

void task_fd_close_all(task_t *task) {
    int i;
    int endpoint;
    task_fd_t *tfd;
    pipe_t *p;
    
    if (!task || !task->fds) return;
    for (i = 0; i < task->fds_capacity; i++) {
        tfd = &task->fds[i];
        if (!tfd->in_use) continue;
        if (FD_TYPE_IS_PIPE(tfd->type)) {
            p = (pipe_t *)tfd->private_data;
            if (p) {
                if (pipe_release_reference(p, tfd->type)) {
                    pipe_destroy_if_unused(p);
                }
            }
        } else if (FD_TYPE_IS_PTY(tfd->type)) {
            endpoint = (int)(uintptr_t)tfd->private_data;
            if (tfd->type == FD_TYPE_PTY_MASTER)
                pty_close_master(endpoint);
            else
                pty_close_slave(endpoint);
        } else if (tfd->type == FD_TYPE_FILE && tfd->node) {
            vfs_close((vfs_node_t *)tfd->node);
        }
        tfd->in_use = 0;
        tfd->ref_count = 0;
        tfd->node = NULL;
        tfd->offset = 0;
        tfd->flags = 0;
        tfd->private_data = NULL;
    }
}

void task_fd_close_cloexec(task_t *task) {
    int i;
    int j;
    int release_flock;
    task_fd_t *tfd;
    pipe_t *p;
    int endpoint;

    if (!task) return;
    socket_close_cloexec(task);
    event_descriptors_close_cloexec(task->pid);
    if (!task->fds) return;
    for (i = 3; i < task->fds_capacity; i++) {
        tfd = &task->fds[i];
        if (!tfd->in_use) continue;
        if (!(tfd->flags & VFS_O_CLOEXEC)) continue;
        if (FD_TYPE_IS_PIPE(tfd->type)) {
            p = (pipe_t *)tfd->private_data;
            if (p) {
                if (pipe_release_reference(p, tfd->type)) {
                    pipe_destroy_if_unused(p);
                }
            }
        } else if (FD_TYPE_IS_PTY(tfd->type)) {
            endpoint = (int)(uintptr_t)tfd->private_data;
            if (tfd->type == FD_TYPE_PTY_MASTER)
                pty_close_master(endpoint);
            else
                pty_close_slave(endpoint);
        } else if (tfd->type == FD_TYPE_FILE && tfd->node) {
            release_flock = 1;
            if (tfd->ref_count > 1) {
                for (j = 0; j < task->fds_capacity; j++) {
                    if (j == i || !task->fds[j].in_use) continue;
                    if (task->fds[j].type == FD_TYPE_FILE &&
                        task->fds[j].node == tfd->node &&
                        task->fds[j].ref_count == tfd->ref_count &&
                        !(task->fds[j].flags & VFS_O_CLOEXEC)) {
                        release_flock = 0;
                        break;
                    }
                }
            }
            file_locks_release_process_node(task->pid,
                                            (vfs_node_t *)tfd->node,
                                            release_flock);
            vfs_close((vfs_node_t *)tfd->node);
        }
        tfd->in_use = 0;
        tfd->ref_count = 0;
        tfd->node = NULL;
        tfd->offset = 0;
        tfd->flags = 0;
        tfd->private_data = NULL;
    }
    task_fd_reclaim_unused(task);
}
