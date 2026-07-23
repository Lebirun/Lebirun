#ifndef UACCESS_H
#define UACCESS_H

#include <stddef.h>
#include <stdint.h>

#define UACCESS_READ 1
#define UACCESS_WRITE 2

int user_access_ok(const void *ptr, size_t size, int access);
int copy_from_user(void *dest, const void *src, size_t size);
int copy_to_user(void *dest, const void *src, size_t size);
int clear_user(void *dest, size_t size);
int strnlen_user(const char *src, size_t max_size, size_t *length);
int copy_string_from_user(char *dest, const char *src, size_t dest_size);

#endif
