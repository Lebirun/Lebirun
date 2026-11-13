#include "task_priv.h"

static uint64_t task_random_stack_top(void) {
    uint64_t pages;

    pages = rng_get_u32() & 0xFFULL;
    return USER_STACK_TOP - pages * PAGE_SIZE;
}

uint64_t task_random_mmap_base(void) {
    uint64_t pages;

    pages = rng_get_u32() & 0x3FFULL;
    return USER_DYNAMIC_LIMIT - pages * PAGE_SIZE;
}

static void task_set_exec_state(task_t *task, int state) {
    if (!task) return;
    lock_scheduler();
    task->exec_completed = state;
    unlock_scheduler();
}

uint64_t task_align_up(uint64_t v, uint64_t align) {
    if (align == 0) return v;
    return (v + align - 1u) & ~(align - 1u);
}

static uint64_t task_initial_stack_size(int argc, char **argv, int envc, char **envp, const char *fallback_name, int aux_entries) {
    uint64_t bytes;
    uint64_t size;
    int i;
    int len;
    const char *s;

    bytes = 16;
    bytes += 16;
    bytes += (uint64_t)aux_entries * 16;
    bytes += 8;
    bytes += 8;
    bytes += ((uint64_t)envc + 1u) * 8u;
    bytes += ((uint64_t)argc + 1u) * 8u;
    bytes += 8;

    if (argc > 0 && argv) {
        for (i = 0; i < argc; i++) {
            len = 0;
            if (argv[i]) {
                while (argv[i][len]) len++;
            }
            bytes += task_align_up((uint64_t)len + 1u, 8);
        }
    } else {
        s = fallback_name ? fallback_name : "program";
        len = 0;
        while (s[len]) len++;
        bytes += task_align_up((uint64_t)len + 1u, 8);
    }

    if (envc > 0 && envp) {
        for (i = 0; i < envc; i++) {
            len = 0;
            if (envp[i]) {
                while (envp[i][len]) len++;
            }
            bytes += task_align_up((uint64_t)len + 1u, 8);
        }
    }

    size = task_align_up(bytes, PAGE_SIZE);
    if (size < USER_STACK_INITIAL_MIN) size = USER_STACK_INITIAL_MIN;
    return size;
}

typedef struct exec_cleanup_entry {
    uint64_t old_pd;
    uint64_t old_pages_count;
    struct exec_cleanup_entry *next;
} exec_cleanup_entry_t;

static exec_cleanup_entry_t *exec_cleanup_head;
static exec_cleanup_entry_t *exec_cleanup_tail;
static volatile int exec_cleanup_lock = 0;

static inline void exec_cleanup_lock_acquire(void) {
    while (__sync_lock_test_and_set(&exec_cleanup_lock, 1)) {
        smp_tlb_flush_poll();
        __asm__ volatile ("pause" ::: "memory");
    }
}

static inline void exec_cleanup_lock_release(void) {
    __sync_lock_release(&exec_cleanup_lock);
}

int task_is_current_on_any_cpu(task_t *task) {
    int i;

    if (!task) return 0;
    for (i = 0; i < cpu_count; i++) {
        if (!cpus[i].active) continue;
        if (cpus[i].running_task == task ||
            __atomic_load_n(&cpus[i].switching_task, __ATOMIC_ACQUIRE) == task)
            return 1;
    }
    if (current_task == task) return 1;
    return 0;
}

int task_pml4_is_current_on_any_cpu(uint64_t pml4_phys) {
    task_t *running;
    int i;

    if (!pml4_phys) return 0;
    for (i = 0; i < cpu_count; i++) {
        if (!cpus[i].active) continue;
        running = cpus[i].running_task;
        if (running && running->pml4_phys == pml4_phys) return 1;
        running = __atomic_load_n(&cpus[i].switching_task, __ATOMIC_ACQUIRE);
        if (running && running->pml4_phys == pml4_phys) return 1;
    }
    if (current_task && current_task->pml4_phys == pml4_phys) return 1;
    return 0;
}

