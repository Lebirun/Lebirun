#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

DIR *opendir(const char *name) {
    int fd = vfs_open(name, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        errno = ENOENT;
        return NULL;
    }
    DIR *dir = malloc(sizeof(DIR));
    if (!dir) {
        vfs_close_fd(fd);
        errno = ENOMEM;
        return NULL;
    }
    dir->dd_fd = fd;
    dir->dd_loc = 0;
    dir->dd_size = 0;
    dir->dd_buf = NULL;
    memset(&dir->dd_dirent, 0, sizeof(dir->dd_dirent));
    return dir;
}

DIR *fdopendir(int fd) {
    DIR *dir = malloc(sizeof(DIR));
    if (!dir) {
        errno = ENOMEM;
        return NULL;
    }
    dir->dd_fd = fd;
    dir->dd_loc = 0;
    dir->dd_size = 0;
    dir->dd_buf = NULL;
    memset(&dir->dd_dirent, 0, sizeof(dir->dd_dirent));
    return dir;
}

struct dirent *readdir(DIR *dirp) {
    if (!dirp) {
        errno = EBADF;
        return NULL;
    }
    unsigned int type = 0;
    int ret = vfs_readdir(dirp->dd_fd, dirp->dd_dirent.d_name, &type, dirp->dd_loc);
    if (ret != 0) {
        return NULL;
    }
    dirp->dd_dirent.d_ino = dirp->dd_loc + 1;
    dirp->dd_dirent.d_off = dirp->dd_loc;
    dirp->dd_dirent.d_reclen = sizeof(struct dirent);
    dirp->dd_dirent.d_type = (type == 2) ? DT_DIR : DT_REG;
    dirp->dd_loc++;
    return &dirp->dd_dirent;
}

int readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result) {
    struct dirent *d = readdir(dirp);
    if (d) {
        *entry = *d;
        *result = entry;
        return 0;
    }
    *result = NULL;
    return 0;
}

void rewinddir(DIR *dirp) {
    if (dirp) {
        dirp->dd_loc = 0;
    }
}

int closedir(DIR *dirp) {
    if (!dirp) {
        errno = EBADF;
        return -1;
    }
    int ret = vfs_close_fd(dirp->dd_fd);
    free(dirp->dd_buf);
    free(dirp);
    return ret;
}

int dirfd(DIR *dirp) {
    if (!dirp) {
        errno = EINVAL;
        return -1;
    }
    return dirp->dd_fd;
}

void seekdir(DIR *dirp, long loc) {
    if (dirp) {
        dirp->dd_loc = (int)loc;
    }
}

long telldir(DIR *dirp) {
    if (!dirp) {
        errno = EBADF;
        return -1;
    }
    return dirp->dd_loc;
}

int alphasort(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

int scandir(const char *dirp, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **)) {
    DIR *d = opendir(dirp);
    if (!d) return -1;
    
    struct dirent **list = NULL;
    int count = 0;
    int capacity = 0;
    
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (filter && !filter(entry)) continue;
        
        if (count >= capacity) {
            capacity = capacity ? capacity * 2 : 16;
            struct dirent **newlist = realloc(list, capacity * sizeof(struct dirent *));
            if (!newlist) {
                for (int i = 0; i < count; i++) free(list[i]);
                free(list);
                closedir(d);
                errno = ENOMEM;
                return -1;
            }
            list = newlist;
        }
        
        list[count] = malloc(sizeof(struct dirent));
        if (!list[count]) {
            for (int i = 0; i < count; i++) free(list[i]);
            free(list);
            closedir(d);
            errno = ENOMEM;
            return -1;
        }
        *list[count] = *entry;
        count++;
    }
    
    closedir(d);
    
    if (compar && count > 0) {
        for (int i = 0; i < count - 1; i++) {
            for (int j = 0; j < count - i - 1; j++) {
                if (compar((const struct dirent **)&list[j], (const struct dirent **)&list[j+1]) > 0) {
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
