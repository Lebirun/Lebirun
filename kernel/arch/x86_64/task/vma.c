#include "task_priv.h"

void task_clear_file_mappings(task_t *t) {
    uint8_t *buffer;
    uint64_t position;
    uint64_t end;
    uint64_t chunk;
    uint64_t file_position;
    uint64_t written;
    int i;

    if (!t) return;
    if (!t->vm->file_maps) {
        t->vm->file_map_count = 0;
        t->vm->file_map_capacity = 0;
        return;
    }
    buffer = NULL;
    if (t->pml4_phys) buffer = (uint8_t *)slab_page_alloc(PAGE_SIZE);
    for (i = 0; i < t->vm->file_map_count; i++) {
        if (buffer && t->vm->file_maps[i].node &&
            (t->vm->file_maps[i].map_flags & TASK_VMA_SHARED) &&
            (t->vm->file_maps[i].map_flags & TASK_VMA_FILE)) {
            position = t->vm->file_maps[i].vaddr;
            end = position + t->vm->file_maps[i].filesz;
            while (position < end) {
                chunk = end - position;
                if (chunk > PAGE_SIZE) chunk = PAGE_SIZE;
                if (!vmm_get_phys_in_pml4(t->pml4_phys, position)) {
                    position += chunk;
                    continue;
                }
                vmm_read_from_pml4(t->pml4_phys, position, buffer, chunk);
                file_position = t->vm->file_maps[i].offset + position -
                                t->vm->file_maps[i].vaddr;
                written = vfs_write(t->vm->file_maps[i].node, file_position,
                                    chunk, buffer);
                if (written != chunk) break;
                position += chunk;
            }
            vfs_sync_node(t->vm->file_maps[i].node, 0);
        }
        if (t->vm->file_maps[i].node) {
            vfs_close(t->vm->file_maps[i].node);
            t->vm->file_maps[i].node = NULL;
        }
    }
    if (buffer) slab_page_free(buffer, PAGE_SIZE);
    kfree(t->vm->file_maps);
    t->vm->file_maps = NULL;
    t->vm->file_map_count = 0;
    t->vm->file_map_capacity = 0;
}

void task_release_file_mappings(task_file_map_t *maps, int count) {
    int i;

    if (!maps) return;
    for (i = 0; i < count; i++) {
        if (maps[i].node) {
            vfs_close(maps[i].node);
        }
    }
    kfree(maps);
}

void task_exec_abort_vm(task_t *t, task_vm_t *old_vm) {
    task_clear_file_mappings(t);
    kfree(t->vm);
    t->vm = old_vm;
}

int task_track_user_page(task_t *task, uint64_t phys) {
    if (!task || !phys) return -1;
    task->vm->user_pages_count++;
    if (task->maxrss_kb < task->vm->user_pages_count * 4ULL)
        task->maxrss_kb = task->vm->user_pages_count * 4ULL;
    return 0;
}

static int task_ensure_file_map_list_capacity(task_file_map_list_t *list,
                                              int needed) {
    task_file_map_t *new_maps;

    if (!list || needed < 0) return -1;
    if (needed <= list->capacity) return 0;
    new_maps = (task_file_map_t *)krealloc(
        list->maps, needed * sizeof(task_file_map_t));
    if (!new_maps) return -1;
    list->maps = new_maps;
    list->capacity = needed;
    return 0;
}

int task_ensure_file_map_capacity(task_t *task, int needed) {
    task_file_map_list_t list;
    int result;

    if (!task) return -1;
    list.maps = task->vm->file_maps;
    list.count = task->vm->file_map_count;
    list.capacity = task->vm->file_map_capacity;
    result = task_ensure_file_map_list_capacity(&list, needed);
    task->vm->file_maps = list.maps;
    task->vm->file_map_count = list.count;
    task->vm->file_map_capacity = list.capacity;
    return result;
}

int task_reserve_vm_areas(task_t *task, int additional) {
    if (!task || additional < 0 ||
        task->vm->file_map_count > INT32_MAX - additional) return -1;
    return task_ensure_file_map_capacity(task,
                                          task->vm->file_map_count + additional);
}

void task_file_map_list_init(task_file_map_list_t *list) {
    if (!list) return;
    list->maps = NULL;
    list->count = 0;
    list->capacity = 0;
}