int task_pml4_live_elsewhere(uint64_t pml4_phys) {
    task_t *running;
    int self;
    int i;

    if (!pml4_phys) return 0;
    pml4_phys &= ~0xFFFULL;
    self = smp_processor_id();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (i = 0; i < cpu_count; i++) {
        if (i == self || !cpus[i].active) continue;
        running = cpus[i].running_task;
        if (running && ((running->pml4_phys & ~0xFFFULL) == pml4_phys ||
                        (running->cr3 & ~0xFFFULL) == pml4_phys))
            return 1;
        running = __atomic_load_n(&cpus[i].switching_task, __ATOMIC_ACQUIRE);
        if (running && ((running->pml4_phys & ~0xFFFULL) == pml4_phys ||
                        (running->cr3 & ~0xFFFULL) == pml4_phys))
            return 1;
    }
    return 0;
}

static int task_refs_pml4(task_t *t, uint64_t pd_page, int include_exec_old) {
    registers_t *frame;

    if (!t || t->resources_released) return 0;
    frame = t->syscall_frame;
    if ((t->pml4_phys & ~0xFFFULL) == pd_page ||
        (t->cr3 & ~0xFFFULL) == pd_page ||
        (include_exec_old && (t->exec_old_pml4 & ~0xFFFULL) == pd_page) ||
        (t->regs.entry_cr3 & ~0xFFFULL) == pd_page ||
        (t->regs.return_cr3 & ~0xFFFULL) == pd_page ||
        (t->regs.saved_entry_cr3 & ~0xFFFULL) == pd_page ||
        (frame && ((frame->entry_cr3 & ~0xFFFULL) == pd_page ||
                   (frame->return_cr3 & ~0xFFFULL) == pd_page ||
                   (frame->saved_entry_cr3 & ~0xFFFULL) == pd_page))) {
        return 1;
    }
    return 0;
}

static int task_pml4_has_owner(task_t *owner, uint64_t pd, int check_owner, int include_owner_exec_old) {
    task_t *t;
    uint64_t pd_page;
    int found;

    if (!pd) return 0;
    pd_page = pd & ~0xFFFULL;
    found = 0;
    lock_scheduler();
    t = all_tasks_head;
    while (t) {
        if (!task_ptr_valid(t)) break;
        if (t == owner) {
            if (check_owner && task_refs_pml4(t, pd_page, include_owner_exec_old)) {
                found = 1;
                break;
            }
        } else {
            if (task_refs_pml4(t, pd_page, 1)) {
                found = 1;
                break;
            }
        }
        t = t->all_next;
    }
    unlock_scheduler();
    return found;
}

static int task_pml4_has_other_owner(task_t *owner, uint64_t pd) {
    return task_pml4_has_owner(owner, pd, 0, 0);
}

int task_free_pml4_if_unowned(task_t *owner, uint64_t pd) {
    if (!pd) return 1;
    if (task_is_current_on_any_cpu(owner)) return 0;
    if ((read_cr3() & ~0xFFFULL) == (pd & ~0xFFFULL)) return 0;
    if (task_pml4_has_other_owner(owner, pd)) return 0;
    vmm_free_pml4(pd);
    return 1;
}

int task_free_exec_old_pml4_if_unowned(task_t *owner, uint64_t pd) {
    if (!pd) return 1;
    if (task_is_current_on_any_cpu(owner)) return 0;
    if ((read_cr3() & ~0xFFFULL) == (pd & ~0xFFFULL)) return 0;
    if (task_pml4_has_owner(owner, pd, 1, 0)) return 0;
    vmm_free_pml4(pd);
    return 1;
}

