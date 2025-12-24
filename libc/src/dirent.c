#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define SYS_GETDENTS (80 | 0x80000000)

struct __dirstream {
    int fd;
    off_t tell;
    int buf_pos;
    int buf_end;
    char buf[2048];
};

static inline int syscall3(int num, int arg1, int arg2, int arg3) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3)
        : "memory"
    );
    return ret;
}

int getdents(int fd, struct dirent *dirp, size_t count) {
    return syscall3(SYS_GETDENTS, fd, (int)dirp, (int)count);
}

DIR *opendir(const char *name) {
    int fd = open(name, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return (void*)0;
    }
    return fdopendir(fd);
}

DIR *fdopendir(int fd) {
    DIR *dir = (DIR *)malloc(sizeof(DIR));
    if (!dir) {
        close(fd);
        return (void*)0;
    }
    dir->fd = fd;
    dir->tell = 0;
    dir->buf_pos = 0;
    dir->buf_end = 0;
    return dir;
}

int closedir(DIR *dirp) {
    if (!dirp) return -1;
    int ret = close(dirp->fd);
    free(dirp);
    return ret;
}

struct dirent *readdir(DIR *dirp) {
    static struct dirent result;
    
    if (!dirp) return (void*)0;
    
    if (dirp->buf_pos >= dirp->buf_end) {
        int len = getdents(dirp->fd, (struct dirent *)dirp->buf, sizeof(dirp->buf));
        if (len <= 0) {
            return (void*)0;
        }
        dirp->buf_pos = 0;
        dirp->buf_end = len;
    }
    
    struct dirent *de = (struct dirent *)(dirp->buf + dirp->buf_pos);
    
    result.d_ino = de->d_ino;
    result.d_off = de->d_off;
    result.d_reclen = de->d_reclen;
    result.d_type = de->d_type;
    
    int i = 0;
    const char *src = de->d_name;
    while (*src && i < 255) {
        result.d_name[i++] = *src++;
    }
    result.d_name[i] = '\0';
    
    dirp->buf_pos += de->d_reclen;
    dirp->tell++;
    
    return &result;
}

int readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result) {
    struct dirent *de = readdir(dirp);
    if (!de) {
        *result = (void*)0;
        return 0;
    }
    *entry = *de;
    *result = entry;
    return 0;
}

void rewinddir(DIR *dirp) {
    if (dirp) {
        lseek(dirp->fd, 0, SEEK_SET);
        dirp->buf_pos = 0;
        dirp->buf_end = 0;
        dirp->tell = 0;
    }
}

void seekdir(DIR *dirp, long loc) {
    if (dirp) {
        lseek(dirp->fd, loc, SEEK_SET);
        dirp->buf_pos = 0;
        dirp->buf_end = 0;
        dirp->tell = loc;
    }
}

long telldir(DIR *dirp) {
    return dirp ? dirp->tell : -1;
}

int dirfd(DIR *dirp) {
    return dirp ? dirp->fd : -1;
}

ssize_t posix_getdents(int fd, void *buf, size_t nbytes, int flags) {
    (void)flags;
    return getdents(fd, (struct dirent *)buf, nbytes);
}

int alphasort(const struct dirent **a, const struct dirent **b) {
    const char *s1 = (*a)->d_name;
    const char *s2 = (*b)->d_name;
    while (*s1 && *s2) {
        if (*s1 != *s2) return (unsigned char)*s1 - (unsigned char)*s2;
        s1++; s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int scandir(const char *path, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **)) {
    DIR *d = opendir(path);
    if (!d) return -1;
    
    struct dirent **list = (void*)0;
    int count = 0;
    int capacity = 0;
    
    struct dirent *entry;
    while ((entry = readdir(d)) != (void*)0) {
        if (filter && !filter(entry)) continue;
        
        if (count >= capacity) {
            capacity = capacity ? capacity * 2 : 16;
            struct dirent **newlist = (struct dirent **)realloc(list, capacity * sizeof(struct dirent *));
            if (!newlist) {
                for (int i = 0; i < count; i++) free(list[i]);
                free(list);
                closedir(d);
                return -1;
            }
            list = newlist;
        }
        
        struct dirent *copy = (struct dirent *)malloc(sizeof(struct dirent));
        if (!copy) {
            for (int i = 0; i < count; i++) free(list[i]);
            free(list);
            closedir(d);
            return -1;
        }
        *copy = *entry;
        list[count++] = copy;
    }
    
    closedir(d);
    
    if (compar && count > 1) {
        for (int i = 0; i < count - 1; i++) {
            for (int j = 0; j < count - i - 1; j++) {
                if (compar((const struct dirent **)&list[j], 
                           (const struct dirent **)&list[j+1]) > 0) {
                    struct dirent *tmp = list[j];
                    list[j] = list[j+1];
                    list[j+1] = tmp;
                }
            }
        }
    }
    
    *namelist = list;
    return count;
}
