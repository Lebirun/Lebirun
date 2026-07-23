#include <lebirun/uaccess.h>
#include <lebirun/mem_map.h>
#include <lebirun/task.h>
#include <string.h>

static int user_page_access_ok(uint64_t address, int access) {
    uint64_t flags;

    flags = vmm_get_flags_in_pml4(current_task->pml4_phys, address);
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
    for (;;) {
        if (!user_page_access_ok(page, access)) return 0;
        if (page == (end & ~(PAGE_SIZE - 1))) break;
        page += PAGE_SIZE;
    }
    return 1;
}

int copy_from_user(void *dest, const void *src, size_t size) {
    if (!dest && size != 0) return -1;
    if (!user_access_ok(src, size, UACCESS_READ)) return -1;
    if (size != 0) {
        vmm_read_from_pml4(current_task->pml4_phys,
                           (uint64_t)(uintptr_t)src, dest, size);
    }
    return 0;
}

int copy_to_user(void *dest, const void *src, size_t size) {
    if (!src && size != 0) return -1;
    if (!user_access_ok(dest, size, UACCESS_WRITE)) return -1;
    if (size != 0) {
        vmm_copy_to_pml4(current_task->pml4_phys,
                         (uint64_t)(uintptr_t)dest, src, size);
    }
    return 0;
}

int clear_user(void *dest, size_t size) {
    uint8_t zeroes[64];
    uint64_t address;
    size_t chunk;
    size_t remaining;

    if (!user_access_ok(dest, size, UACCESS_WRITE)) return -1;
    memset(zeroes, 0, sizeof(zeroes));
    address = (uint64_t)(uintptr_t)dest;
    remaining = size;
    while (remaining != 0) {
        chunk = remaining < sizeof(zeroes) ? remaining : sizeof(zeroes);
        vmm_copy_to_pml4(current_task->pml4_phys, address, zeroes, chunk);
        address += chunk;
        remaining -= chunk;
    }
    return 0;
}

int strnlen_user(const char *src, size_t max_size, size_t *length) {
    size_t index;
    char value;

    if (!length) return -1;
    for (index = 0; index < max_size; index++) {
        if (copy_from_user(&value, src + index, 1) < 0) return -1;
        if (value == '\0') {
            *length = index;
            return 0;
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