void exec_cleanup_enqueue(uint64_t pd, uint64_t count) {
    exec_cleanup_entry_t *entry;

    entry = (exec_cleanup_entry_t *)kmalloc(sizeof(exec_cleanup_entry_t));
    if (!entry) {
        task_free_pml4_if_unowned(NULL, pd);
        return;
    }
    entry->old_pd = pd;
    entry->old_pages_count = count;
    entry->next = NULL;

    exec_cleanup_lock_acquire();
    if (exec_cleanup_tail) exec_cleanup_tail->next = entry;
    else exec_cleanup_head = entry;
    exec_cleanup_tail = entry;
    exec_cleanup_lock_release();
}

void exec_cleanup_drain(void) {
    uint64_t pd;
    int freed;
    exec_cleanup_entry_t *entry;

    while (1) {
        exec_cleanup_lock_acquire();
        entry = exec_cleanup_head;
        if (!entry) {
            exec_cleanup_lock_release();
            return;
        }
        exec_cleanup_head = entry->next;
        if (!exec_cleanup_head) exec_cleanup_tail = NULL;
        exec_cleanup_lock_release();

        pd = entry->old_pd;
        freed = task_free_pml4_if_unowned(NULL, pd);
        if (!freed) {
            entry->next = NULL;
            exec_cleanup_lock_acquire();
            if (exec_cleanup_tail) exec_cleanup_tail->next = entry;
            else exec_cleanup_head = entry;
            exec_cleanup_tail = entry;
            exec_cleanup_lock_release();
            return;
        }
        kfree(entry);
    }
}

static void task_discard_exec_old(task_t *t) {
    uint64_t pd;
    uint64_t count;

    if (!t || !t->exec_old_pml4) return;
    pd = t->exec_old_pml4;
    count = t->exec_old_pages_count;
    t->exec_old_pml4 = 0;
    t->exec_old_pages_count = 0;
    exec_cleanup_enqueue(pd, count);
}

void exec_cleanup_stats(uint64_t *entries, uint64_t *pages) {
    exec_cleanup_entry_t *entry;
    uint64_t e;
    uint64_t p;

    e = 0;
    p = 0;
    exec_cleanup_lock_acquire();
    entry = exec_cleanup_head;
    while (entry) {
        e++;
        p += entry->old_pages_count;
        entry = entry->next;
    }
    exec_cleanup_lock_release();
    if (entries) *entries = e;
    if (pages) *pages = p;
}

static void task_error(const char *fmt, ...) {
    char buf[256];
    int n;
    va_list ap;
    
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s", buf);
    if (current_task && current_task->console_id >= 0 && console_is_initialized()) {
        console_write_to(current_task->console_id, buf, (size_t)n);
    }
}

static void __attribute__((noinline)) task_free_owned_exec_args(
        int argc, char **argv, int envc, char **envp) {
    int i;

    if (argv) {
        for (i = 0; i < argc; i++) {
            if (argv[i]) kfree(argv[i]);
        }
        kfree(argv);
    }
    if (envp) {
        for (i = 0; i < envc; i++) {
            if (envp[i]) kfree(envp[i]);
        }
        kfree(envp);
    }
}

