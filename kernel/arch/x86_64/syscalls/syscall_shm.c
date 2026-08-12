#include "syscall_defs.h"

extern task_t *current_task;

#define SHM_INIT_COUNT 1
#define SHM_RDONLY 010000
#define SHM_RND 020000
#define IPC_CREAT 01000
#define IPC_EXCL 02000
#define IPC_RMID 0

typedef struct {
    int in_use;
    int removed;
    int key;
    uint64_t size;
    uint64_t *pages;
    uint64_t page_count;
    int nattach;
    uint32_t mode;
    uint64_t uid;
    uint64_t gid;
} shm_seg_t;

typedef struct shm_attachment {
    struct shm_attachment *next;
    pid_t pid;
    int shmid;
    uint64_t address;
    uint64_t size;
} shm_attachment_t;

static shm_seg_t *shm_segs;
static int shm_capacity;
static shm_attachment_t *shm_attachments;
static mutex_t shm_lock;

static int shm_grow(void) {
    int new_capacity;
    int i;
    shm_seg_t *new_segments;

    new_capacity = shm_capacity ? shm_capacity * 2 : SHM_INIT_COUNT;
    new_segments = (shm_seg_t *)krealloc(
        shm_segs, (size_t)new_capacity * sizeof(shm_seg_t));
    if (!new_segments) return -1;
    for (i = shm_capacity; i < new_capacity; i++) {
        memset(&new_segments[i], 0, sizeof(shm_seg_t));
    }
    shm_segs = new_segments;
    shm_capacity = new_capacity;
    return 0;
}

static int shm_permission(shm_seg_t *segment, int write_access) {
    uint32_t bits;

    if (!segment || !current_task) return 0;
    if (current_task->euid == 0) return 1;
    if (current_task->euid == segment->uid) bits = segment->mode >> 6;
    else if (current_task->egid == segment->gid) bits = segment->mode >> 3;
    else bits = segment->mode;
    return write_access ? ((bits & 2) != 0) : ((bits & 4) != 0);
}

static void shm_free_segment(shm_seg_t *segment) {
    uint64_t i;

    if (!segment || !segment->in_use || segment->nattach != 0) return;
    for (i = 0; i < segment->page_count; i++) {
        if (segment->pages[i]) pfa_free(segment->pages[i]);
    }
    if (segment->pages) kfree(segment->pages);
    memset(segment, 0, sizeof(*segment));
}

static int shm_allocate_pages(shm_seg_t *segment, uint64_t size) {
    uint64_t i;

    segment->page_count = size / PAGE_SIZE;
    segment->pages = (uint64_t *)kmalloc(
        segment->page_count * sizeof(uint64_t));
    if (!segment->pages) return -1;
    memset(segment->pages, 0, segment->page_count * sizeof(uint64_t));
    for (i = 0; i < segment->page_count; i++) {
        segment->pages[i] = pfa_alloc();
        if (!segment->pages[i]) {
            while (i > 0) {
                i--;
                pfa_free(segment->pages[i]);
            }
            kfree(segment->pages);
            segment->pages = NULL;
            segment->page_count = 0;
            return -1;
        }
        pmm_zero_page_phys(segment->pages[i]);
    }
    return 0;
}

static int shm_range_free(uint64_t address, uint64_t size) {
    uint64_t offset;
    uint64_t end;
    uint64_t area_end;
    int i;

    if (!current_task || address + size < address)
        return 0;
    if (!((address >= USER_DYNAMIC_BASE &&
           address + size <= USER_DYNAMIC_LIMIT) ||
          (address >= USER_HIGH_DYNAMIC_BASE &&
           address + size <= USER_HIGH_DYNAMIC_LIMIT))) return 0;
    end = address + size;
    if (address < current_task->user_brk &&
        end > current_task->user_brk_start) return 0;
    for (i = 0; i < current_task->file_map_count; i++) {
        area_end = current_task->file_maps[i].vaddr +
                   current_task->file_maps[i].memsz;
        if (area_end < current_task->file_maps[i].vaddr) return 0;
        if (address < area_end && end > current_task->file_maps[i].vaddr)
            return 0;
    }
    for (offset = 0; offset < size; offset += PAGE_SIZE) {
        if (vmm_get_phys_in_pml4(current_task->pml4_phys,
                                 address + offset) != 0) return 0;
    }
    return 1;
}

