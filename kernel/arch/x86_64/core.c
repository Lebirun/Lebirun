#include "syscalls/syscall_defs.h"
#include <lebirun/debug.h>
#include <lebirun/creds.h>
#include <lebirun/elf.h>
#include <lebirun/kstack.h>
#include <sys/procfs.h>

_Static_assert(sizeof(debug_registers_t) == sizeof(elf_gregset_t), "core register ABI");
_Static_assert(sizeof(elf_fpregset_t) == 512, "core FPU ABI");
_Static_assert(sizeof(struct elf_prstatus) == 336, "core status ABI");
_Static_assert(sizeof(struct elf_prpsinfo) == 136, "core process ABI");

static uint64_t core_table_entry(uint64_t physical, uint64_t index) {
    uint64_t entry;
    uint64_t temporary;

    temporary = TEMP_SLOT(7);
    vmm_temp_map_raw(temporary, physical);
    entry = ((uint64_t *)temporary)[index];
    vmm_temp_unmap_raw(temporary);
    return entry;
}

static int core_next_page(uint64_t pml4, uint64_t *address,
                           uint64_t *flags) {
    uint64_t cursor;
    uint64_t table;
    uint64_t entry;
    uint64_t saved_flags;
    uint64_t step;
    int shift;
    int present;

    cursor = *address;
    while (cursor < 0x0000800000000000ULL) {
        __asm__ volatile ("pushfq; pop %0; cli" : "=r"(saved_flags) :: "memory");
        table = pml4;
        present = 0;
        entry = 0;
        for (shift = 39; shift >= 12; shift -= 9) {
            entry = core_table_entry(table, (cursor >> shift) & 511);
            if ((entry & (VMM_PTE_PRESENT | VMM_PTE_USER)) !=
                (VMM_PTE_PRESENT | VMM_PTE_USER)) break;
            if (shift == 12 || ((shift == 21 || shift == 30) && (entry & 0x80))) {
                present = 1;
                break;
            }
            table = entry & 0x000FFFFFFFFFF000ULL;
        }
        __asm__ volatile ("push %0; popfq" :: "r"(saved_flags) : "memory", "cc");
        if (present && !(entry & (VMM_PTE_PCD | VMM_PTE_PWT))) {
            *address = cursor;
            *flags = entry;
            return 1;
        }
        step = 1ULL << shift;
        cursor = (cursor & ~(step - 1)) + step;
    }
    return 0;
}

static Elf64_Phdr *core_segments(task_t *task, size_t *count) {
    Elf64_Phdr *segments;
    Elf64_Phdr *grown;
    Elf64_Phdr *segment;
    uint64_t address;
    uint64_t flags;
    uint32_t permissions;
    size_t capacity;
    size_t new_capacity;

    segments = NULL;
    capacity = 0;
    *count = 1;
    address = 0;
    while (core_next_page(task->pml4_phys, &address, &flags)) {
        permissions = PF_R;
        if (flags & (VMM_PTE_WRITE | VMM_PTE_COW)) permissions |= PF_W;
        if (!(flags & VMM_PTE_NX)) permissions |= PF_X;
        segment = *count > 1 ? &segments[*count - 1] : NULL;
        if (segment && segment->p_vaddr + segment->p_memsz == address &&
            segment->p_flags == permissions) {
            segment->p_memsz += PAGE_SIZE;
        } else {
            if (*count >= capacity) {
                new_capacity = capacity ? capacity * 2 : 8;
                if (new_capacity < capacity ||
                    new_capacity > SIZE_MAX / sizeof(*segments)) goto fail;
                grown = krealloc(segments, new_capacity * sizeof(*segments));
                if (!grown) goto fail;
                segments = grown;
                capacity = new_capacity;
            }
            segment = &segments[(*count)++];
            memset(segment, 0, sizeof(*segment));
            segment->p_type = PT_LOAD;
            segment->p_vaddr = address;
            segment->p_memsz = PAGE_SIZE;
            segment->p_flags = permissions;
            segment->p_align = PAGE_SIZE;
        }
        address += PAGE_SIZE;
    }
    if (segments) memset(&segments[0], 0, sizeof(*segments));
    return segments;
fail:
    kfree(segments);
    return NULL;
}