static int task_exec_with_args_common(
        vfs_node_t *bin_node, const uint8_t *bin_start, uint64_t bin_size,
        registers_t *regs, int argc, char **argv, int envc, char **envp,
        int take_arguments) {
    uint8_t *kernel_bin;
    char **k_argv;
    char **k_envp;
    int i, len;
    int elf_valid;
    uint64_t old_pd;
    uint64_t stack_top;
    uint64_t stack_size;
    uint64_t new_pd;
    uint64_t *elf_pages;
    uint64_t elf_page_count;
    int load_result;
    uint64_t stack_page_count;
    uint64_t *stack_pages;
    uint64_t total_pages;
    uint64_t sp;
    uint8_t random_bytes[16];
    uint64_t random_addr;
    uint64_t entry_to_use;
    uint64_t final_entry;
    uint64_t final_sp;
    const char *base;
    int n;
    elf_info_t elf_info;
    int use_node;
    int held_node;
    uint64_t tbl_stack[64];
    uint64_t *tbl_buf;
    int tbl_idx;
    uint64_t tbl_bytes;
    int tbl_cap;
    int tbl_heap;
    int argv_table_index;
    int envp_table_index;
    task_vm_t *old_vm;
    int load_errno;
    uint64_t new_user_brk;
    uint64_t exec_euid;
    uint64_t exec_egid;
    uint64_t stack_limit;
    task_file_map_list_t new_file_maps;

    if (!current_task || !current_task->is_user) {
        task_error("task_exec_with_args: can only exec in user tasks\n");
        if (take_arguments)
            task_free_owned_exec_args(argc, argv, envc, envp);
        if (bin_start) kfree((void *)bin_start);
        return -KERR_EPERM;
    }
    use_node = (bin_node != NULL) ? 1 : 0;
    exec_euid = current_task->euid;
    exec_egid = current_task->egid;
    held_node = 0;
    if (use_node) {
        bin_size = bin_node->length;
        if (!(vfs_get_mount_flags_for_node(bin_node) & VFS_MS_NOSUID)) {
            if (bin_node->mask & 04000) exec_euid = bin_node->uid;
            if (bin_node->mask & 02000) exec_egid = bin_node->gid;
        }
    }
    if ((!use_node && !bin_start) || bin_size == 0) {
        task_error("task_exec_with_args: invalid binary\n");
        if (take_arguments)
            task_free_owned_exec_args(argc, argv, envc, envp);
        if (bin_start) kfree((void *)bin_start);
        return -KERR_ENOEXEC;
    }

    kernel_bin = (uint8_t *)bin_start;

    k_argv = NULL;
    k_envp = NULL;

    if (take_arguments) {
        k_argv = argv;
        k_envp = envp;
    } else {
        if (argc > 0 && argv) {
            k_argv = (char **)kmalloc((argc + 1) * sizeof(char *));
            if (!k_argv) {
                if (kernel_bin) kfree(kernel_bin);
                return -KERR_ENOMEM;
            }
            for (i = 0; i < argc; i++) {
                len = strlen(argv[i]);
                k_argv[i] = (char *)kmalloc(len + 1);
                if (!k_argv[i]) {
                    task_free_owned_exec_args(i, k_argv, 0, NULL);
                    if (kernel_bin) kfree(kernel_bin);
                    return -KERR_ENOMEM;
                }
                memcpy(k_argv[i], argv[i], len + 1);
            }
            k_argv[argc] = NULL;
        }

        if (envc > 0 && envp) {
            k_envp = (char **)kmalloc((envc + 1) * sizeof(char *));
            if (!k_envp) {
                task_free_owned_exec_args(argc, k_argv, 0, NULL);
                if (kernel_bin) kfree(kernel_bin);
                return -KERR_ENOMEM;
            }
            for (i = 0; i < envc; i++) {
                len = strlen(envp[i]);
                k_envp[i] = (char *)kmalloc(len + 1);
                if (!k_envp[i]) {
                    task_free_owned_exec_args(argc, k_argv, i, k_envp);
                    if (kernel_bin) kfree(kernel_bin);
                    return -KERR_ENOMEM;
                }
                memcpy(k_envp[i], envp[i], len + 1);
            }
            k_envp[envc] = NULL;
        }
    }

    elf_valid = 0;
    if (!use_node) {
        elf_valid = elf_validate(kernel_bin, bin_size);
    }
    if (elf_valid != 0) {
        task_error("task_exec_with_args: ELF validation failed code=%d size=%llu magic=%02x%02x%02x%02x type=%04x\n",
                   elf_valid, (unsigned long long)bin_size,
                   kernel_bin[0], kernel_bin[1], kernel_bin[2], kernel_bin[3],
                   (unsigned)((const Elf64_Ehdr *)kernel_bin)->e_type);
        task_free_owned_exec_args(argc, k_argv, envc, k_envp);
        if (kernel_bin) kfree(kernel_bin);
        return -KERR_ENOEXEC;
    }

    stack_size = task_initial_stack_size(argc, k_argv, envc, k_envp, "program", 11);
    stack_limit = task_rlimit_get(current_task, 3, 0);
    if (stack_limit > USER_STACK_TOP - USER_STACK_FLOOR - USER_STACK_GAP)
        stack_limit = USER_STACK_TOP - USER_STACK_FLOOR - USER_STACK_GAP;
    if (stack_size > stack_limit) {
        task_error("task_exec_with_args: initial stack too large\n");
        task_free_owned_exec_args(argc, k_argv, envc, k_envp);
        if (kernel_bin) kfree(kernel_bin);
        return -KERR_ENOMEM;
    }
    stack_top = task_random_stack_top();
    if (stack_top < USER_STACK_FLOOR + USER_STACK_GAP + stack_size)
        stack_top = USER_STACK_TOP;

    old_pd = current_task->pml4_phys;
    old_vm = current_task->vm;
    new_pd = vmm_create_pml4();
    current_task->vm = new_pd ? task_vm_alloc() : NULL;
    if (!current_task->vm) {
        task_error("task_exec_with_args: failed to create page directory\n");
        current_task->vm = old_vm;
        if (new_pd) vmm_free_pml4(new_pd);
        task_free_owned_exec_args(argc, k_argv, envc, k_envp);
        if (kernel_bin) kfree(kernel_bin);
        return -KERR_ENOMEM;
    }
    task_set_exec_state(current_task, 2);

    if (use_node) {
        vfs_open(bin_node, 0);
        held_node = 1;
    }

    elf_pages = NULL;
    elf_page_count = 0;
    task_file_map_list_init(&new_file_maps);
    if (use_node) {
        load_result = elf_load_node_to_pd(new_pd, bin_node, &new_file_maps,
                                          &elf_info, &elf_pages,
                                          &elf_page_count);
    } else {
        load_result = elf_load_to_pd(new_pd, kernel_bin, bin_size, &elf_info, &elf_pages, &elf_page_count);
    }
    if (held_node) {
        vfs_close(bin_node);
        held_node = 0;
    }
    if (load_result != 0) {
        load_errno = (load_result == -10 || load_result == -12) ?
                     KERR_ENOMEM : KERR_ENOEXEC;
        task_error("task_exec_with_args: ELF loading failed code=%d\n", load_result);
        task_file_map_list_release(&new_file_maps);
        task_exec_abort_vm(current_task, old_vm);
        if (elf_pages) kfree(elf_pages);
        vmm_free_pml4(new_pd);
        task_free_owned_exec_args(argc, k_argv, envc, k_envp);
        if (kernel_bin) kfree(kernel_bin);
        task_set_exec_state(current_task, 0);
        return -load_errno;
    }
    if (use_node) task_file_map_list_adopt(current_task, &new_file_maps);

    if (kernel_bin) kfree(kernel_bin);

    stack_page_count = 0;
    stack_pages = vmm_map_range_in_pml4_tracked(new_pd, stack_top - USER_STACK_GAP - stack_size, stack_size, 0x7 | VMM_PTE_NX, &stack_page_count);
    if (!stack_pages) {
        task_error("task_exec_with_args: failed to map stack\n");
        task_exec_abort_vm(current_task, old_vm);
        if (elf_pages) kfree(elf_pages);
        vmm_free_pml4(new_pd);
        task_free_owned_exec_args(argc, k_argv, envc, k_envp);
        task_set_exec_state(current_task, 0);
        return -KERR_ENOMEM;
    }

    new_user_brk = (elf_info.bss_end + 0xFFF) & ~0xFFFu;
    if (new_user_brk < USER_DYNAMIC_LIMIT - 0x100000ULL)
        new_user_brk += (rng_get_u32() & 0xFFULL) * PAGE_SIZE;

    total_pages = elf_page_count + stack_page_count;
    if (elf_page_count > UINT64_MAX - stack_page_count ||
        total_pages == 0 || total_pages > SIZE_MAX / sizeof(uint64_t)) {
        task_error("task_exec_with_args: invalid total_pages=%u\n", total_pages);
        task_exec_abort_vm(current_task, old_vm);
        kfree(elf_pages);
        kfree(stack_pages);
        task_free_owned_exec_args(argc, k_argv, envc, k_envp);
        vmm_free_pml4(new_pd);
        task_set_exec_state(current_task, 0);
        return -KERR_ENOEXEC;
    }

    if (total_pages > UINT64_MAX / PAGE_SIZE ||
        !task_memory_total_allows(current_task, total_pages * PAGE_SIZE)) {
        task_exec_abort_vm(current_task, old_vm);
        kfree(elf_pages);
        kfree(stack_pages);
        task_free_owned_exec_args(argc, k_argv, envc, k_envp);
        vmm_free_pml4(new_pd);
        task_set_exec_state(current_task, 0);
        return -KERR_ENOMEM;
    }

    kfree(elf_pages);
    kfree(stack_pages);

    sp = stack_top - USER_STACK_GAP - 16;

    tbl_buf = NULL;
    tbl_idx = 0;
    tbl_bytes = 0;
    tbl_cap = argc + envc + 32;
    tbl_heap = 0;

    if (tbl_cap <= (int)(sizeof(tbl_stack) / sizeof(tbl_stack[0]))) {
        tbl_buf = tbl_stack;
    } else {
        tbl_buf = (uint64_t *)kmalloc((uint64_t)tbl_cap * sizeof(uint64_t));
        tbl_heap = 1;
    }
    if (!tbl_buf) {
        task_exec_abort_vm(current_task, old_vm);
        vmm_free_pml4(new_pd);
        task_free_owned_exec_args(argc, k_argv, envc, k_envp);
        task_set_exec_state(current_task, 0);
        return -KERR_ENOMEM;
    }

    tbl_buf[tbl_idx++] = (uint64_t)argc;
    argv_table_index = tbl_idx;
    for (i = 0; i <= argc; i++) tbl_buf[tbl_idx++] = 0;
    envp_table_index = tbl_idx;
    for (i = 0; i <= envc; i++) tbl_buf[tbl_idx++] = 0;

    if (envc > 0 && k_envp) {
        for (i = envc - 1; i >= 0; i--) {
            len = 0;
            while (k_envp[i][len]) len++;
            sp -= (len + 1 + 7) & ~7;
            vmm_copy_to_pml4(new_pd, sp, k_envp[i], len + 1);
            tbl_buf[envp_table_index + i] = sp;
        }
    }

    if (argc > 0 && k_argv) {
        for (i = argc - 1; i >= 0; i--) {
            len = 0;
            while (k_argv[i][len]) len++;
            sp -= (len + 1 + 7) & ~7;
            vmm_copy_to_pml4(new_pd, sp, k_argv[i], len + 1);
            tbl_buf[argv_table_index + i] = sp;
        }
    }

    rng_fill(random_bytes, sizeof(random_bytes));
    sp -= 16;
    random_addr = sp;
    vmm_copy_to_pml4(new_pd, sp, random_bytes, 16);

    sp = sp & ~0xF;

#define AT_NULL         0
#define AT_PHDR         3
#define AT_PHENT        4
#define AT_PHNUM        5
#define AT_PAGESZ       6
#define AT_BASE         7
#define AT_ENTRY        9
#define AT_UID          11
#define AT_EUID         12
#define AT_GID          13
#define AT_EGID         14
#define AT_RANDOM       25

    if (elf_info.phdr_vaddr != 0 && elf_info.phnum != 0) {
        tbl_buf[tbl_idx++] = AT_PHDR;
        tbl_buf[tbl_idx++] = elf_info.phdr_vaddr;
    }
    tbl_buf[tbl_idx++] = AT_PHENT;
    tbl_buf[tbl_idx++] = elf_info.phent;
    tbl_buf[tbl_idx++] = AT_PHNUM;
    tbl_buf[tbl_idx++] = elf_info.phnum;
    tbl_buf[tbl_idx++] = AT_ENTRY;
    tbl_buf[tbl_idx++] = elf_info.entry_point;
    tbl_buf[tbl_idx++] = AT_PAGESZ;
    tbl_buf[tbl_idx++] = 4096;
    if (elf_info.interp_entry) {
        tbl_buf[tbl_idx++] = AT_BASE;
        tbl_buf[tbl_idx++] = elf_info.interp_base;
    }
    tbl_buf[tbl_idx++] = AT_UID;
    tbl_buf[tbl_idx++] = current_task->uid;
    tbl_buf[tbl_idx++] = AT_EUID;
    tbl_buf[tbl_idx++] = exec_euid;
    tbl_buf[tbl_idx++] = AT_GID;
    tbl_buf[tbl_idx++] = current_task->gid;
    tbl_buf[tbl_idx++] = AT_EGID;
    tbl_buf[tbl_idx++] = exec_egid;
    tbl_buf[tbl_idx++] = AT_RANDOM;
    tbl_buf[tbl_idx++] = random_addr;
    tbl_buf[tbl_idx++] = AT_NULL;
    tbl_buf[tbl_idx++] = 0;

    tbl_bytes = (uint64_t)tbl_idx * 8;
    sp -= tbl_bytes;
    vmm_copy_to_pml4(new_pd, sp, tbl_buf, tbl_bytes);
    if (tbl_heap) kfree(tbl_buf);

#undef AT_NULL
#undef AT_PHDR
#undef AT_PHENT
#undef AT_PHNUM
#undef AT_PAGESZ
#undef AT_BASE
#undef AT_ENTRY
#undef AT_UID
#undef AT_EUID
#undef AT_GID
#undef AT_EGID
#undef AT_RANDOM

    entry_to_use = elf_info.interp_entry ? elf_info.interp_entry :
                                               elf_info.entry_point;
    if (!use_node && !vmm_get_phys_in_pml4(new_pd, entry_to_use & ~0xFFFULL)) {
        task_exec_abort_vm(current_task, old_vm);
        vmm_free_pml4(new_pd);
        task_free_owned_exec_args(argc, k_argv, envc, k_envp);
        task_set_exec_state(current_task, 0);
        return -KERR_ENOEXEC;
    }

    if (k_argv && k_argv[0]) {
        base = k_argv[0];
        for (i = 0; k_argv[0][i]; i++) {
            if (k_argv[0][i] == '/')
                base = &k_argv[0][i + 1];
        }
        for (n = 0; n < 15 && base[n]; n++)
            current_task->name[n] = base[n];
        current_task->name[n] = '\0';
    }

    task_free_owned_exec_args(argc, k_argv, envc, k_envp);

    final_entry = entry_to_use;
    final_sp = sp;

    regs->ds = 0x23;
    regs->es = 0x23;
    regs->ss = 0x23;
    regs->cs = 0x1B;
    regs->rax = 0;
    regs->rbx = 0;
    regs->rcx = 0;
    regs->rdx = 0;
    regs->rsi = 0;
    regs->rdi = 0;
    regs->rbp = 0;
    regs->r8 = 0;
    regs->r9 = 0;
    regs->r10 = 0;
    regs->r11 = 0;
    regs->r12 = 0;
    regs->r13 = 0;
    regs->r14 = 0;
    regs->r15 = 0;
    regs->rflags = 0x202;
    regs->rsp = final_sp;
    regs->int_no = 0;
    regs->err_code = 0;
    regs->rip = final_entry;

    task_fd_close_cloexec(current_task);

    {
        extern void task_reset_signals_on_exec(void);
        task_reset_signals_on_exec();
    }

    current_task->vm->user_pages_count = total_pages;
    current_task->vm->user_brk = new_user_brk;
    current_task->vm->user_brk_start = new_user_brk;
    current_task->vm->mmap_next_addr = task_random_mmap_base();
    current_task->stack_size = stack_size;
    current_task->pml4_phys = new_pd;
    current_task->cr3 = new_pd;
    regs->entry_cr3 = new_pd;
    regs->return_cr3 = new_pd;
    regs->saved_entry_cr3 = new_pd;
    current_task->regs.rip = final_entry;
    current_task->regs.rsp = final_sp;
    current_task->regs.rbp = 0;
    current_task->regs.rax = 0;
    current_task->regs.rbx = 0;
    current_task->regs.rcx = 0;
    current_task->regs.rdx = 0;
    current_task->regs.rsi = 0;
    current_task->regs.rdi = 0;
    current_task->regs.cs = 0x1B;
    current_task->regs.ds = 0x23;
    current_task->regs.es = 0x23;
    current_task->regs.ss = 0x23;
    current_task->regs.rflags = 0x202;
    current_task->regs.int_no = 0;
    current_task->regs.err_code = 0;
    current_task->regs.rsp = (uint64_t)regs;
    current_task->regs.entry_cr3 = new_pd;
    current_task->regs.return_cr3 = new_pd;
    current_task->regs.saved_entry_cr3 = new_pd;

    task_discard_exec_old(current_task);
    if (old_vm != &task_vm_none &&
        __atomic_sub_fetch(&old_vm->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        task_release_file_mappings(old_vm->file_maps, old_vm->file_map_count);
        shm_close_vm(old_vm);
        current_task->exec_old_pml4 = old_pd;
        current_task->exec_old_pages_count = old_vm->user_pages_count;
        kfree(old_vm);
    }
    current_task->shares_address_space = false;
    current_task->sighand_private = false;

    current_task->tls_base = 0;
    current_task->tls_limit = 0;
    task_write_fs_base(0);

    lock_scheduler();
    if (task_is_traced(current_task)) {
        exec_euid = current_task->euid;
        exec_egid = current_task->egid;
    }
    creds_apply_exec_ids(current_task, exec_euid, exec_egid);
    task_trace_exec();
    unlock_scheduler();
    task_reset_fpu_state(current_task);

    return 0;
}

int task_exec_with_args(const uint8_t *bin_start, uint64_t bin_size, registers_t *regs,
                        int argc, char **argv, int envc, char **envp) {
    return task_exec_with_args_common(NULL, bin_start, bin_size, regs,
                                      argc, argv, envc, envp, 0);
}

int task_exec(const uint8_t *bin_start, uint64_t bin_size, registers_t *regs) {
    uint8_t *kernel_bin;
    char *argv[1];

    if (!bin_start || bin_size == 0) return -KERR_ENOEXEC;
    kernel_bin = (uint8_t *)kmalloc(bin_size);
    if (!kernel_bin) return -KERR_ENOMEM;
    if (copy_from_user(kernel_bin, bin_start, bin_size) < 0) {
        kfree(kernel_bin);
        return -KERR_EPERM;
    }
    argv[0] = "program";
    return task_exec_with_args_common(NULL, kernel_bin, bin_size, regs,
                                      1, argv, 0, NULL, 0);
}

int task_exec_node_with_args(vfs_node_t *node, registers_t *regs,
                             int argc, char **argv, int envc, char **envp) {
    return task_exec_with_args_common(node, NULL, 0, regs,
                                      argc, argv, envc, envp, 0);
}

int task_exec_node_with_owned_args(vfs_node_t *node, registers_t *regs,
                                   int argc, char **argv,
                                   int envc, char **envp) {
    return task_exec_with_args_common(node, NULL, 0, regs,
                                      argc, argv, envc, envp, 1);
}