int task_file_map_list_add(task_file_map_list_t *list, vfs_node_t *node,
                           uint64_t vaddr, uint64_t memsz, uint64_t filesz,
                           uint64_t offset, uint64_t flags) {
    uint64_t start;
    uint64_t end;
    uint64_t delta;
    int idx;

    if (!list || !node || memsz == 0) return -1;
    start = vaddr & ~(PAGE_SIZE - 1);
    delta = vaddr - start;
    if (offset < delta) return -1;
    end = (vaddr + memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (end < vaddr) return -1;
    if (filesz > memsz || filesz + delta < filesz) return -1;
    if (task_ensure_file_map_list_capacity(list, list->count + 1) != 0)
        return -1;
    idx = list->count;
    vfs_open(node, 0);
    list->maps[idx].node = node;
    list->maps[idx].vaddr = start;
    list->maps[idx].memsz = end - start;
    list->maps[idx].filesz = filesz + delta;
    list->maps[idx].offset = offset - delta;
    list->maps[idx].flags = flags;
    list->maps[idx].map_flags = TASK_VMA_FILE | TASK_VMA_PRIVATE;
    if (!(flags & VMM_PTE_WRITE)) list->maps[idx].map_flags |= TASK_VMA_CLEAN;
    list->maps[idx].map_flags |= TASK_VMA_MAYWRITE;
    list->count++;
    return 0;
}

void task_file_map_list_release(task_file_map_list_t *list) {
    if (!list) return;
    task_release_file_mappings(list->maps, list->count);
    task_file_map_list_init(list);
}

void task_file_map_list_adopt(task_t *task, task_file_map_list_t *list) {
    if (!task || !list) return;
    task->vm->file_maps = list->maps;
    task->vm->file_map_count = list->count;
    task->vm->file_map_capacity = list->capacity;
    task_file_map_list_init(list);
}

int task_add_vm_area(task_t *task, vfs_node_t *node, uint64_t vaddr,
                     uint64_t size, uint64_t file_size, uint64_t offset,
                     uint64_t flags, uint32_t map_flags) {
    task_file_map_t *area;
    uint64_t end;

    if (!task || size == 0 || (vaddr & (PAGE_SIZE - 1)) != 0) return -1;
    end = vaddr + size;
    if (end < vaddr || (size & (PAGE_SIZE - 1)) != 0) return -1;
    if (task_ensure_file_map_capacity(task, task->vm->file_map_count + 1) != 0)
        return -1;
    area = &task->vm->file_maps[task->vm->file_map_count];
    memset(area, 0, sizeof(*area));
    area->node = node;
    area->vaddr = vaddr;
    area->memsz = size;
    area->filesz = file_size > size ? size : file_size;
    area->offset = offset;
    area->flags = flags;
    area->map_flags = map_flags;
    if (node && !(flags & VMM_PTE_WRITE))
        area->map_flags |= TASK_VMA_CLEAN;
    if (node) vfs_open(node, 0);
    task->vm->file_map_count++;
    return 0;
}

int task_unmap_vm_areas(task_t *task, uint64_t vaddr, uint64_t size) {
    task_file_map_t original;
    task_file_map_t *area;
    task_file_map_t *right;
    uint64_t end;
    uint64_t area_end;
    uint64_t file_end;
    int i;

    if (!task || size == 0) return -1;
    end = vaddr + size;
    if (end < vaddr) return -1;
    i = 0;
    while (i < task->vm->file_map_count) {
        area = &task->vm->file_maps[i];
        area_end = area->vaddr + area->memsz;
        if (end <= area->vaddr || vaddr >= area_end) {
            i++;
            continue;
        }
        if (vaddr <= area->vaddr && end >= area_end) {
            if (area->node) vfs_close(area->node);
            if (i + 1 < task->vm->file_map_count) {
                memmove(area, area + 1,
                        (task->vm->file_map_count - i - 1) * sizeof(*area));
            }
            task->vm->file_map_count--;
            continue;
        }
        original = *area;
        file_end = original.vaddr + original.filesz;
        if (vaddr <= original.vaddr) {
            area->vaddr = end;
            area->memsz = area_end - end;
            area->offset = original.offset + end - original.vaddr;
            area->filesz = file_end > end ? file_end - end : 0;
            i++;
            continue;
        }
        if (end >= area_end) {
            area->memsz = vaddr - original.vaddr;
            if (area->filesz > area->memsz) area->filesz = area->memsz;
            i++;
            continue;
        }
        if (task_ensure_file_map_capacity(task,
                                          task->vm->file_map_count + 1) != 0)
            return -1;
        area = &task->vm->file_maps[i];
        area->memsz = vaddr - original.vaddr;
        if (area->filesz > area->memsz) area->filesz = area->memsz;
        right = &task->vm->file_maps[task->vm->file_map_count];
        *right = original;
        right->vaddr = end;
        right->memsz = area_end - end;
        right->offset = original.offset + end - original.vaddr;
        right->filesz = file_end > end ? file_end - end : 0;
        if (right->node) vfs_open(right->node, 0);
        task->vm->file_map_count++;
        i++;
    }
    return 0;
}

static int task_split_vm_area(task_t *task, int index, uint64_t address) {
    task_file_map_t original;
    task_file_map_t *left;
    task_file_map_t *right;
    uint64_t original_end;
    uint64_t file_end;

    if (!task || index < 0 || index >= task->vm->file_map_count) return -1;
    original = task->vm->file_maps[index];
    original_end = original.vaddr + original.memsz;
    if (address <= original.vaddr || address >= original_end) return 0;
    if (task_ensure_file_map_capacity(task, task->vm->file_map_count + 1) != 0)
        return -1;
    left = &task->vm->file_maps[index];
    right = &task->vm->file_maps[task->vm->file_map_count];
    file_end = original.vaddr + original.filesz;
    *left = original;
    left->memsz = address - original.vaddr;
    if (left->filesz > left->memsz) left->filesz = left->memsz;
    *right = original;
    right->vaddr = address;
    right->memsz = original_end - address;
    right->offset = original.offset + address - original.vaddr;
    right->filesz = file_end > address ? file_end - address : 0;
    if (right->node) vfs_open(right->node, 0);
    task->vm->file_map_count++;
    return 0;
}

int task_isolate_vm_area(task_t *task, uint64_t address, uint64_t size) {
    uint64_t end;
    uint64_t area_end;
    int i;

    if (!task || !size || address + size < address) return -1;
    end = address + size;
    for (i = 0; i < task->vm->file_map_count; i++) {
        area_end = task->vm->file_maps[i].vaddr + task->vm->file_maps[i].memsz;
        if (address < task->vm->file_maps[i].vaddr || end > area_end) continue;
        if (address > task->vm->file_maps[i].vaddr) {
            if (task_split_vm_area(task, i, address) < 0) return -1;
            i = task->vm->file_map_count - 1;
        }
        if (task_split_vm_area(task, i, end) < 0) return -1;
        return i;
    }
    return -1;
}

int task_advise_vm_areas(task_t *task, uint64_t vaddr, uint64_t size,
                         uint32_t set, uint32_t clear) {
    uint64_t end;
    uint64_t area_end;
    int i;

    if (!task || size == 0 || vaddr + size < vaddr) return -1;
    if (task_reserve_vm_areas(task, 2) != 0) return -1;
    end = vaddr + size;
    for (i = 0; i < task->vm->file_map_count; i++) {
        area_end = task->vm->file_maps[i].vaddr + task->vm->file_maps[i].memsz;
        if (end <= task->vm->file_maps[i].vaddr || vaddr >= area_end) continue;
        if (vaddr > task->vm->file_maps[i].vaddr) {
            if (task_split_vm_area(task, i, vaddr) != 0) return -1;
            continue;
        }
        if (end < area_end && task_split_vm_area(task, i, end) != 0)
            return -1;
        task->vm->file_maps[i].map_flags =
            (task->vm->file_maps[i].map_flags & ~clear) | set;
    }
    return 0;
}

int task_protect_vm_areas(task_t *task, uint64_t vaddr, uint64_t size,
                          uint64_t flags) {
    uint64_t end;
    uint64_t area_end;
    int i;

    if (!task || size == 0) return -1;
    end = vaddr + size;
    if (end < vaddr) return -1;
    for (i = 0; i < task->vm->file_map_count; i++) {
        area_end = task->vm->file_maps[i].vaddr + task->vm->file_maps[i].memsz;
        if (end <= task->vm->file_maps[i].vaddr || vaddr >= area_end) continue;
        if (vaddr > task->vm->file_maps[i].vaddr && vaddr < area_end) {
            if (task_split_vm_area(task, i, vaddr) != 0) return -1;
            continue;
        }
        area_end = task->vm->file_maps[i].vaddr + task->vm->file_maps[i].memsz;
        if (end > task->vm->file_maps[i].vaddr && end < area_end) {
            if (task_split_vm_area(task, i, end) != 0) return -1;
        }
        if (flags & VMM_PTE_WRITE)
            task->vm->file_maps[i].map_flags &= ~TASK_VMA_CLEAN;
        task->vm->file_maps[i].flags = flags |
            (task->vm->file_maps[i].flags & (VMM_PTE_SHARED | VMM_PTE_NOFREE));
    }
    return 0;
}
