#include <lebirun/uaccess.h>
#include <lebirun/mem_map.h>
#include <lebirun/task.h>
#include <lebirun/common.h>
#include <string.h>

static int user_page_fault_in(uint64_t address, int access) {
    uint64_t flags;
    int ok;

    task_vm_lock(current_task);
    flags = vmm_get_flags_in_pml4(current_task->pml4_phys, address);
    ok = 1;
    if (!(flags & VMM_PTE_PRESENT)) {
        ok = task_handle_file_page_fault(current_task, address) ||
             task_handle_anon_page_fault(current_task, address,
                                         (access & UACCESS_WRITE) != 0) ||
             task_map_stack_fault(current_task, address,
                                  address & ~(PAGE_SIZE - 1));
        flags = vmm_get_flags_in_pml4(current_task->pml4_phys, address);
    }
    if (ok && (access & UACCESS_WRITE) && !(flags & VMM_PTE_WRITE) &&
        (flags & VMM_PTE_COW)) {
        ok = cow_handle_fault(address, current_task->pml4_phys) == 1;
        flags = vmm_get_flags_in_pml4(current_task->pml4_phys, address);
    }
    if (ok && (access & UACCESS_WRITE) && !(flags & VMM_PTE_WRITE))
        ok = task_handle_file_write_fault(current_task, address);
    task_vm_unlock(current_task);
    return ok;
}

static int user_page_access_ok(uint64_t address, int access) {
    uint64_t flags;

    flags = vmm_get_flags_in_pml4(current_task->pml4_phys, address);
    if (!(flags & VMM_PTE_PRESENT) ||
        ((access & UACCESS_WRITE) && !(flags & VMM_PTE_WRITE))) {
        if (!user_page_fault_in(address, access)) return 0;
        flags = vmm_get_flags_in_pml4(current_task->pml4_phys, address);
    }
    if (!(flags & VMM_PTE_PRESENT) || !(flags & VMM_PTE_USER)) return 0;
    if ((access & UACCESS_WRITE) && !(flags & VMM_PTE_WRITE)) return 0;
    return 1;
}

int user_access_ok(const void *ptr, size_t size, int access) {
    uint64_t start;
    uint64_t end;
    uint64_t page;

    if (!current_task || !current_task->is_user) return 0;
    if (size == 0) return 1;
    start = (uint64_t)(uintptr_t)ptr;
    if (start < PAGE_SIZE || start >= KERNEL_VMA) return 0;
    if ((uint64_t)size - 1 > UINT64_MAX - start) return 0;
    end = start + (uint64_t)size - 1;
    if (end >= KERNEL_VMA) return 0;
    page = start & ~(PAGE_SIZE - 1);
    end &= ~(PAGE_SIZE - 1);
    for (;;) {
        if (!user_page_access_ok(page, access)) return 0;
        if (page == end) break;
        page += PAGE_SIZE;
    }
    user_access_begin();
    return 1;
}

size_t uaccess_copy(void *dest, const void *src, size_t size);
size_t uaccess_zero(void *dest, size_t size);
extern char uaccess_copy_insn[];
extern char uaccess_zero_insn[];

__asm__ (
    ".pushsection .text\n"
    ".globl uaccess_copy\n"
    ".type uaccess_copy, @function\n"
    "uaccess_copy:\n"
    "    movq %rdx, %rcx\n"
    ".globl uaccess_copy_insn\n"
    "uaccess_copy_insn:\n"
    "    rep movsb\n"
    "    movq %rcx, %rax\n"
    "    ret\n"
    ".globl uaccess_zero\n"
    ".type uaccess_zero, @function\n"
    "uaccess_zero:\n"
    "    movq %rsi, %rcx\n"
    "    xorl %eax, %eax\n"
    ".globl uaccess_zero_insn\n"
    "uaccess_zero_insn:\n"
    "    rep stosb\n"
    "    movq %rcx, %rax\n"
    "    ret\n"
    ".popsection\n"
);

int uaccess_fixup(uint64_t *rip) {
    if (*rip != (uint64_t)uaccess_copy_insn &&
        *rip != (uint64_t)uaccess_zero_insn) return 0;
    *rip += 2;
    return 1;
}

static size_t user_copy_fast(uint64_t address, uint8_t *kbuf, size_t size,
                             int write) {
    uint64_t rflags;
    size_t left;

    if (address + size > 0x0000800000000000ULL ||
        __atomic_load_n(&current_task->vm->protnone, __ATOMIC_RELAXED) ||
        ((read_cr3() ^ current_task->pml4_phys) & ~0xFFFULL))
        return size;
    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    user_access_begin();
    if (!kbuf)
        left = uaccess_zero((void *)(uintptr_t)address, size);
    else if (write)
        left = uaccess_copy((void *)(uintptr_t)address, kbuf, size);
    else
        left = uaccess_copy(kbuf, (const void *)(uintptr_t)address, size);
    if (!(rflags & (1ULL << 18))) user_access_end();
    return left;
}

