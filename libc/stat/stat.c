#if !defined(__is_libk)

#include <sys/stat.h>
#include <syscall.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

static mode_t current_umask = 022;

int stat(const char *path, struct stat *buf) {
    if (!path || !buf) {
        errno = EINVAL;
        return -1;
    }
    
    int fd = syscall2(SYS_VFS_OPEN, (int)path, 0);
    if (fd < 0) {
        errno = ENOENT;
        return -1;
    }
    
    unsigned int size = 0;
    unsigned int type = 0;
    int ret = syscall3(SYS_VFS_STAT, fd, (int)&size, (int)&type);
    syscall1(SYS_VFS_CLOSE, fd);
    
    if (ret < 0) {
        errno = EIO;
        return -1;
    }
    
    memset(buf, 0, sizeof(struct stat));
    buf->st_size = (off_t)size;
    buf->st_blksize = 512;
    buf->st_blocks = (size + 511) / 512;
    
    if (type == 2) {
        buf->st_mode = S_IFDIR | 0755;
        buf->st_nlink = 2;
    } else {
        buf->st_mode = S_IFREG | 0644;
        buf->st_nlink = 1;
    }
    
    buf->st_dev = 1;
    buf->st_ino = 1;
    buf->st_uid = 0;
    buf->st_gid = 0;
    buf->st_rdev = 0;
    
    return 0;
}

int fstat(int fd, struct stat *buf) {
    if (fd < 0 || !buf) {
        errno = EINVAL;
        return -1;
    }
    
    unsigned int size = 0;
    unsigned int type = 0;
    int ret = syscall3(SYS_VFS_STAT, fd, (int)&size, (int)&type);
    
    if (ret < 0) {
        errno = EBADF;
        return -1;
    }
    
    memset(buf, 0, sizeof(struct stat));
    buf->st_size = (off_t)size;
    buf->st_blksize = 512;
    buf->st_blocks = (size + 511) / 512;
    
    if (type == 2) {
        buf->st_mode = S_IFDIR | 0755;
        buf->st_nlink = 2;
    } else {
        buf->st_mode = S_IFREG | 0644;
        buf->st_nlink = 1;
    }
    
    buf->st_dev = 1;
    buf->st_ino = 1;
    buf->st_uid = 0;
    buf->st_gid = 0;
    buf->st_rdev = 0;
    
    return 0;
}

int lstat(const char *path, struct stat *buf) {
    return stat(path, buf);
}

int fstatat(int dirfd, const char *pathname, struct stat *buf, int flags) {
    if (!pathname || !buf) {
        errno = EINVAL;
        return -1;
    }
    
    (void)dirfd;
    (void)flags;
    
    if (flags & AT_SYMLINK_NOFOLLOW) {
        return lstat(pathname, buf);
    }
    return stat(pathname, buf);
}

int mkdir(const char *path, mode_t mode) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    
    mode_t actual_mode = mode & ~current_umask;
    int ret = syscall2(SYS_VFS_MKDIR, (int)path, (int)actual_mode);
    
    if (ret < 0) {
        errno = ENOENT;
        return -1;
    }
    
    return 0;
}

int chmod(const char *path, mode_t mode) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    (void)mode;
    
    int fd = syscall2(SYS_VFS_OPEN, (int)path, 0);
    if (fd < 0) {
        errno = ENOENT;
        return -1;
    }
    
    syscall1(SYS_VFS_CLOSE, fd);
    return 0;
}

int fchmod(int fd, mode_t mode) {
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    (void)mode;
    return 0;
}

mode_t umask(mode_t mask) {
    mode_t old = current_umask;
    current_umask = mask & 0777;
    return old;
}

int mknod(const char *path, mode_t mode, dev_t dev) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    
    (void)dev;
    
    if (S_ISREG(mode)) {
        int ret = syscall2(SYS_VFS_CREATE, (int)path, (int)(mode & 0777));
        if (ret < 0) {
            errno = ENOENT;
            return -1;
        }
        return 0;
    }
    
    errno = EPERM;
    return -1;
}

#endif