static size_t core_note(uint8_t *buffer, uint32_t type,
                         const void *data, uint32_t length) {
    uint32_t header[3];
    size_t padded;

    header[0] = 5;
    header[1] = length;
    header[2] = type;
    memcpy(buffer, header, sizeof(header));
    memcpy(buffer + sizeof(header), "CORE\0\0\0", 8);
    memcpy(buffer + 20, data, length);
    padded = (length + 3ULL) & ~3ULL;
    if (padded > length) memset(buffer + 20 + length, 0, padded - length);
    return 20 + padded;
}

static vfs_node_t *core_create(task_t *task) {
    vfs_node_t *parent;
    vfs_node_t *node;
    char name[32];

    parent = vfs_namei(task->cwd ? task->cwd : "/");
    if (!parent) return NULL;
    if (vfs_check_perm(parent, VFS_PERM_WRITE | VFS_PERM_EXEC) < 0 ||
        (vfs_get_mount_flags_for_node(parent) & VFS_MS_RDONLY)) {
        vfs_release(parent);
        return NULL;
    }
    snprintf(name, sizeof(name), "core.%d", task->pid);
    node = vfs_finddir(parent, name);
    if (node) {
        vfs_release(node);
        vfs_release(parent);
        return NULL;
    }
    if (vfs_create(parent, name, 0600 | VFS_CREATE_EXCL) != 0) {
        vfs_release(parent);
        return NULL;
    }
    node = vfs_finddir(parent, name);
    if (node && VFS_GET_TYPE(node->flags) == VFS_FILE &&
        node->uid == task->euid && (node->mask & 0777) == 0600 &&
        node->length == 0) {
        vfs_open(node, VFS_O_WRONLY);
    } else {
        node = NULL;
    }
    vfs_release(parent);
    return node;
}