static int user_copy_page(uint64_t address, uint8_t *kbuf, uint64_t size,
                          int write) {
    if (!kbuf)
        return vmm_clear_user_page(current_task->pml4_phys, address, size);
    return vmm_copy_user_page(current_task->pml4_phys, address, kbuf, size,
                              write);
}

static int user_copy(uint64_t address, void *buf, size_t size, int access) {
    uint8_t *kbuf;
    uint64_t chunk;
    size_t left;
    int write;

    if (!current_task || !current_task->is_user) return -1;
    if (size == 0) return 0;
    if (address < PAGE_SIZE || address >= KERNEL_VMA ||
        (uint64_t)size - 1 >= KERNEL_VMA - address) return -1;
    kbuf = (uint8_t *)buf;
    write = (access & UACCESS_WRITE) != 0;
    while (size != 0) {
        left = user_copy_fast(address, kbuf, size, write);
        address += size - left;
        if (kbuf) kbuf += size - left;
        size = left;
        if (size == 0) break;
        chunk = PAGE_SIZE - (address & (PAGE_SIZE - 1));
        if (chunk > size) chunk = size;
        if (user_copy_page(address, kbuf, chunk, write) < 0 &&
            (!user_page_access_ok(address & ~(PAGE_SIZE - 1), access) ||
             user_copy_page(address, kbuf, chunk, write) < 0))
            return -1;
        address += chunk;
        if (kbuf) kbuf += chunk;
        size -= chunk;
    }
    return 0;
}

int copy_from_user(void *dest, const void *src, size_t size) {
    if (!dest) return -1;
    return user_copy((uint64_t)(uintptr_t)src, dest, size, UACCESS_READ);
}

int copy_to_user(void *dest, const void *src, size_t size) {
    if (!src) return -1;
    return user_copy((uint64_t)(uintptr_t)dest, (void *)src, size,
                     UACCESS_WRITE);
}

int clear_user(void *dest, size_t size) {
    return user_copy((uint64_t)(uintptr_t)dest, NULL, size, UACCESS_WRITE);
}

int strnlen_user(const char *src, size_t max_size, size_t *length) {
    size_t index;
    size_t chunk;
    size_t part;
    size_t i;
    uint64_t base;
    uint64_t current;
    char buf[128];

    if (!length) return -1;
    if (!src) return -1;
    base = (uint64_t)(uintptr_t)src;
    if (base < PAGE_SIZE || base >= KERNEL_VMA) return -1;
    index = 0;
    while (index < max_size) {
        current = base + index;
        if (current < base) return -1;
        chunk = ((current & ~(PAGE_SIZE - 1)) + PAGE_SIZE) - current;
        if (chunk > max_size - index) chunk = max_size - index;
        while (chunk != 0) {
            part = chunk < sizeof(buf) ? chunk : sizeof(buf);
            if (copy_from_user(buf, src + index, part) < 0) return -1;
            for (i = 0; i < part; i++) {
                if (buf[i] == '\0') {
                    *length = index + i;
                    return 0;
                }
            }
            index += part;
            chunk -= part;
        }
    }
    return -1;
}

int copy_string_from_user(char *dest, const char *src, size_t dest_size) {
    size_t length;

    if (!dest || dest_size == 0) return -1;
    if (strnlen_user(src, dest_size, &length) < 0) return -1;
    if (copy_from_user(dest, src, length + 1) < 0) return -1;
    return 0;
}

char *copy_string_from_user_alloc(const char *src) {
    uint64_t address;
    size_t maximum;
    size_t length;
    size_t chunk;
    char *copy;
    char buf[256];

    if (!src) return NULL;
    address = (uint64_t)(uintptr_t)src;
    if (address < 0x1000 || address >= KERNEL_VMA) return NULL;
    maximum = (size_t)(KERNEL_VMA - address);
    if (maximum > 65536) maximum = 65536;
    chunk = PAGE_SIZE - (address & (PAGE_SIZE - 1));
    if (chunk > sizeof(buf)) chunk = sizeof(buf);
    if (chunk > maximum) chunk = maximum;
    if (copy_from_user(buf, src, chunk) < 0) return NULL;
    for (length = 0; length < chunk; length++) {
        if (buf[length] != '\0') continue;
        copy = (char *)kmalloc(length + 1);
        if (copy) memcpy(copy, buf, length + 1);
        return copy;
    }
    if (strnlen_user(src, maximum, &length) < 0) return NULL;
    if (length == SIZE_MAX) return NULL;
    copy = (char *)kmalloc(length + 1);
    if (!copy) return NULL;
    if (copy_from_user(copy, src, length + 1) < 0) {
        kfree(copy);
        return NULL;
    }
    return copy;
}