static uint64_t shm_find_address(uint64_t requested, uint64_t size) {
    uint64_t address;

    if (requested != 0) {
        address = requested & ~(PAGE_SIZE - 1);
        return shm_range_free(address, size) ? address : 0;
    }
    address = current_task->mmap_next_addr;
    if (address >= USER_HIGH_DYNAMIC_BASE &&
        address <= USER_HIGH_DYNAMIC_LIMIT) {
        address &= ~(PAGE_SIZE - 1);
        while (address >= USER_HIGH_DYNAMIC_BASE + size) {
            address -= size;
            if (shm_range_free(address, size)) return address;
            address += size - PAGE_SIZE;
        }
        return 0;
    }
    if (address <= USER_DYNAMIC_BASE || address > USER_DYNAMIC_LIMIT)
        address = USER_DYNAMIC_LIMIT;
    address &= ~(PAGE_SIZE - 1);
    while (address >= USER_DYNAMIC_BASE + size) {
        address -= size;
        if (shm_range_free(address, size)) return address;
        address += size - PAGE_SIZE;
    }
    address = USER_HIGH_DYNAMIC_LIMIT & ~(PAGE_SIZE - 1);
    while (address >= USER_HIGH_DYNAMIC_BASE + size) {
        address -= size;
        if (shm_range_free(address, size)) return address;
        address += size - PAGE_SIZE;
    }
    return 0;
}

static int shm_map_segment(shm_seg_t *segment, uint64_t address,
                           int readonly) {
    uint64_t flags;
    uint64_t i;

    flags = VMM_PTE_PRESENT | VMM_PTE_USER | VMM_PTE_NX |
            VMM_PTE_NOFREE;
    if (!readonly) flags |= VMM_PTE_WRITE;
    for (i = 0; i < segment->page_count; i++) {
        if (vmm_map_page_in_pml4(current_task->pml4_phys,
                                 address + i * PAGE_SIZE,
                                 segment->pages[i], flags) != 0) {
            while (i > 0) {
                i--;
                vmm_unmap_page_in_pml4(current_task->pml4_phys,
                                       address + i * PAGE_SIZE);
            }
            return -1;
        }
    }
    return 0;
}

static void shm_unmap_attachment(shm_attachment_t *attachment) {
    uint64_t offset;

    if (!attachment || !current_task) return;
    for (offset = 0; offset < attachment->size; offset += PAGE_SIZE) {
        vmm_unmap_page_in_pml4(current_task->pml4_phys,
                               attachment->address + offset);
    }
    vmm_prune_user_range(current_task->pml4_phys, attachment->address,
                         attachment->size);
    task_unmap_vm_areas(current_task, attachment->address, attachment->size);
}

