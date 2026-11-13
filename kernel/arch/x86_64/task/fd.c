#include "task_priv.h"

spinlock_t task_fd_array_lock = {0};
static int task_fd_position_group_seq = 1;

void task_init_fds(task_t *task) {
    if (!task) return;
    task->fds = (task_fd_t *)kmalloc(TASK_INIT_FDS * sizeof(task_fd_t));
    if (!task->fds) {
        task->fds_capacity = 0;
        return;
    }
    task->fds_capacity = TASK_INIT_FDS;
    memset(task->fds, 0, TASK_INIT_FDS * sizeof(task_fd_t));
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

int task_fd_alloc(task_t *task) {
    int i;
    int ret;

    if (!task || !task->fds) return -1;
    for (i = 3; i < task->fds_capacity; i++) {
        if (!task->fds[i].in_use) {
            memset(&task->fds[i], 0, sizeof(task_fd_t));
            task->fds[i].in_use = 1;
            task->fds[i].ref_count = 1;
            return i;
        }
    }
    ret = task_fd_ensure_capacity(task, task->fds_capacity);
    if (ret != 0) return -1;
    for (i = 3; i < task->fds_capacity; i++) {
        if (!task->fds[i].in_use) {
            memset(&task->fds[i], 0, sizeof(task_fd_t));
            task->fds[i].in_use = 1;
            task->fds[i].ref_count = 1;
            return i;
        }
    }
    return -1;
}

int task_fd_ensure_capacity(task_t *task, int min_fd) {
    task_fd_t *new_fds;
    unsigned long limit;
    int old_cap;
    int new_cap;

    if (!task || !task->fds) return -1;
    if (min_fd < 0) return -1;
    limit = task_rlimit_get(task, 7, 0);
    if (limit > 0x7FFFFFFFUL) limit = 0x7FFFFFFFUL;
    if ((unsigned long)min_fd >= limit) return -1;
    if (min_fd < task->fds_capacity) return 0;
    old_cap = task->fds_capacity;
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
    new_fds = (task_fd_t *)krealloc(task->fds, new_cap * sizeof(task_fd_t));
    if (!new_fds) return -1;
    memset(&new_fds[old_cap], 0, (new_cap - old_cap) * sizeof(task_fd_t));
    spin_lock(&task_fd_array_lock);
    task->fds = new_fds;
    task->fds_capacity = new_cap;
    spin_unlock(&task_fd_array_lock);
    return 0;
}

void task_fd_reclaim_unused(task_t *task) {
    task_fd_t *new_fds;
    int new_cap;
    int rounded_cap;
    int i;

    if (!task || !task->fds) return;
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
    new_fds = (task_fd_t *)kmalloc(new_cap * sizeof(task_fd_t));
    if (!new_fds) return;
    memcpy(new_fds, task->fds, new_cap * sizeof(task_fd_t));
    spin_lock(&task_fd_array_lock);
    kfree(task->fds);
    task->fds = new_fds;
    task->fds_capacity = new_cap;
    spin_unlock(&task_fd_array_lock);
}

void task_fd_free(task_t *task, int fd) {
    if (!task || !task->fds || fd < 0 || fd >= task->fds_capacity) return;
    if (!task->fds[fd].in_use) return;
    memset(&task->fds[fd], 0, sizeof(task_fd_t));
    task_fd_reclaim_unused(task);
}

task_fd_t *task_fd_get(task_t *task, int fd) {
    if (!task || !task->fds || fd < 0 || fd >= task->fds_capacity) return NULL;
    if (!task->fds[fd].in_use) return NULL;
    return &task->fds[fd];
}

void task_fd_position_share(task_fd_t *source, task_fd_t *copy) {
    int group;

    if (!source || !copy) return;
    if (source->type != FD_TYPE_FILE || copy->type != FD_TYPE_FILE) return;
    group = source->ref_count;
    if (group <= 1) {
        spin_lock(&task_fd_array_lock);
        task_fd_position_group_seq++;
        if (task_fd_position_group_seq <= 1) task_fd_position_group_seq = 2;
        group = task_fd_position_group_seq;
        spin_unlock(&task_fd_array_lock);
        source->ref_count = group;
    }
    copy->ref_count = group;
    copy->offset = source->offset;
}

uint64_t task_fd_position_get(task_fd_t *fd) {
    if (!fd) return 0;
    return fd->offset & ~TASK_FD_POSITION_EOF;
}

void task_fd_position_set(task_fd_t *fd, uint64_t value) {
    task_t *task;
    task_fd_t *fds;
    int capacity;
    int group;
    int i;
    int retry;

    if (!fd) return;
    fd->offset = value;
    group = fd->ref_count;
    if (fd->type != FD_TYPE_FILE || group <= 1) return;
    spin_lock(&task_fd_array_lock);
    retry = 1;
    while (retry) {
        retry = 0;
        lock_scheduler();
        task = all_tasks_head;
        while (task && task_ptr_valid(task)) {
            fds = task->fds;
            if (fds) {
                capacity = task->fds_capacity;
                for (i = 0; i < capacity; i++) {
                    if (!fds[i].in_use) continue;
                    if (fds[i].type != FD_TYPE_FILE) continue;
                    if (fds[i].ref_count != group) continue;
                    fds[i].offset = value;
                }
                if (task->fds != fds || task->fds_capacity != capacity) {
                    retry = 1;
                    break;
                }
            }
            task = task->all_next;
        }
        unlock_scheduler();
    }
    spin_unlock(&task_fd_array_lock);
}

void task_fd_position_add(task_fd_t *fd, uint64_t value) {
    task_fd_position_set(fd, task_fd_position_get(fd) + value);
}

int task_fd_position_is_eof(task_fd_t *fd) {
    if (!fd) return 0;
    return (fd->offset & TASK_FD_POSITION_EOF) != 0;
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
