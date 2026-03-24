#include <kernel/mem_map.h>
#include <kernel/task.h>
#include <kernel/tty.h>
#include <kernel/debug.h>
#include <kernel/elf.h>
#include "launch_user.h"
#include <kernel/console.h>
#include <kernel/vfs.h>
#include <kernel/rng.h>
#include <stdio.h>
#include <string.h>

#define USER_STACK_TOP 0x00800000u
#define USER_STACK_SIZE 0x4000u
#define USER_STACK_GAP  0x1000u

#define AT_NULL         0
#define AT_PHDR         3
#define AT_PHENT        4
#define AT_PHNUM        5
#define AT_PAGESZ       6
#define AT_ENTRY        9
#define AT_UID          11
#define AT_EUID         12
#define AT_GID          13
#define AT_EGID         14
#define AT_RANDOM       25

static uint64_t align_down_u64(uint64_t v, uint64_t align) {
    if (align == 0) return v;
    return v & ~(align - 1u);
}

static int setup_initial_stack_with_elf(uint64_t pd_phys, const char *argv0, 
                                         const elf_info_t *elf_info,
                                         uint64_t *out_useresp) {
    uint64_t sp;
    uint64_t zero;
    uint64_t argc_val;
    const char *prog_name;
    int prog_len;
    uint64_t prog_addr;
    uint8_t random_bytes[16];
    uint64_t random_addr;

    if (!out_useresp) return -1;

    sp = USER_STACK_TOP - USER_STACK_GAP;
    zero = 0;
    argc_val = 1;

    prog_name = (argv0 && argv0[0]) ? argv0 : "program";
    prog_len = 0;
    while (prog_name[prog_len]) prog_len++;

    sp -= (uint64_t)((prog_len + 1 + 7) & ~7);
    prog_addr = sp;
    vmm_copy_to_pml4(pd_phys, sp, prog_name, (uint64_t)(prog_len + 1));

    rng_fill(random_bytes, sizeof(random_bytes));
    sp -= 16;
    random_addr = sp;
    vmm_copy_to_pml4(pd_phys, sp, random_bytes, 16);

    sp = align_down_u64(sp, 16);

    #define PUSH_AUXV(type, val) do { \
        uint64_t _t = (type), _v = (val); \
        sp -= 8; vmm_copy_to_pml4(pd_phys, sp, &_v, sizeof(uint64_t)); \
        sp -= 8; vmm_copy_to_pml4(pd_phys, sp, &_t, sizeof(uint64_t)); \
    } while(0)

    PUSH_AUXV(AT_NULL, 0);

    PUSH_AUXV(AT_RANDOM, random_addr);

    PUSH_AUXV(AT_EGID, 0);
    PUSH_AUXV(AT_GID, 0);
    PUSH_AUXV(AT_EUID, 0);
    PUSH_AUXV(AT_UID, 0);

    PUSH_AUXV(AT_PAGESZ, 4096);

    if (elf_info) {
        PUSH_AUXV(AT_ENTRY, elf_info->entry_point);
        PUSH_AUXV(AT_PHNUM, elf_info->phnum);
        PUSH_AUXV(AT_PHENT, elf_info->phent);
        PUSH_AUXV(AT_PHDR, elf_info->phdr_vaddr);
    }

    #undef PUSH_AUXV

    sp -= 8;
    vmm_copy_to_pml4(pd_phys, sp, &zero, sizeof(uint64_t));

    sp -= 8;
    vmm_copy_to_pml4(pd_phys, sp, &zero, sizeof(uint64_t));

    sp -= 8;
    vmm_copy_to_pml4(pd_phys, sp, &prog_addr, sizeof(uint64_t));

    sp -= 8;
    vmm_copy_to_pml4(pd_phys, sp, &argc_val, sizeof(uint64_t));

    *out_useresp = sp;
    return 0;
}