static int core_write(task_t *task, registers_t *frame, int signal,
                       uint64_t limit) {
    Elf64_Ehdr header;
    Elf64_Shdr section;
    Elf64_Phdr *segments;
    struct elf_prstatus status;
    struct elf_prpsinfo info;
    debug_registers_t registers;
    uint8_t *notes;
    uint8_t *buffer;
    vfs_node_t *node;
    uint64_t prefix;
    uint64_t position;
    uint64_t length;
    uint64_t offset;
    size_t count;
    size_t note_size;
    size_t i;
    int complete;

    segments = core_segments(task, &count);
    if (!segments) return 0;
    notes = kmalloc(60 + sizeof(status) + sizeof(info) + 512);
    buffer = slab_page_alloc(PAGE_SIZE);
    node = NULL;
    complete = 0;
    if (!notes || !buffer) goto out;
    memset(&status, 0, sizeof(status));
    status.pr_info.si_signo = signal;
    status.pr_cursig = (short)signal;
    status.pr_sigpend = signal_pending_mask(task);
    status.pr_sighold = signal_blocked_mask(task);
    status.pr_pid = task->pid;
    status.pr_ppid = task->ppid;
    status.pr_pgrp = task->pgid;
    status.pr_sid = task->sid;
    task_debug_registers(task, frame, &registers);
    memcpy(status.pr_reg, &registers, sizeof(registers));
    status.pr_fpvalid = task->fpu_state != NULL;
    memset(&info, 0, sizeof(info));
    info.pr_sname = 'R';
    info.pr_uid = (unsigned)task->uid;
    info.pr_gid = (unsigned)task->gid;
    info.pr_pid = task->pid;
    info.pr_ppid = task->ppid;
    info.pr_pgrp = task->pgid;
    info.pr_sid = task->sid;
    memcpy(info.pr_fname, task->name, sizeof(info.pr_fname));
    memcpy(info.pr_psargs, task->name, sizeof(task->name));
    note_size = core_note(notes, 1, &status, sizeof(status));
    note_size += core_note(notes + note_size, 3, &info, sizeof(info));
    if (task->fpu_state) {
        __asm__ volatile ("fxsave64 %0" : "=m"(*(uint8_t (*)[512])task->fpu_state) :: "memory");
        note_size += core_note(notes + note_size, 2, task->fpu_state, 512);
    }
    if (count > (UINT64_MAX - sizeof(header) - sizeof(section)) /
                sizeof(*segments)) goto out;
    prefix = sizeof(header) + count * sizeof(*segments);
    if (count >= 0xFFFF) prefix += sizeof(section);
    if (prefix > limit || note_size > limit - prefix) goto out;
    memset(&header, 0, sizeof(header));
    memcpy(header.e_ident, "\177ELF\2\1\1", 7);
    header.e_type = ET_CORE;
    header.e_machine = EM_X86_64;
    header.e_version = 1;
    header.e_ehsize = sizeof(header);
    header.e_phoff = sizeof(header);
    header.e_phentsize = sizeof(*segments);
    header.e_phnum = count < 0xFFFF ? (uint16_t)count : 0xFFFF;
    memset(&section, 0, sizeof(section));
    if (count >= 0xFFFF) {
        if (count > UINT32_MAX) goto out;
        header.e_shoff = prefix - sizeof(section);
        header.e_shentsize = sizeof(section);
        header.e_shnum = 1;
        section.sh_info = (uint32_t)count;
    }
    segments[0].p_type = PT_NOTE;
    segments[0].p_offset = prefix;
    segments[0].p_filesz = note_size;
    segments[0].p_align = 4;
    position = prefix + note_size;
    for (i = 1; i < count; i++) {
        if (position > UINT64_MAX - PAGE_SIZE + 1) goto out;
        position = (position + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        segments[i].p_offset = position;
        length = position < limit ? limit - position : 0;
        if (length > segments[i].p_memsz) length = segments[i].p_memsz;
        segments[i].p_filesz = length;
        position += length;
    }
    node = core_create(task);
    if (!node) goto out;
    if (vfs_write(node, 0, sizeof(header), (uint8_t *)&header) != sizeof(header) ||
        vfs_write(node, sizeof(header), count * sizeof(*segments),
                   (uint8_t *)segments) != count * sizeof(*segments)) goto out;
    if (header.e_shnum && vfs_write(node, header.e_shoff, sizeof(section),
                                    (uint8_t *)&section) != sizeof(section)) goto out;
    if (vfs_write(node, prefix, note_size, notes) != note_size) goto out;
    for (i = 1; i < count; i++) {
        offset = 0;
        while (offset < segments[i].p_filesz) {
            length = segments[i].p_filesz - offset;
            if (length > PAGE_SIZE) length = PAGE_SIZE;
            vmm_read_from_pml4(task->pml4_phys,
                               segments[i].p_vaddr + offset, buffer, length);
            if (vfs_write(node, segments[i].p_offset + offset,
                           length, buffer) != length) goto out;
            offset += length;
        }
    }
    complete = vfs_sync_node(node, 0) == 0;
out:
    if (node) vfs_close(node);
    if (buffer) slab_page_free(buffer, PAGE_SIZE);
    kfree(notes);
    kfree(segments);
    return complete;
}

int task_core_dump(registers_t *frame, int signal) {
    task_t *task;
    task_t *other;
    uint64_t limit;
    uint64_t file_limit;
    uint64_t saved_flags;
    uint8_t pinned;
    uint8_t stage;
    int result;

    task = current_task;
    if (!task || !frame || (frame->cs & 3) != 3 ||
        (signal != 3 && signal != 4 && signal != 5 && signal != 6 &&
         signal != 7 && signal != 8 && signal != 11 && signal != 24 &&
         signal != 25 && signal != 31)) return 0;
    limit = task_rlimit_get(task, 4, 0);
    file_limit = task_rlimit_get(task, 1, 0);
    if (file_limit < limit) limit = file_limit;
    if (!limit || !creds_get_dumpable(task) || task->uid != task->euid ||
        task->gid != task->egid || task->uid != task->suid ||
        task->gid != task->sgid) return 0;
    lock_scheduler();
    other = all_tasks_head;
    while (other) {
        if (other != task && other->state != TASK_DEAD &&
            other->pml4_phys == task->pml4_phys) break;
        other = other->all_next;
    }
    unlock_scheduler();
    if (other || kstack_expand_syscall() != 0) return 0;
    pinned = task->kernel_cpu_pinned;
    stage = task->kernel_stage;
    task->kernel_cpu_pinned = 1;
    task->kernel_stage = TASK_KERNEL_STAGE_FILE_FAULT;
    __asm__ volatile ("pushfq; pop %0" : "=r"(saved_flags) :: "memory");
    if (frame->rflags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
    result = core_write(task, frame, signal, limit);
    __asm__ volatile ("cli" ::: "memory");
    task->kernel_stage = stage;
    task->kernel_cpu_pinned = pinned;
    __asm__ volatile ("push %0; popfq" :: "r"(saved_flags) : "memory", "cc");
    return result;
}
