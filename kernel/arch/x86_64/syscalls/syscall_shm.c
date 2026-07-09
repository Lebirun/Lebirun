#include "syscall_defs.h"

extern task_t *current_task;
extern void **syscall_table;

#define SHM_INIT_COUNT 1
#define SHM_RDONLY 010000
#define SHM_RND    020000

typedef struct {
    int in_use;
    int key;
    uint64_t size;
    uint64_t phys_addr;
    int nattach;
    int mode;
    int uid;
    int gid;
} shm_seg_t;

static shm_seg_t *shm_segs = NULL;
static int shm_capacity = 0;
static int shm_initialized = 0;

static int shm_grow(void) {
    int new_cap;
    shm_seg_t *new_arr;
    int i;

    new_cap = shm_capacity ? shm_capacity * 2 : SHM_INIT_COUNT;
    new_arr = (shm_seg_t *)krealloc(shm_segs, new_cap * sizeof(shm_seg_t));
    if (!new_arr) return -1;
    for (i = shm_capacity; i < new_cap; i++) {
        memset(&new_arr[i], 0, sizeof(shm_seg_t));
    }
    shm_segs = new_arr;
    shm_capacity = new_cap;
    return 0;
}

static void init_shm(void) {
    if (shm_initialized) return;
    shm_initialized = 1;
}

static int sys_shmget(int key, const char *size_ptr, int shmflg) {
    init_shm();
    
    uint64_t size = (uint64_t)(uintptr_t)size_ptr;
    
    if (key != -1) {
        for (int i = 0; i < shm_capacity; i++) {
            if (shm_segs[i].in_use && shm_segs[i].key == key) {
                return i;
            }
        }
    }
    
    if (!(shmflg & 0x200)) {
        return -ENOENT;
    }
    
    int slot = -1;
    for (int i = 0; i < shm_capacity; i++) {
        if (!shm_segs[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        int old_cap = shm_capacity;
        if (shm_grow() < 0) return -ENOSPC;
        slot = old_cap;
    }
    
    if (size == 0) size = 4096;
    size = (size + 4095) & ~4095;
    
    uint64_t phys = pfa_alloc_contiguous(size / 4096);
    if (!phys) return -ENOMEM;
    
    shm_segs[slot].in_use = 1;
    shm_segs[slot].key = key;
    shm_segs[slot].size = size;
    shm_segs[slot].phys_addr = phys;
    shm_segs[slot].nattach = 0;
    shm_segs[slot].mode = shmflg & 0777;
    shm_segs[slot].uid = 0;
    shm_segs[slot].gid = 0;
    
    return slot;
}

static int sys_shmat(int shmid, const char *shmaddr_ptr, int shmflg) {
    init_shm();
    
    if (shmid < 0 || shmid >= shm_capacity || !shm_segs[shmid].in_use) {
        return -EINVAL;
    }
    
    uint64_t addr = (uint64_t)(uintptr_t)shmaddr_ptr;
    
    if (addr == 0) {
        addr = 0x40000000 + (shmid * 0x100000);
    }
    
    if (shmflg & SHM_RND) {
        addr &= ~4095;
    }
    
    if (!current_task) return -ESRCH;
    
    uint64_t size = shm_segs[shmid].size;
    uint64_t phys = shm_segs[shmid].phys_addr;
    
    uint64_t pd = current_task->pml4_phys;
    if (!pd) return -EINVAL;
    
    int flags = 0x7;
    if (shmflg & SHM_RDONLY) {
        flags = 0x5;
    }
    
    for (uint64_t off = 0; off < size; off += 4096) {
        vmm_map_page_in_pml4(pd, addr + off, phys + off, flags);
    }
    
    shm_segs[shmid].nattach++;
    
    return (int)addr;
}

static int sys_shmdt(int shmaddr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    init_shm();
    
    uint64_t addr = (uint64_t)shmaddr;
    
    for (int i = 0; i < shm_capacity; i++) {
        if (shm_segs[i].in_use) {
            uint64_t base = 0x40000000 + (i * 0x100000);
            if (addr >= base && addr < base + shm_segs[i].size) {
                shm_segs[i].nattach--;
                return 0;
            }
        }
    }
    
    return -EINVAL;
}

static int sys_shmctl(int shmid, const char *cmd_ptr, int buf) {
    (void)buf;
    init_shm();
    
    int cmd = (int)(uintptr_t)cmd_ptr;
    
    if (shmid < 0 || shmid >= shm_capacity || !shm_segs[shmid].in_use) {
        return -EINVAL;
    }
    
    if (cmd == 0) {
        if (shm_segs[shmid].nattach > 0) {
            return -EBUSY;
        }
        pfa_free_contiguous(shm_segs[shmid].phys_addr, shm_segs[shmid].size / 4096);
        shm_segs[shmid].in_use = 0;
        return 0;
    }
    
    return 0;
}

static int sys_shm_open(int name_ptr, const char *oflag_ptr, int mode) {
    (void)name_ptr; (void)mode;
    int oflag = (int)(uintptr_t)oflag_ptr;
    
    int key = 0;
    if (name_ptr) {
        const char *name = (const char *)(uint64_t)name_ptr;
        while (*name) {
            key = key * 31 + *name++;
        }
    }
    
    int flags = 0;
    if (oflag & 0x40) flags |= 0x200;
    
    return sys_shmget(key, (const char *)(uintptr_t)4096, flags | (mode & 0777));
}

static int sys_shm_unlink(int name_ptr, const char *unused1, int unused2) {
    (void)unused1; (void)unused2;
    
    int key = 0;
    if (name_ptr) {
        const char *name = (const char *)(uint64_t)name_ptr;
        while (*name) {
            key = key * 31 + *name++;
        }
    }
    
    for (int i = 0; i < shm_capacity; i++) {
        if (shm_segs[i].in_use && shm_segs[i].key == key) {
            return sys_shmctl(i, (const char *)0, 0);
        }
    }
    
    return -ENOENT;
}

#define SYSCALL_SHMGET 238
#define SYSCALL_SHMAT 239
#define SYSCALL_SHMDT 240
#define SYSCALL_SHMCTL 241
#define SYSCALL_SHM_OPEN 242
#define SYSCALL_SHM_UNLINK 243

void syscalls_shm_init(void) {
    init_shm();
    syscall_table[SYSCALL_SHMGET] = sys_shmget;
    syscall_table[SYSCALL_SHMAT] = sys_shmat;
    syscall_table[SYSCALL_SHMDT] = sys_shmdt;
    syscall_table[SYSCALL_SHMCTL] = sys_shmctl;
    syscall_table[SYSCALL_SHM_OPEN] = sys_shm_open;
    syscall_table[SYSCALL_SHM_UNLINK] = sys_shm_unlink;
}