static task_t* launch_user_binary_common(const uint8_t *bin_start, const uint8_t *bin_end, int console_id, const char *argv0) {
    if (!bin_start || !bin_end || bin_end <= bin_start) return NULL;
    uint64_t size = (uint64_t)(bin_end - bin_start);

    int elf_valid = elf_validate(bin_start, size);
    if (elf_valid != 0) {
        printf("launch_user_binary: ELF validation failed (%d)\n", elf_valid);
        return NULL;
    }

    uint64_t new_pd = vmm_create_pml4();
    if (!new_pd) {
        printf("launch_user_binary: Failed to create page directory\n");
        return NULL;
    }

    elf_info_t elf_info;
    uint64_t *elf_pages = NULL;
    uint64_t elf_page_count = 0;

    int load_result = elf_load_to_pd(new_pd, bin_start, size, &elf_info, &elf_pages, &elf_page_count);
    if (load_result != 0) {
        printf("launch_user_binary: ELF loading failed (%d)\n", load_result);
        if (elf_pages) {
            for (uint64_t i = 0; i < elf_page_count; i++) pfa_free(elf_pages[i]);
            kfree(elf_pages);
        }
        vmm_free_pml4(new_pd);
        return NULL;
    }

    uint64_t stack_page_count = 0;
    uint64_t *stack_pages = vmm_map_range_in_pml4_tracked(new_pd, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE, 0x7, &stack_page_count);

    uint64_t initial_useresp = USER_STACK_TOP - USER_STACK_GAP - 16u;
    if (setup_initial_stack_with_elf(new_pd, argv0, &elf_info, &initial_useresp) != 0) {
        printf("launch_user_binary: failed to setup initial user stack\n");
        if (elf_pages) {
            for (uint64_t i = 0; i < elf_page_count; i++) pfa_free(elf_pages[i]);
            kfree(elf_pages);
        }
        if (stack_pages) {
            for (uint64_t i = 0; i < stack_page_count; i++) pfa_free(stack_pages[i]);
            kfree(stack_pages);
        }
        vmm_free_pml4(new_pd);
        return NULL;
    }

    task_t* t = create_task_with_cr3((void*)elf_info.entry_point, TASK_BLOCKED, true, new_pd);
    if (!t) {
        printf("launch_user_binary: create_task failed\n");
        if (elf_pages) {
            for (uint64_t i = 0; i < elf_page_count; i++) pfa_free(elf_pages[i]);
            kfree(elf_pages);
        }
        if (stack_pages) {
            for (uint64_t i = 0; i < stack_page_count; i++) pfa_free(stack_pages[i]);
            kfree(stack_pages);
        }
        vmm_free_pml4(new_pd);
        return NULL;
    }

    t->pml4_phys = new_pd;
    t->user_brk = (elf_info.bss_end + 0xFFF) & ~0xFFFu;
    t->console_id = console_id;

    {
        const char *base = argv0;
        int ni = 0;
        if (base) {
            int bi;
            for (bi = 0; argv0[bi]; bi++) {
                if (argv0[bi] == '/') base = &argv0[bi + 1];
            }
            while (ni < 15 && base[ni]) { t->name[ni] = base[ni]; ni++; }
        }
        t->name[ni] = '\0';
    }

    registers_t *frame = (registers_t *)(uintptr_t)t->regs.rsp;
    frame->rsp = initial_useresp;

    uint64_t total_pages = elf_page_count + stack_page_count;

    if (total_pages == 0 || total_pages > 65536) {
        printf("launch_user_binary: suspicious total_pages=%u\n", total_pages);
        if (elf_pages) {
            for (uint64_t i = 0; i < elf_page_count; i++) pfa_free(elf_pages[i]);
            kfree(elf_pages);
        }
        if (stack_pages) {
            for (uint64_t i = 0; i < stack_page_count; i++) pfa_free(stack_pages[i]);
            kfree(stack_pages);
        }
        vmm_free_pml4(new_pd);
        return NULL;
    }

    t->user_pages = (uint64_t *)kmalloc(total_pages * sizeof(uint64_t));
    if (t->user_pages) {
        if (elf_pages) {
            memcpy(t->user_pages, elf_pages, elf_page_count * sizeof(uint64_t));
        }
        if (stack_pages) {
            memcpy(t->user_pages + elf_page_count, stack_pages, stack_page_count * sizeof(uint64_t));
        }
        t->user_pages_count = total_pages;
    } else {
        t->user_pages_count = 0;
    }

    if (elf_pages) kfree(elf_pages);
    if (stack_pages) kfree(stack_pages);

    t->state = TASK_READY;
    lock_scheduler();
    add_task_to_runqueue(t);
    unlock_scheduler();

    return t;
}

task_t* launch_user_binary(const uint8_t *bin_start, const uint8_t *bin_end, int console_id) {
    return launch_user_binary_common(bin_start, bin_end, console_id, "program");
}

task_t* launch_user_path(const char *path, int console_id) {
    vfs_node_t *node;
    uint64_t size;
    uint8_t *buf;
    uint64_t off;
    task_t *t;

    if (!path || path[0] == '\0') return NULL;

    node = vfs_namei(path);
    if (!node) {
        printf("launch_user_path: '%s' not found (vfs_namei returned NULL)\n", path);
        return NULL;
    }

    if (VFS_GET_TYPE(node->flags) != VFS_FILE) {
        printf("launch_user_path: '%s' is not a regular file\n", path);
        return NULL;
    }

    size = node->length;
    if (size == 0 || size > (32u * 1024u * 1024u)) {
        printf("launch_user_path: '%s' invalid size %u\n", path, size);
        return NULL;
    }

    buf = (uint8_t *)kmalloc(size);
    if (!buf) {
        printf("launch_user_path: OOM (%u bytes)\n", size);
        return NULL;
    }

    off = 0;
    while (off < size) {
        uint64_t chunk = size - off;
        if (chunk > 32768) chunk = 32768;
        uint64_t r = vfs_read(node, off, chunk, buf + off);
        if (r == 0) break;
        off += r;
    }

    if (off != size) {
        printf("launch_user_path: short read (%u/%u) for '%s'\n", off, size, path);
        kfree(buf);
        return NULL;
    }

    t = launch_user_binary_common(buf, buf + size, console_id, path);
    kfree(buf);
    return t;
}