static int sys_shmget(int key, const char *size_ptr, int shmflg) {
    uint64_t size;
    int slot;
    int i;
    shm_seg_t *segment;

    size = (uint64_t)(uintptr_t)size_ptr;
    mutex_lock(&shm_lock);
    if (key != -1) {
        for (i = 0; i < shm_capacity; i++) {
            if (!shm_segs[i].in_use || shm_segs[i].removed ||
                shm_segs[i].key != key) continue;
            if ((shmflg & IPC_CREAT) && (shmflg & IPC_EXCL)) {
                mutex_unlock(&shm_lock);
                return -EEXIST;
            }
            if (size != 0 && size > shm_segs[i].size) {
                mutex_unlock(&shm_lock);
                return -EINVAL;
            }
            mutex_unlock(&shm_lock);
            return i;
        }
    }
    if (!(shmflg & IPC_CREAT) && key != -1) {
        mutex_unlock(&shm_lock);
        return -ENOENT;
    }
    slot = -1;
    for (i = 0; i < shm_capacity; i++) {
        if (!shm_segs[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = shm_capacity;
        if (shm_grow() != 0) {
            mutex_unlock(&shm_lock);
            return -ENOMEM;
        }
    }
    if (size == 0) size = PAGE_SIZE;
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (size == 0) {
        mutex_unlock(&shm_lock);
        return -EINVAL;
    }
    segment = &shm_segs[slot];
    memset(segment, 0, sizeof(*segment));
    if (shm_allocate_pages(segment, size) != 0) {
        mutex_unlock(&shm_lock);
        return -ENOMEM;
    }
    segment->in_use = 1;
    segment->key = key;
    segment->size = size;
    segment->mode = (uint32_t)shmflg & 0777u;
    segment->uid = current_task ? current_task->euid : 0;
    segment->gid = current_task ? current_task->egid : 0;
    mutex_unlock(&shm_lock);
    return slot;
}

static int64_t sys_shmat(int shmid, const char *shmaddr_ptr, int shmflg) {
    uint64_t requested;
    uint64_t address;
    uint64_t flags;
    int readonly;
    shm_seg_t *segment;
    shm_attachment_t *attachment;
    uint64_t offset;

    if (!current_task) return -ESRCH;
    requested = (uint64_t)(uintptr_t)shmaddr_ptr;
    if (requested != 0 && !(shmflg & SHM_RND) &&
        (requested & (PAGE_SIZE - 1)) != 0) return -EINVAL;
    attachment = (shm_attachment_t *)kmalloc(sizeof(*attachment));
    if (!attachment) return -ENOMEM;
    mutex_lock(&shm_lock);
    if (shmid < 0 || shmid >= shm_capacity ||
        !shm_segs[shmid].in_use || shm_segs[shmid].removed) {
        mutex_unlock(&shm_lock);
        kfree(attachment);
        return -EINVAL;
    }
    segment = &shm_segs[shmid];
    readonly = (shmflg & SHM_RDONLY) != 0;
    if (!shm_permission(segment, !readonly)) {
        mutex_unlock(&shm_lock);
        kfree(attachment);
        return -EACCES;
    }
    address = shm_find_address(requested, segment->size);
    if (!address || shm_map_segment(segment, address, readonly) != 0) {
        mutex_unlock(&shm_lock);
        kfree(attachment);
        return -ENOMEM;
    }
    flags = VMM_PTE_PRESENT | VMM_PTE_USER | VMM_PTE_NX |
            VMM_PTE_NOFREE;
    if (!readonly) flags |= VMM_PTE_WRITE;
    if (task_add_vm_area(current_task, NULL, address, segment->size, 0,
                         (uint64_t)shmid, flags,
                         TASK_VMA_SHARED | TASK_VMA_ANONYMOUS) != 0) {
        for (offset = 0; offset < segment->size; offset += PAGE_SIZE) {
            vmm_unmap_page_in_pml4(current_task->pml4_phys,
                                   address + offset);
        }
        mutex_unlock(&shm_lock);
        kfree(attachment);
        return -ENOMEM;
    }
    attachment->pid = current_task->pid;
    attachment->shmid = shmid;
    attachment->address = address;
    attachment->size = segment->size;
    attachment->next = shm_attachments;
    shm_attachments = attachment;
    segment->nattach++;
    current_task->mmap_next_addr = address;
    mutex_unlock(&shm_lock);
    return (int64_t)address;
}

static int sys_shmdt(uint64_t shmaddr, const char *unused1, int unused2) {
    uint64_t address;
    shm_attachment_t **link;
    shm_attachment_t *attachment;
    shm_seg_t *segment;

    (void)unused1;
    (void)unused2;
    if (!current_task) return -ESRCH;
    address = shmaddr;
    mutex_lock(&shm_lock);
    link = &shm_attachments;
    while (*link && ((*link)->pid != current_task->pid ||
                     (*link)->address != address)) {
        link = &(*link)->next;
    }
    if (!*link) {
        mutex_unlock(&shm_lock);
        return -EINVAL;
    }
    attachment = *link;
    *link = attachment->next;
    shm_unmap_attachment(attachment);
    segment = &shm_segs[attachment->shmid];
    if (segment->nattach > 0) segment->nattach--;
    if (segment->removed && segment->nattach == 0) shm_free_segment(segment);
    mutex_unlock(&shm_lock);
    kfree(attachment);
    return 0;
}

static int sys_shmctl(int shmid, const char *cmd_ptr, uint64_t buf) {
    int cmd;
    shm_seg_t *segment;

    (void)buf;
    cmd = (int)(uintptr_t)cmd_ptr;
    cmd &= ~0x100;
    mutex_lock(&shm_lock);
    if (shmid < 0 || shmid >= shm_capacity || !shm_segs[shmid].in_use) {
        mutex_unlock(&shm_lock);
        return -EINVAL;
    }
    segment = &shm_segs[shmid];
    if (!current_task || (current_task->euid != 0 &&
        current_task->euid != segment->uid)) {
        mutex_unlock(&shm_lock);
        return -EPERM;
    }
    if (cmd != IPC_RMID) {
        mutex_unlock(&shm_lock);
        return -EINVAL;
    }
    segment->removed = 1;
    segment->key = -1;
    if (segment->nattach == 0) shm_free_segment(segment);
    mutex_unlock(&shm_lock);
    return 0;
}

static int shm_name_key(uint64_t name_ptr, int *key) {
    const char *name;
    char value;
    uint32_t hash;
    size_t i;

    if (!name_ptr || !key) return -EFAULT;
    name = (const char *)(uintptr_t)name_ptr;
    hash = 0;
    for (i = 0; ; i++) {
        if (copy_from_user(&value, &name[i], 1) != 0) return -EFAULT;
        if (value == '\0') {
            *key = (int)hash;
            return 0;
        }
        hash = hash * 31u + (unsigned char)value;
        if (i == SIZE_MAX) return -ENAMETOOLONG;
    }
}

static int sys_shm_open(uint64_t name_ptr, const char *oflag_ptr, int mode) {
    int key;
    int oflag;
    int flags;
    int result;

    result = shm_name_key(name_ptr, &key);
    if (result != 0) return result;
    oflag = (int)(uintptr_t)oflag_ptr;
    flags = mode & 0777;
    if (oflag & 0x40) flags |= IPC_CREAT;
    if (oflag & 0x80) flags |= IPC_EXCL;
    return sys_shmget(key, (const char *)(uintptr_t)PAGE_SIZE, flags);
}

static int sys_shm_unlink(uint64_t name_ptr, const char *unused1, int unused2) {
    int key;
    int i;
    int result;

    (void)unused1;
    (void)unused2;
    result = shm_name_key(name_ptr, &key);
    if (result != 0) return result;
    mutex_lock(&shm_lock);
    for (i = 0; i < shm_capacity; i++) {
        if (shm_segs[i].in_use && !shm_segs[i].removed &&
            shm_segs[i].key == key) {
            mutex_unlock(&shm_lock);
            return sys_shmctl(i, (const char *)(uintptr_t)IPC_RMID, 0);
        }
    }
    mutex_unlock(&shm_lock);
    return -ENOENT;
}

void shm_close_task(pid_t pid) {
    shm_attachment_t **link;
    shm_attachment_t *attachment;
    shm_seg_t *segment;

    mutex_lock(&shm_lock);
    link = &shm_attachments;
    while (*link) {
        attachment = *link;
        if (attachment->pid != pid) {
            link = &attachment->next;
            continue;
        }
        *link = attachment->next;
        segment = &shm_segs[attachment->shmid];
        if (segment->nattach > 0) segment->nattach--;
        if (segment->removed && segment->nattach == 0)
            shm_free_segment(segment);
        kfree(attachment);
    }
    mutex_unlock(&shm_lock);
}

int shm_fork_task(pid_t parent_pid, pid_t child_pid) {
    shm_attachment_t *attachment;
    shm_attachment_t *copy;
    shm_attachment_t *copies;
    shm_attachment_t *next;

    copies = NULL;
    mutex_lock(&shm_lock);
    attachment = shm_attachments;
    while (attachment) {
        if (attachment->pid == parent_pid) {
            copy = (shm_attachment_t *)kmalloc(sizeof(*copy));
            if (!copy) {
                mutex_unlock(&shm_lock);
                while (copies) {
                    next = copies->next;
                    kfree(copies);
                    copies = next;
                }
                return -ENOMEM;
            }
            *copy = *attachment;
            copy->pid = child_pid;
            copy->next = copies;
            copies = copy;
        }
        attachment = attachment->next;
    }
    copy = copies;
    while (copy) {
        shm_segs[copy->shmid].nattach++;
        copy = copy->next;
    }
    if (copies) {
        copy = copies;
        while (copy->next) copy = copy->next;
        copy->next = shm_attachments;
        shm_attachments = copies;
    }
    mutex_unlock(&shm_lock);
    return 0;
}

void syscalls_shm_init(void) {
    shm_segs = NULL;
    shm_capacity = 0;
    shm_attachments = NULL;
    mutex_init(&shm_lock);
    syscall_table_set(SYSCALL_SHMGET, (void *)(sys_shmget));
    syscall_table_set(SYSCALL_SHMAT, (void *)(sys_shmat));
    syscall_table_set(SYSCALL_SHMDT, (void *)(sys_shmdt));
    syscall_table_set(SYSCALL_SHMCTL, (void *)(sys_shmctl));
    syscall_table_set(SYSCALL_SHM_OPEN, (void *)(sys_shm_open));
    syscall_table_set(SYSCALL_SHM_UNLINK, (void *)(sys_shm_unlink));
}
