#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct block_header {
    size_t size;
    struct block_header *next;
    int free;
} block_header_t;

static block_header_t *heap_start = NULL;

static block_header_t *find_free_block(size_t size) {
    block_header_t *curr = heap_start;
    while (curr) {
        if (curr->free && curr->size >= size) return curr;
        curr = curr->next;
    }
    return NULL;
}

static block_header_t *request_space(size_t size) {
    size_t total = sizeof(block_header_t) + size;
    total = (total + 15) & ~15;
    
    void *ptr = sbrk((int)total);
    if (ptr == (void *)-1) return NULL;
    
    block_header_t *block = (block_header_t *)ptr;
    block->size = total - sizeof(block_header_t);
    block->next = NULL;
    block->free = 0;
    
    if (!heap_start) {
        heap_start = block;
    } else {
        block_header_t *curr = heap_start;
        while (curr->next) curr = curr->next;
        curr->next = block;
    }
    
    return block;
}

void *malloc(size_t size) {
    if (size == 0) return NULL;
    
    size = (size + 15) & ~15;
    
    block_header_t *block = find_free_block(size);
    if (block) {
        block->free = 0;
        return (void *)(block + 1);
    }
    
    block = request_space(size);
    if (!block) return NULL;
    
    return (void *)(block + 1);
}

void free(void *ptr) {
    if (!ptr) return;
    block_header_t *block = (block_header_t *)ptr - 1;
    block->free = 1;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    
    block_header_t *block = (block_header_t *)ptr - 1;
    if (block->size >= size) return ptr;
    
    void *newptr = malloc(size);
    if (!newptr) return NULL;
    
    memcpy(newptr, ptr, block->size);
    free(ptr);
    return newptr;
}
