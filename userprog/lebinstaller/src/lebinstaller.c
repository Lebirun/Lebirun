#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <crypt.h>
#include <lebirun.h>
#include <lebui.h>

#define SECTOR_SIZE  512
#define BUF_SIZE     4096
#define COPY_BUF_MAX 16384
#define MAX_DISKS    8
#define MAX_PARTS    16
#define MAX_PATH     256
#define MAX_LINE     128
#define LEBPKG_INSTALLED_DIR "/etc/lebpkg/installed"
#define MBR_SIG      0xAA55

int vfs_open(const char *path, int flags);
int vfs_close_fd(int fd);
int vfs_read_fd(int fd, void *buf, unsigned int count);
int vfs_write_fd(int fd, const void *buf, unsigned int count);
int vfs_readdir(int fd, char *name, unsigned int *type, unsigned int index);
int vfs_stat(int fd, uint64_t *size, uint64_t *type);
int vfs_create(const char *path, unsigned int perms);
int vfs_mkdir(const char *path, unsigned int perms);
int vfs_unlink(const char *path);
int vfs_mounts(void);
unsigned int getticks(void);

typedef struct {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t sector_count;
} __attribute__((packed)) mbr_entry_t;

typedef struct {
    uint8_t     bootstrap[446];
    mbr_entry_t parts[4];
    uint16_t    signature;
} __attribute__((packed)) mbr_t;

typedef struct {
    int      valid;
    int      number;
    uint64_t start_lba;
    uint64_t sector_count;
    uint8_t  mbr_type;
    char     devpath[32];
} part_info_t;

typedef struct {
    char     devname[16];
    char     devpath[32];
    uint64_t disk_sectors;
    int      part_count;
    part_info_t parts[MAX_PARTS];
} disk_info_t;

static disk_info_t disks[MAX_DISKS];
static int disk_count;
static lebui_size_t term_sz;
static lebui_prog_state_t prog_st;

static const char *timezones[] = {
    "GMT-12", "GMT-11", "GMT-10", "GMT-9", "GMT-8", "GMT-7",
    "GMT-6",  "GMT-5",  "GMT-4",  "GMT-3", "GMT-2", "GMT-1",
    "GMT+0",
    "GMT+1",  "GMT+2",  "GMT+3",  "GMT+4", "GMT+5", "GMT+6",
    "GMT+7",  "GMT+8",  "GMT+9",  "GMT+10", "GMT+11", "GMT+12",
    "GMT+13", "GMT+14",
    NULL
};

static const char *tz_values[] = {
    "GMT+12", "GMT+11", "GMT+10", "GMT+9", "GMT+8", "GMT+7",
    "GMT+6",  "GMT+5",  "GMT+4",  "GMT+3", "GMT+2", "GMT+1",
    "GMT0",
    "GMT-1",  "GMT-2",  "GMT-3",  "GMT-4", "GMT-5", "GMT-6",
    "GMT-7",  "GMT-8",  "GMT-9",  "GMT-10", "GMT-11", "GMT-12",
    "GMT-13", "GMT-14",
    NULL
};


static int inst_disk_read(const char *devpath, uint32_t lba, uint32_t count, void *buf)
{
    int fd;
    uint32_t total;
    int ret;
    off_t offset;

    fd = vfs_open(devpath, 0);
    if (fd < 0) return -1;

    total = count * SECTOR_SIZE;
    offset = (off_t)lba * SECTOR_SIZE;

    if (offset > 0) {
        if (lseek(fd, offset, SEEK_SET) < 0) {
            vfs_close_fd(fd);
            return -1;
        }
    }

    ret = vfs_read_fd(fd, buf, total);
    vfs_close_fd(fd);
    return (ret < 0) ? -1 : ret;
}

static void inst_format_size(uint64_t sectors, char *buf, int bufsz)
{
    uint64_t bytes;
    uint64_t mb;
    uint64_t gb;

    bytes = sectors * SECTOR_SIZE;
    mb = bytes / (1024 * 1024);
    gb = mb / 1024;

    if (gb >= 1)
        snprintf(buf, bufsz, "%llu GiB", (unsigned long long)gb);
    else
        snprintf(buf, bufsz, "%llu MiB", (unsigned long long)mb);
}

static int inst_is_whole_disk(const char *name)
{
    int i;

    if (name[0] != 's' || name[1] != 'd') return 0;
    if (name[2] < 'a' || name[2] > 'z') return 0;
    for (i = 3; name[i]; i++) {
        if (name[i] >= '0' && name[i] <= '9') return 0;
    }
    return 1;
}

static void inst_scan_disk(disk_info_t *disk)
{
    static uint8_t sector0[SECTOR_SIZE];
    mbr_t *mbr;
    int ret;
    int i;
    int stat_fd;
    int devfd;
    char name[64];
    unsigned int dtype;
    unsigned int didx;
    int dlen;
    char partpath[64];
    uint64_t psize;
    uint64_t ptype;

    memset(disk->parts, 0, sizeof(disk->parts));
    disk->part_count = 0;
    disk->disk_sectors = 0;

    stat_fd = vfs_open(disk->devpath, 0);
    if (stat_fd >= 0) {
        uint64_t stat_size;
        uint64_t stat_type;
        stat_size = 0;
        stat_type = 0;
        if (vfs_stat(stat_fd, &stat_size, &stat_type) == 0) {
            disk->disk_sectors = stat_size / SECTOR_SIZE;
        }
        vfs_close_fd(stat_fd);
    }

    ret = inst_disk_read(disk->devpath, 0, 1, sector0);
    if (ret >= SECTOR_SIZE) {
        mbr = (mbr_t *)sector0;
        if (mbr->signature == MBR_SIG) {
            for (i = 0; i < 4; i++) {
                if (mbr->parts[i].type == 0) continue;
                if (mbr->parts[i].sector_count == 0) continue;
                disk->parts[disk->part_count].valid = 1;
                disk->parts[disk->part_count].number = i + 1;
                disk->parts[disk->part_count].start_lba = mbr->parts[i].lba_start;
                disk->parts[disk->part_count].sector_count = mbr->parts[i].sector_count;
                disk->parts[disk->part_count].mbr_type = mbr->parts[i].type;
                snprintf(disk->parts[disk->part_count].devpath,
                         sizeof(disk->parts[disk->part_count].devpath),
                         "%s%d", disk->devpath, i + 1);
                disk->part_count++;
            }
        }
    }

    if (disk->part_count > 0) return;

    dlen = (int)strlen(disk->devname);
    devfd = vfs_open("/dev", 0);
    if (devfd < 0) return;

    for (didx = 0; disk->part_count < MAX_PARTS; didx++) {
        if (vfs_readdir(devfd, name, &dtype, didx) != 0) break;
        if (strncmp(name, disk->devname, dlen) != 0) continue;
        if (name[dlen] < '1' || name[dlen] > '9') continue;

        snprintf(partpath, sizeof(partpath), "/dev/%s", name);
        stat_fd = vfs_open(partpath, 0);
        if (stat_fd < 0) continue;
        psize = 0;
        ptype = 0;
        vfs_stat(stat_fd, &psize, &ptype);
        vfs_close_fd(stat_fd);

        disk->parts[disk->part_count].valid = 1;
        disk->parts[disk->part_count].number = name[dlen] - '0';
        disk->parts[disk->part_count].start_lba = 0;
        disk->parts[disk->part_count].sector_count = psize / SECTOR_SIZE;
        disk->parts[disk->part_count].mbr_type = 0x83;
        strncpy(disk->parts[disk->part_count].devpath, partpath,
                sizeof(disk->parts[disk->part_count].devpath) - 1);
        disk->part_count++;
    }
    vfs_close_fd(devfd);
}

static int inst_enumerate_disks(void)
{
    int fd;
    char name[64];
    unsigned int type;
    unsigned int idx;

    disk_count = 0;

    fd = vfs_open("/dev", 0);
    if (fd < 0) return -1;

    for (idx = 0; disk_count < MAX_DISKS; idx++) {
        if (vfs_readdir(fd, name, &type, idx) != 0) break;
        if (!inst_is_whole_disk(name)) continue;

        strncpy(disks[disk_count].devname, name, sizeof(disks[disk_count].devname) - 1);
        snprintf(disks[disk_count].devpath, sizeof(disks[disk_count].devpath),
                 "/dev/%s", name);
        inst_scan_disk(&disks[disk_count]);
        disk_count++;
    }
    vfs_close_fd(fd);
    return disk_count;
}

static uint64_t inst_get_free_mem(void)
{
    int fd;
    char mbuf[512];
    int n;
    char *p;
    uint64_t mem_free;

    mem_free = 0;
    fd = vfs_open("/proc/meminfo", 0);
    if (fd < 0) return 0;
    n = vfs_read_fd(fd, mbuf, sizeof(mbuf) - 1);
    vfs_close_fd(fd);
    if (n <= 0) return 0;
    mbuf[n] = '\0';
    p = mbuf;
    while (*p) {
        if (strncmp(p, "MemFree:", 8) == 0) {
            p += 8;
            while (*p == ' ' || *p == '\t') p++;
            while (*p >= '0' && *p <= '9') {
                mem_free = mem_free * 10 + (*p - '0');
                p++;
            }
            break;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return mem_free * 1024;
}

static char *copy_buf = NULL;
static int copy_buf_size = 0;
static int copy_overwrite_existing = 1;

static int inst_resize_copy_buf(uint64_t file_size)
{
    uint64_t avail;
    uint64_t want64;
    char *new_buf;
    int want;

    if (copy_buf && copy_buf_size >= COPY_BUF_MAX) return 0;
    if (copy_buf && file_size > 0 && file_size <= (uint64_t)copy_buf_size) return 0;

    avail = inst_get_free_mem();
    if (avail == 0) avail = BUF_SIZE;

    want64 = avail / 16;
    if (file_size > 0 && file_size < want64) want64 = file_size;
    if (want64 < BUF_SIZE) want64 = BUF_SIZE;
    if (want64 > COPY_BUF_MAX) want64 = COPY_BUF_MAX;
    if (want64 > 0x7fffffffULL) want64 = 0x7fffffffULL;

    want = (int)want64;
    if (copy_buf && copy_buf_size >= want) return 0;

    while (want >= BUF_SIZE) {
        new_buf = (char *)malloc(want);
        if (new_buf) {
            if (copy_buf) free(copy_buf);
            copy_buf = new_buf;
            copy_buf_size = want;
            return 0;
        }
        want /= 2;
    }

    if (copy_buf) return 0;

    return -1;
}

static int inst_copy_file_vfs(const char *src, const char *dst)
{
    int fd_in;
    int fd_out;
    int r;
    struct stat src_st;
    mode_t src_mode;

    fd_in = vfs_open(src, 0);
    if (fd_in < 0) return -1;

    src_mode = 0644;
    src_st.st_size = 0;
    if (fstat(fd_in, &src_st) == 0)
        src_mode = src_st.st_mode & 07777;

    if (inst_resize_copy_buf((uint64_t)src_st.st_size) < 0) {
        vfs_close_fd(fd_in);
        return -1;
    }

    if (!copy_overwrite_existing) {
        fd_out = (int)leb_syscall3(LEB_SYSCALL_VFS_OPEN, (long)dst,
                                   O_WRONLY | O_CREAT | O_TRUNC,
                                   src_mode);
    } else {
        vfs_unlink(dst);
        vfs_create(dst, src_mode);
        fd_out = vfs_open(dst, 2);
    }
    if (fd_out < 0) {
        fd_out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, src_mode);
        if (fd_out < 0) {
            close(fd_in);
            return -1;
        }
    }

    while ((r = read(fd_in, copy_buf, copy_buf_size)) > 0) {
        if (write(fd_out, copy_buf, r) != r) {
            close(fd_in);
            close(fd_out);
            return -1;
        }
    }

    close(fd_in);
    close(fd_out);

    chmod(dst, src_mode);

    return 0;
}

#define PKG_CORE       0
#define PKG_C_HDR      1
#define PKG_C_LIB      2
#define PKG_COUNT      3

static int pkg_selected[PKG_COUNT] = { 1, 1, 1 };

static int inst_pkg_skip(const char *path)
{
    if (!pkg_selected[PKG_C_HDR] && strcmp(path, "/usr/include") == 0)
        return 1;
    if (!pkg_selected[PKG_C_LIB] && strcmp(path, "/usr/lib") == 0)
        return 1;
    return 0;
}

static int inst_count_dir_entries(const char *path)
{
    int fd;
    char name[256];
    unsigned int type;
    unsigned int idx;
    int count;
    char sub[MAX_PATH];

    count = 0;
    fd = vfs_open(path, 0);
    if (fd < 0) return 0;
    for (idx = 0; ; idx++) {
        if (vfs_readdir(fd, name, &type, idx) != 0) break;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (strcmp(name, "lebinstaller") == 0) continue;
        snprintf(sub, sizeof(sub), "%s/%s", path, name);
        if (inst_pkg_skip(sub)) continue;
        if (type == 2) {
            count += inst_count_dir_entries(sub);
        } else if (type == 6) {
            count++;
        } else {
            count++;
        }
    }
    vfs_close_fd(fd);
    return count;
}

static int copy_total;

static int copy_done;
static int copy_last_pct;

static void inst_copy_progress(const char *path)
{
    int pct;

    if (copy_total <= 0) {
        return;
    }

    pct = (copy_done * 100) / copy_total;
    if (pct > 99) pct = 99;
    if (pct != copy_last_pct) {
        copy_last_pct = pct;
        lebui_progress_update(&prog_st, path, pct);
        lebui_progress_log(&prog_st, path);
    }
}

static void inst_copy_current(const char *path)
{
    int pct;

    if (copy_total <= 0) {
        return;
    }

    pct = (copy_done * 100) / copy_total;
    if (pct > 99) pct = 99;
    lebui_progress_update(&prog_st, path, pct);
}

static int inst_copy_symlink_vfs(const char *src, const char *dst)
{
    char link_target[MAX_PATH];
    const char *fast_target;
    int link_len;

    fast_target = NULL;
    if (strncmp(src, "/bin/", 5) == 0) {
        if (strcmp(src + 5, "sh") == 0) {
            fast_target = "lsh";
        } else {
            fast_target = "lebu";
        }
    } else if (strncmp(src, "/sbin/", 6) == 0) {
        fast_target = "../bin/lebu";
    }

    if (fast_target) {
        strncpy(link_target, fast_target, sizeof(link_target) - 1);
        link_target[sizeof(link_target) - 1] = '\0';
    } else {
        link_len = (int)readlink(src, link_target, sizeof(link_target) - 1);
        if (link_len <= 0) {
            return -1;
        }
        link_target[link_len] = '\0';
    }

    if (copy_overwrite_existing) {
        vfs_unlink(dst);
    }

    if (symlink(link_target, dst) < 0) {
        return -1;
    }
    return 0;
}

static int inst_copy_dir_recursive(const char *src, const char *dst, const char *skip)
{
    int fd;
    int dst_fd;
    char name[256];
    unsigned int type;
    unsigned int idx;
    char src_path[MAX_PATH];
    char dst_path[MAX_PATH];
    int errors;
    int slen;
    int dlen;

    vfs_mkdir(dst, 0755);

    dst_fd = vfs_open(dst, 0);
    fd = vfs_open(src, 0);
    if (fd < 0) {
        if (dst_fd >= 0) {
            vfs_close_fd(dst_fd);
        }
        return -1;
    }

    errors = 0;
    for (idx = 0; ; idx++) {
        if (vfs_readdir(fd, name, &type, idx) != 0) break;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        slen = snprintf(src_path, sizeof(src_path), "%s/%s", src, name);
        if (slen < 0 || slen >= (int)sizeof(src_path)) {
            errors++;
            continue;
        }

        if (skip && strcmp(src_path, skip) == 0) continue;
        if (strcmp(name, "lebinstaller") == 0) continue;
        if (inst_pkg_skip(src_path)) continue;

        dlen = snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, name);
        if (dlen < 0 || dlen >= (int)sizeof(dst_path)) {
            errors++;
            continue;
        }

        if (type == 2) {
            if (inst_copy_dir_recursive(src_path, dst_path, skip) < 0)
                errors++;
        } else if (type == 6) {
            inst_copy_current(src_path);
            if (inst_copy_symlink_vfs(src_path, dst_path) < 0)
                errors++;
            copy_done++;
            inst_copy_progress(src_path);
        } else {
            inst_copy_current(src_path);
            if (inst_copy_file_vfs(src_path, dst_path) < 0) {
                errors++;
            }
            copy_done++;
            inst_copy_progress(src_path);
        }
    }
    vfs_close_fd(fd);
    if (dst_fd >= 0) {
        vfs_close_fd(dst_fd);
    }
    return (errors > 0) ? -1 : 0;
}

static int inst_count_rootfs(void)
{
    static const char *dirs[] = {
        "bin", "boot", "dev", "etc", "home", "lib", "proc",
        "root", "sbin", "tmp", "usr", "var", NULL
    };
    int total;
    int i;
    char path[MAX_PATH];

    total = 1;
    for (i = 0; dirs[i]; i++) {
        if (strcmp(dirs[i], "dev") == 0 || strcmp(dirs[i], "proc") == 0)
            continue;
        snprintf(path, sizeof(path), "/%s", dirs[i]);
        total += inst_count_dir_entries(path);
    }
    return total;
}

static int inst_mount_partition(const char *devpath, const char *mountpoint)
{
    int ret;
    int pid;

    pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        int nfd;
        char *argv[6];
        nfd = open("/dev/null", O_WRONLY);
        if (nfd >= 0) { dup2(nfd, 1); dup2(nfd, 2); close(nfd); }
        argv[0] = "mount";
        argv[1] = "-t";
        argv[2] = "ext4";
        argv[3] = (char *)devpath;
        argv[4] = (char *)mountpoint;
        argv[5] = NULL;
        execv("/sbin/mount", argv);
        execv("/bin/mount", argv);
        execv("/bin/lebu", argv);
        _exit(127);
    }

    waitpid(pid, &ret, 0);
    return ret;
}

static int inst_umount_partition(const char *mountpoint)
{
    int ret;

    ret = (int)leb_syscall1(LEB_SYSCALL_VFS_UMOUNT, (long)mountpoint);
    return ret;
}

static int inst_format_ext4(const char *devpath)
{
    int pid;
    int ret;

    pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        int null_fd;
        char *argv[3];
        null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            dup2(null_fd, 1);
            dup2(null_fd, 2);
            close(null_fd);
        }
        argv[0] = "lformat.ext4";
        argv[1] = (char *)devpath;
        argv[2] = NULL;
        execv("/sbin/lformat.ext4", argv);
        execv("/bin/lformat.ext4", argv);
        execv("/bin/lebu", argv);
        _exit(127);
    }

    waitpid(pid, &ret, 0);
    return ret;
}

static int inst_copy_rootfs(const char *mountpoint)
{
    static const char *dirs[] = {
        "bin", "boot", "dev", "etc", "home", "lib", "proc",
        "root", "sbin", "tmp", "usr", "var", NULL
    };
    static const char *root_files[] = {
        "init", NULL
    };
    char src[MAX_PATH];
    char dst[MAX_PATH];
    int i;
    int errors;

    errors = 0;

    for (i = 0; root_files[i]; i++) {
        snprintf(src, sizeof(src), "/%s", root_files[i]);
        snprintf(dst, sizeof(dst), "%s/%s", mountpoint, root_files[i]);
        inst_copy_current(src);
        if (inst_copy_file_vfs(src, dst) < 0) {
            errors++;
        }
        copy_done++;
        inst_copy_progress(src);
    }

    for (i = 0; dirs[i]; i++) {
        snprintf(src, sizeof(src), "/%s", dirs[i]);
        snprintf(dst, sizeof(dst), "%s/%s", mountpoint, dirs[i]);

        if (strcmp(dirs[i], "dev") == 0 || strcmp(dirs[i], "proc") == 0) {
            vfs_mkdir(dst, 0755);
            continue;
        }
        if (inst_copy_dir_recursive(src, dst, mountpoint) < 0) {
            errors++;
        }
    }

    return (errors > 0) ? -1 : 0;
}

static int inst_install_grub_mbr(const char *disk_dev, int boot_part_num)
{
    static uint8_t mbr_buf[SECTOR_SIZE];
    static uint8_t boot_buf[SECTOR_SIZE];
    static uint8_t core_buf[BUF_SIZE];
    static uint8_t verify_buf[SECTOR_SIZE];
    int fd_disk;
    int fd_boot;
    int fd_core;
    int r;
    int off;
    int pi;
    int entry_off;
    uint32_t core_lba;

    fd_disk = vfs_open(disk_dev, 2);
    if (fd_disk < 0) return -1;

    r = vfs_read_fd(fd_disk, mbr_buf, SECTOR_SIZE);
    if (r < SECTOR_SIZE) {
        vfs_close_fd(fd_disk);
        return -1;
    }
    vfs_close_fd(fd_disk);

    fd_boot = vfs_open("/boot/grub/i386-pc/boot.img", 0);
    if (fd_boot < 0) return -1;
    vfs_read_fd(fd_boot, boot_buf, SECTOR_SIZE);
    vfs_close_fd(fd_boot);

    memcpy(mbr_buf, boot_buf, 440);

    core_lba = 1;
    mbr_buf[0x5C] = (uint8_t)(core_lba & 0xFF);
    mbr_buf[0x5D] = (uint8_t)((core_lba >> 8) & 0xFF);
    mbr_buf[0x5E] = (uint8_t)((core_lba >> 16) & 0xFF);
    mbr_buf[0x5F] = (uint8_t)((core_lba >> 24) & 0xFF);

    mbr_buf[0x40] = 0xFF;

    for (pi = 0; pi < 4; pi++) {
        entry_off = 446 + pi * 16;
        if (pi == boot_part_num - 1)
            mbr_buf[entry_off] = 0x80;
        else
            mbr_buf[entry_off] = 0x00;
    }

    mbr_buf[0x1FE] = 0x55;
    mbr_buf[0x1FF] = 0xAA;

    fd_disk = vfs_open(disk_dev, 2);
    if (fd_disk < 0) return -1;

    r = vfs_write_fd(fd_disk, mbr_buf, SECTOR_SIZE);
    if (r < SECTOR_SIZE) {
        vfs_close_fd(fd_disk);
        return -1;
    }

    fd_core = vfs_open("/boot/grub/i386-pc/core.img", 0);
    if (fd_core < 0) {
        vfs_close_fd(fd_disk);
        return -1;
    }

    lseek(fd_disk, (off_t)SECTOR_SIZE, SEEK_SET);

    off = 0;
    while ((r = vfs_read_fd(fd_core, core_buf, BUF_SIZE)) > 0) {
        if (vfs_write_fd(fd_disk, core_buf, r) != r) {
            vfs_close_fd(fd_core);
            vfs_close_fd(fd_disk);
            return -1;
        }
        off += r;
    }

    vfs_close_fd(fd_core);

    lseek(fd_disk, (off_t)SECTOR_SIZE, SEEK_SET);
    r = vfs_read_fd(fd_disk, verify_buf, SECTOR_SIZE);
    if (r >= SECTOR_SIZE) {
        fd_core = vfs_open("/boot/grub/i386-pc/core.img", 0);
        if (fd_core >= 0) {
            vfs_read_fd(fd_core, core_buf, SECTOR_SIZE);
            vfs_close_fd(fd_core);
            if (memcmp(verify_buf, core_buf, SECTOR_SIZE) != 0) {
                lseek(fd_disk, (off_t)SECTOR_SIZE, SEEK_SET);
                vfs_write_fd(fd_disk, core_buf, SECTOR_SIZE);
            }
        }
    }

    vfs_close_fd(fd_disk);
    return 0;
}

static int inst_write_grub_config(const char *mountpoint, const char *part_dev);

static int inst_install_boot(const char *mountpoint, const char *disk_dev, const char *part_dev, int part_num)
{
    char boot_dir[MAX_PATH];
    char grub_dir[MAX_PATH];
    char grub_mod_dir[MAX_PATH];

    snprintf(boot_dir, sizeof(boot_dir), "%s/boot", mountpoint);
    vfs_mkdir(boot_dir, 0755);
    snprintf(grub_dir, sizeof(grub_dir), "%s/boot/grub", mountpoint);
    vfs_mkdir(grub_dir, 0755);

    snprintf(grub_mod_dir, sizeof(grub_mod_dir), "%s/boot/grub/i386-pc", mountpoint);
    vfs_mkdir(grub_mod_dir, 0755);
    inst_copy_dir_recursive("/boot/grub/i386-pc", grub_mod_dir, NULL);

    if (inst_write_grub_config(mountpoint, part_dev) < 0)
        return -1;

    if (inst_install_grub_mbr(disk_dev, part_num) < 0) {
        return -1;
    }

    return 0;
}

static int inst_write_grub_config(const char *mountpoint, const char *part_dev)
{
    char grub_dir[MAX_PATH];
    char cfg_path[MAX_PATH];
    char grub_cfg[512];
    int fd;
    int written;

    snprintf(grub_cfg, sizeof(grub_cfg),
        "set timeout=5\n"
        "set default=0\n"
        "\n"
        "menuentry \"Lebirun\" {\n"
        "\tmultiboot2 /boot/lebirun.kernel root=%s\n"
        "\tboot\n"
        "}\n", part_dev);

    snprintf(grub_dir, sizeof(grub_dir), "%s/boot/grub", mountpoint);
    vfs_mkdir(grub_dir, 0755);

    snprintf(cfg_path, sizeof(cfg_path), "%s/boot/grub/grub.cfg", mountpoint);
    vfs_create(cfg_path, 0644);
    fd = vfs_open(cfg_path, 2);
    if (fd < 0) {
        fd = open(cfg_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (fd < 0) return -1;

    written = vfs_write_fd(fd, grub_cfg, strlen(grub_cfg));
    if (written < 0)
        written = write(fd, grub_cfg, strlen(grub_cfg));
    vfs_close_fd(fd);
    return (written < 0) ? -1 : 0;
}

#define SALT_LEN 8

static void inst_generate_salt(char *salt, int len)
{
    static const char charset[] =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789./";
    unsigned int ticks;
    unsigned int seed;
    int i;

    ticks = getticks();
    seed = ticks ^ (ticks >> 16) ^ (unsigned int)getpid();

    for (i = 0; i < len; i++) {
        seed = seed * 1103515245 + 12345;
        salt[i] = charset[(seed >> 16) % (sizeof(charset) - 1)];
    }
    salt[len] = '\0';
}

static const char *inst_hash_password(const char *password)
{
    static char salt[3 + SALT_LEN + 2];
    const char *hashed;

    memcpy(salt, "$5$", 3);
    inst_generate_salt(salt + 3, SALT_LEN);
    salt[3 + SALT_LEN] = '$';
    salt[3 + SALT_LEN + 1] = '\0';

    hashed = crypt(password, salt);
    return hashed;
}

static int inst_create_user(const char *mountpoint, const char *username, const char *password, int uid)
{
    char path[MAX_PATH];
    char line[256];
    int fd;
    int wlen;
    const char *hashed;

    snprintf(path, sizeof(path), "%s/home/%s", mountpoint, username);
    vfs_mkdir(path, 0755);

    snprintf(path, sizeof(path), "%s/etc/passwd", mountpoint);
    fd = vfs_open(path, 2);
    if (fd < 0) return -1;

    {
        uint64_t fsize;
        uint64_t ftype;
        vfs_stat(fd, &fsize, &ftype);
        lseek(fd, (off_t)fsize, SEEK_SET);
    }

    snprintf(line, sizeof(line), "%s:x:%d:%d:%s:/home/%s:/bin/lsh\n",
             username, uid, uid, username, username);
    wlen = (int)strlen(line);
    vfs_write_fd(fd, line, wlen);
    vfs_close_fd(fd);

    if (password[0] != '\0')
        hashed = inst_hash_password(password);
    else
        hashed = NULL;

    snprintf(path, sizeof(path), "%s/etc/shadow", mountpoint);
    fd = vfs_open(path, 2);
    if (fd < 0) {
        vfs_create(path, 0600);
        fd = vfs_open(path, 2);
        if (fd < 0) return -1;
    }

    {
        uint64_t fsize;
        uint64_t ftype;
        vfs_stat(fd, &fsize, &ftype);
        lseek(fd, (off_t)fsize, SEEK_SET);
    }

    snprintf(line, sizeof(line), "%s:%s:0:0:99999:7:::\n", username, hashed ? hashed : "!");
    wlen = (int)strlen(line);
    vfs_write_fd(fd, line, wlen);
    vfs_close_fd(fd);

    return 0;
}

static int inst_write_timezone(const char *mountpoint, const char *tz)
{
    char path[MAX_PATH];
    int fd;
    int len;

    snprintf(path, sizeof(path), "%s/etc/timezone", mountpoint);
    vfs_create(path, 0644);
    fd = vfs_open(path, 2);
    if (fd < 0) return -1;
    len = (int)strlen(tz);
    vfs_write_fd(fd, tz, len);
    vfs_write_fd(fd, "\n", 1);
    vfs_close_fd(fd);

    snprintf(path, sizeof(path), "%s/etc/environment", mountpoint);
    {
        char envline[128];
        vfs_create(path, 0644);
        fd = vfs_open(path, 2);
        if (fd < 0) return -1;
        snprintf(envline, sizeof(envline), "TZ=%s\n", tz);
        vfs_write_fd(fd, envline, (int)strlen(envline));
        vfs_close_fd(fd);
    }

    return 0;
}

static int inst_read_pkg_version_file(const char *path, char *ver, int versz)
{
    char buf[128];
    int fd;
    int n;
    char *p;
    char *end;

    ver[0] = '\0';
    fd = vfs_open(path, 0);
    if (fd < 0) return -1;
    n = vfs_read_fd(fd, buf, sizeof(buf) - 1);
    vfs_close_fd(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    p = strstr(buf, "Version:");
    if (p) {
        p += 8;
        while (*p == ' ' || *p == '\t') p++;
    } else {
        p = strstr(buf, "VERSION:");
        if (!p) return -1;
        p += 8;
    }
    end = p;
    while (*end && *end != '\n' && *end != '\r') end++;
    n = (int)(end - p);
    if (n <= 0 || n >= versz) return -1;
    memcpy(ver, p, n);
    ver[n] = '\0';
    return 0;
}

static void inst_read_pkg_version(const char *mountpoint, const char *pkgname, char *ver, int versz)
{
    char db_path[MAX_PATH];

    snprintf(db_path, sizeof(db_path), "%s%s/%s", mountpoint, LEBPKG_INSTALLED_DIR, pkgname);
    if (inst_read_pkg_version_file(db_path, ver, versz) < 0)
        ver[0] = '\0';
}

static int inst_compare_versions(const char *left, const char *right)
{
    const char *a;
    const char *b;
    unsigned long na;
    unsigned long nb;
    int az;
    int bz;
    unsigned char ca;
    unsigned char cb;

    a = left;
    b = right;
    while (*a || *b) {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            while (*a == '0') a++;
            while (*b == '0') b++;
            na = 0;
            nb = 0;
            az = 0;
            bz = 0;
            while (*a >= '0' && *a <= '9') {
                az++;
                if (na < 1000000000UL)
                    na = na * 10 + (unsigned long)(*a - '0');
                a++;
            }
            while (*b >= '0' && *b <= '9') {
                bz++;
                if (nb < 1000000000UL)
                    nb = nb * 10 + (unsigned long)(*b - '0');
                b++;
            }
            if (az != bz) return (az > bz) ? 1 : -1;
            if (na != nb) return (na > nb) ? 1 : -1;
            continue;
        }
        ca = (unsigned char)(*a ? *a : 0);
        cb = (unsigned char)(*b ? *b : 0);
        if (ca != cb) return (ca > cb) ? 1 : -1;
        if (*a) a++;
        if (*b) b++;
    }
    return 0;
}

static int inst_copy_pkg_db_entry(const char *mountpoint, const char *pkgname)
{
    char src_path[MAX_PATH];
    char db_dir[MAX_PATH];
    char dst_path[MAX_PATH];
    char buf[512];
    int src_fd;
    int dst_fd;
    int n;
    int written;
    int off;

    snprintf(src_path, sizeof(src_path), "%s/%s", LEBPKG_INSTALLED_DIR, pkgname);
    src_fd = vfs_open(src_path, 0);
    if (src_fd < 0) return -1;

    snprintf(db_dir, sizeof(db_dir), "%s/etc/lebpkg", mountpoint);
    vfs_mkdir(db_dir, 0755);
    snprintf(db_dir, sizeof(db_dir), "%s%s", mountpoint, LEBPKG_INSTALLED_DIR);
    vfs_mkdir(db_dir, 0755);
    snprintf(dst_path, sizeof(dst_path), "%s%s/%s", mountpoint, LEBPKG_INSTALLED_DIR, pkgname);
    vfs_unlink(dst_path);
    vfs_create(dst_path, 0644);
    dst_fd = vfs_open(dst_path, 2);
    if (dst_fd < 0) {
        vfs_close_fd(src_fd);
        return -1;
    }

    for (;;) {
        n = vfs_read_fd(src_fd, buf, sizeof(buf));
        if (n <= 0) break;
        off = 0;
        while (off < n) {
            written = vfs_write_fd(dst_fd, buf + off, n - off);
            if (written <= 0) {
                vfs_close_fd(src_fd);
                vfs_close_fd(dst_fd);
                return -1;
            }
            off += written;
        }
    }

    vfs_close_fd(src_fd);
    vfs_close_fd(dst_fd);
    return 0;
}

static int inst_seed_pkg_db_from_iso(const char *mountpoint)
{
    int fd;
    char name[256];
    unsigned int dtype;
    unsigned int idx;
    int copied;

    fd = vfs_open(LEBPKG_INSTALLED_DIR, 0);
    if (fd < 0) return 0;
    copied = 0;
    for (idx = 0; ; idx++) {
        if (vfs_readdir(fd, name, &dtype, idx) != 0) break;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (dtype != 1) continue;
        if (inst_copy_pkg_db_entry(mountpoint, name) == 0)
            copied++;
    }
    vfs_close_fd(fd);
    return copied;
}

static int inst_upgrade_pkg_db_from_iso(const char *mountpoint, int *kept)
{
    char inst_dir[MAX_PATH];
    char live_path[MAX_PATH];
    char target_path[MAX_PATH];
    char name[256];
    unsigned int dtype;
    unsigned int idx;
    int fd;
    int upgraded;
    char installed_ver[32];
    char live_ver[32];

    *kept = 0;
    snprintf(inst_dir, sizeof(inst_dir), "%s%s", mountpoint, LEBPKG_INSTALLED_DIR);
    fd = vfs_open(inst_dir, 0);
    if (fd < 0) return 0;
    upgraded = 0;
    for (idx = 0; ; idx++) {
        if (vfs_readdir(fd, name, &dtype, idx) != 0) break;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (dtype != 1) continue;
        snprintf(live_path, sizeof(live_path), "%s/%s", LEBPKG_INSTALLED_DIR, name);
        snprintf(target_path, sizeof(target_path), "%s%s/%s", mountpoint, LEBPKG_INSTALLED_DIR, name);
        if (inst_read_pkg_version_file(live_path, live_ver, sizeof(live_ver)) < 0)
            continue;
        if (inst_read_pkg_version_file(target_path, installed_ver, sizeof(installed_ver)) < 0)
            installed_ver[0] = '\0';
        if (installed_ver[0] == '\0' || inst_compare_versions(live_ver, installed_ver) > 0) {
            if (inst_copy_pkg_db_entry(mountpoint, name) == 0)
                upgraded++;
        } else {
            (*kept)++;
        }
    }
    vfs_close_fd(fd);
    return upgraded;
}

static void cleanup_exit(void)
{
    lebui_show_cursor();
    printf("\033[?1049l");
    fflush(stdout);
    lebui_raw_disable();
}

#define STEP_DISK    0
#define STEP_PART    1
#define STEP_FORMAT  2
#define STEP_PKGS    3
#define STEP_USER    4
#define STEP_ROOTPW  5
#define STEP_TZ      6
#define STEP_INSTALL 7
#define STEP_COUNT   8

#define STEP_NONE    0
#define STEP_DONE    1

static int step_disk(int *disk_idx)
{
    char *disk_items[MAX_DISKS];
    char disk_labels[MAX_DISKS][64];
    char sizebuf[32];
    int i;
    int choice;

    for (i = 0; i < disk_count; i++) {
        inst_format_size(disks[i].disk_sectors, sizebuf, sizeof(sizebuf));
        snprintf(disk_labels[i], sizeof(disk_labels[i]),
                 "%-10s  %8s  %d partition(s)",
                 disks[i].devpath, sizebuf, disks[i].part_count);
        disk_items[i] = disk_labels[i];
    }

    choice = lebui_menu_auto("Select Disk", (const char **)disk_items, disk_count,
                       " \x18\x19 Move  <Enter> Select  <Esc> Back", term_sz.rows, term_sz.cols);
    if (choice < 0) return -1;

    if (disks[choice].part_count == 0) {
        lebui_msgbox_auto("No Partitions",
                   "No partitions on this disk. Use ldiskutil first.", term_sz.rows, term_sz.cols);
        return -1;
    }

    *disk_idx = choice;
    return 0;
}

static int step_partition(int disk_idx, int *part_idx)
{
    disk_info_t *d;
    char *part_items[MAX_PARTS];
    char part_labels[MAX_PARTS][64];
    char sizebuf[32];
    int i;
    int choice;
    const char *type_name;

    d = &disks[disk_idx];
    for (i = 0; i < d->part_count; i++) {
        inst_format_size(d->parts[i].sector_count, sizebuf, sizeof(sizebuf));
        switch (d->parts[i].mbr_type) {
        case 0x83: type_name = "Linux"; break;
        case 0x82: type_name = "Swap"; break;
        case 0x0B: case 0x0C: type_name = "FAT32"; break;
        case 0x07: type_name = "NTFS"; break;
        default: type_name = "Other"; break;
        }
        snprintf(part_labels[i], sizeof(part_labels[i]),
                 "%-14s  %8s  %s",
                 d->parts[i].devpath, sizebuf, type_name);
        part_items[i] = part_labels[i];
    }

    choice = lebui_menu_auto("Select Partition", (const char **)part_items,
                       d->part_count,
                       " \x18\x19 Move  <Enter> Select  <Esc> Back", term_sz.rows, term_sz.cols);
    if (choice < 0) return -1;

    *part_idx = choice;
    return 0;
}

static int step_format(int *do_format)
{
    *do_format = lebui_confirm_auto("Format", "Format partition as ext4?", term_sz.rows, term_sz.cols);
    return 0;
}

#define MAX_USERS 8

typedef struct {
    char username[64];
    char password[64];
} user_entry_t;

static user_entry_t users[MAX_USERS];
static int user_count;
static char root_password[64];

static int step_packages(void)
{
    static const char *pkg_names[PKG_COUNT] = {
        "Core system (required)",
        "C development headers",
        "C development libraries"
    };
    int tmp[PKG_COUNT];
    int i;

    for (i = 0; i < PKG_COUNT; i++) tmp[i] = pkg_selected[i];
    tmp[PKG_CORE] = -1;

    if (lebui_checklist_auto("Package Selection", pkg_names, tmp, PKG_COUNT,
                      " <Space> Toggle  <Enter> Confirm  <Esc> Cancel", term_sz.rows, term_sz.cols) < 0)
        return -1;

    tmp[PKG_CORE] = 1;
    for (i = 0; i < PKG_COUNT; i++) pkg_selected[i] = tmp[i];
    return 0;
}

static int step_user_setup(void)
{
    char *menu_items[MAX_USERS + 2];
    char menu_labels[MAX_USERS + 2][48];
    int choice;
    int i;
    char password2[64];

    for (;;) {
        for (i = 0; i < user_count; i++) {
            snprintf(menu_labels[i], sizeof(menu_labels[i]), "  %s", users[i].username);
            menu_items[i] = menu_labels[i];
        }
        if (user_count < MAX_USERS) {
            snprintf(menu_labels[user_count], sizeof(menu_labels[user_count]), "  Add new user...");
            menu_items[user_count] = menu_labels[user_count];
        }
        snprintf(menu_labels[user_count + (user_count < MAX_USERS ? 1 : 0)],
                 sizeof(menu_labels[0]), "  Done");
        menu_items[user_count + (user_count < MAX_USERS ? 1 : 0)] =
            menu_labels[user_count + (user_count < MAX_USERS ? 1 : 0)];

        choice = lebui_menu_auto("User Accounts", (const char **)menu_items,
                           user_count + (user_count < MAX_USERS ? 2 : 1),
                           " \x18\x19 Move  <Enter> Select  <Esc> Back", term_sz.rows, term_sz.cols);
        if (choice < 0) return 0;

        if (choice == user_count + (user_count < MAX_USERS ? 1 : 0)) {
            return 0;
        }

        if (user_count < MAX_USERS && choice == user_count) {
            if (lebui_input_ex("New User", "Enter username:", users[user_count].username,
                          sizeof(users[user_count].username), 0, term_sz.rows, term_sz.cols) < 0)
                continue;
            if (users[user_count].username[0] == '\0') {
                lebui_msgbox_auto("Error", "Username cannot be empty.", term_sz.rows, term_sz.cols);
                continue;
            }
            for (;;) {
                if (lebui_input_ex("New User", "Enter password (empty=none):",
                              users[user_count].password,
                              sizeof(users[user_count].password), 1, term_sz.rows, term_sz.cols) < 0)
                    break;
                if (users[user_count].password[0] == '\0') {
                    user_count++;
                    break;
                }
                if (lebui_input_ex("New User", "Confirm password:",
                              password2, sizeof(password2), 1, term_sz.rows, term_sz.cols) < 0)
                    break;
                if (strcmp(users[user_count].password, password2) != 0) {
                    lebui_msgbox_auto("Error", "Passwords do not match.", term_sz.rows, term_sz.cols);
                    continue;
                }
                memset(password2, 0, sizeof(password2));
                user_count++;
                break;
            }
        } else if (choice < user_count) {
            if (lebui_confirm_auto("Remove User", users[choice].username, term_sz.rows, term_sz.cols)) {
                memset(users[choice].password, 0, sizeof(users[choice].password));
                for (i = choice; i < user_count - 1; i++)
                    users[i] = users[i + 1];
                user_count--;
            }
        }
    }
}

static int step_rootpw(void)
{
    char pw1[64];
    char pw2[64];

    for (;;) {
        if (lebui_input_ex("Root Password", "Enter root password (empty=none):", pw1, sizeof(pw1), 1, term_sz.rows, term_sz.cols) < 0)
            return -1;
        if (pw1[0] == '\0') {
            root_password[0] = '\0';
            return 0;
        }
        if (lebui_input_ex("Root Password", "Confirm root password:", pw2, sizeof(pw2), 1, term_sz.rows, term_sz.cols) < 0)
            return -1;
        if (strcmp(pw1, pw2) != 0) {
            lebui_msgbox_auto("Error", "Passwords do not match.", term_sz.rows, term_sz.cols);
            continue;
        }
        strncpy(root_password, pw1, sizeof(root_password) - 1);
        root_password[sizeof(root_password) - 1] = '\0';
        memset(pw1, 0, sizeof(pw1));
        memset(pw2, 0, sizeof(pw2));
        return 0;
    }
}

static int step_timezone(int *tz_idx)
{
    int tz_count;
    int choice;

    tz_count = 0;
    while (timezones[tz_count]) tz_count++;

    choice = lebui_menu_auto("Select Timezone", timezones, tz_count,
                       " \x18\x19 Move  <Enter> Select  <Esc> Back", term_sz.rows, term_sz.cols);
    if (choice < 0) return -1;

    *tz_idx = choice;
    return 0;
}

static void attach_tabbar(int active_tab, int cols);

static int step_do_install(int disk_idx, int part_idx, int do_format,
                           int tz_idx)
{
    disk_info_t *d;
    part_info_t *p;
    char mountpoint[MAX_PATH];
    char donemsg[128];
    char logbuf[64];
    char fmsg[128];
    int fret;
    int i;
    int seeded;

    d = &disks[disk_idx];
    p = &d->parts[part_idx];

    lebui_progress_reset(&prog_st);
    lebui_progress_init(&prog_st, "Installing", term_sz.rows, term_sz.cols);
    lebui_progress_update(&prog_st, "Preparing...", 0);
    usleep(50000);

    if (do_format) {
        lebui_progress_update(&prog_st, "Formatting partition...", 0);
        snprintf(fmsg, sizeof(fmsg), "Formatting %s as ext4...", p->devpath);
        lebui_progress_log(&prog_st, fmsg);
        fret = inst_format_ext4(p->devpath);
        if (fret != 0) {
            snprintf(fmsg, sizeof(fmsg), "Failed to format partition (status=%d, path=%s).", fret, p->devpath);
            lebui_msgbox_auto("Error", fmsg, term_sz.rows, term_sz.cols);
            return -1;
        }
        lebui_progress_log(&prog_st, "Format complete.");
    }

    snprintf(mountpoint, sizeof(mountpoint), "/tmp/lebinstall");
    vfs_mkdir("/tmp", 0755);
    vfs_mkdir(mountpoint, 0755);
    inst_umount_partition(mountpoint);

    lebui_progress_update(&prog_st, "Mounting partition...", 5);
    snprintf(logbuf, sizeof(logbuf), "Mounting %s...", p->devpath);
    lebui_progress_log(&prog_st, logbuf);
    if (inst_mount_partition(p->devpath, mountpoint) != 0) {
        lebui_msgbox_auto("Error", "Failed to mount partition.", term_sz.rows, term_sz.cols);
        return -1;
    }
    lebui_progress_log(&prog_st, "Partition mounted.");

    lebui_progress_update(&prog_st, "Counting files...", 10);
    lebui_progress_log(&prog_st, "Counting files to copy...");
    copy_total = inst_count_rootfs();
    if (copy_total < 1) copy_total = 100;
    copy_done = 0;
    copy_last_pct = -1;
    copy_overwrite_existing = 0;

    lebui_progress_log(&prog_st, "Copying rootfs...");
    if (inst_copy_rootfs(mountpoint) < 0) {
        lebui_progress_log(&prog_st, "Warning: some files could not be copied.");
    }
    copy_overwrite_existing = 1;
    lebui_progress_log(&prog_st, "Rootfs copy complete.");

    lebui_progress_update(&prog_st, "Installing bootloader...", 96);
    lebui_progress_log(&prog_st, "Installing GRUB bootloader...");
    if (inst_install_boot(mountpoint, d->devpath, p->devpath, p->number) < 0) {
        lebui_progress_log(&prog_st, "Warning: bootloader had errors.");
    } else {
        lebui_progress_log(&prog_st, "Bootloader installed.");
    }

    free(copy_buf);
    copy_buf = NULL;
    copy_buf_size = 0;

    for (i = 0; i < user_count; i++) {
        lebui_progress_update(&prog_st, "Creating user accounts...", 97);
        snprintf(logbuf, sizeof(logbuf), "Creating user: %s", users[i].username);
        lebui_progress_log(&prog_st, logbuf);
        inst_create_user(mountpoint, users[i].username, users[i].password, 1000 + i);
    }

    if (root_password[0] != '\0') {
        char shadow_path[MAX_PATH];
        char line[256];
        static char shadow_buf[4096];
        static char new_shadow[4096];
        const char *hashed;
        int shadow_fd;
        int rlen;
        int wlen;
        int new_len;
        int line_start;
        int line_end;
        int j;

        lebui_progress_update(&prog_st, "Setting root password...", 98);
        hashed = inst_hash_password(root_password);
        if (hashed) {
            snprintf(shadow_path, sizeof(shadow_path), "%s/etc/shadow", mountpoint);
            shadow_fd = vfs_open(shadow_path, 0);
            if (shadow_fd >= 0) {
                rlen = vfs_read_fd(shadow_fd, shadow_buf, sizeof(shadow_buf) - 1);
                vfs_close_fd(shadow_fd);
                if (rlen > 0) {
                    shadow_buf[rlen] = '\0';
                    new_len = 0;
                    j = 0;
                    while (j < rlen) {
                        line_start = j;
                        while (j < rlen && shadow_buf[j] != '\n') j++;
                        line_end = j;
                        if (j < rlen) j++;
                        if (line_end - line_start < (int)sizeof(line) - 1) {
                            memcpy(line, shadow_buf + line_start, line_end - line_start);
                            line[line_end - line_start] = '\0';
                        } else {
                            line[0] = '\0';
                        }
                        if (strncmp(line, "root:", 5) == 0) {
                            wlen = snprintf(new_shadow + new_len,
                                sizeof(new_shadow) - new_len,
                                "root:%s:0:0:99999:7:::\n", hashed);
                        } else {
                            wlen = snprintf(new_shadow + new_len,
                                sizeof(new_shadow) - new_len,
                                "%s\n", line);
                        }
                        new_len += wlen;
                        if (new_len >= (int)sizeof(new_shadow) - 2) break;
                    }
                    shadow_fd = vfs_open(shadow_path, 2);
                    if (shadow_fd >= 0) {
                        vfs_write_fd(shadow_fd, new_shadow, new_len);
                        vfs_close_fd(shadow_fd);
                    }
                }
            }
        }
        memset(root_password, 0, sizeof(root_password));
        lebui_progress_log(&prog_st, "Root password updated.");
    }

    lebui_progress_update(&prog_st, "Setting timezone...", 99);
    snprintf(logbuf, sizeof(logbuf), "Timezone: %s", tz_values[tz_idx]);
    lebui_progress_log(&prog_st, logbuf);
    inst_write_timezone(mountpoint, tz_values[tz_idx]);

    lebui_progress_update(&prog_st, "Writing package database...", 99);
    seeded = inst_seed_pkg_db_from_iso(mountpoint);
    if (seeded > 0) {
        snprintf(logbuf, sizeof(logbuf), "Package records copied: %d", seeded);
        lebui_progress_log(&prog_st, logbuf);
    } else {
        lebui_progress_log(&prog_st, "No ISO package records found.");
    }

    lebui_progress_log(&prog_st, "Unmounting...");
    if (inst_umount_partition(mountpoint) != 0) {
        lebui_progress_log(&prog_st, "Warning: unmount failed.");
    }

    lebui_progress_update(&prog_st, "Installation complete!", 100);
    lebui_progress_log(&prog_st, "Done!");

    snprintf(donemsg, sizeof(donemsg),
             "Lebirun installed to %s. Reboot to start.",
             p->devpath);
    lebui_msgbox_auto("Complete", donemsg, term_sz.rows, term_sz.cols);

    return 0;
}

#define UPSTEP_DISK    0
#define UPSTEP_PART    1
#define UPSTEP_ITEMS   2
#define UPSTEP_DO      3
#define UPSTEP_COUNT   4

#define UPD_CORE       0
#define UPD_BOOT       1
#define UPD_USR_INC    2
#define UPD_TERMINFO   3
#define UPD_PKGDB      4
#define UPD_GRUB_CODE  5
#define UPD_GRUB_CFG   6
#define UPD_COUNT      7

static int upd_selected[UPD_COUNT] = { 1, 1, 1, 1, 1, 1, 0 };

static const char *upd_preserve_paths[] = {
    "/home",
    "/root",
    "/etc/passwd",
    "/etc/shadow",
    "/etc/hostname",
    "/etc/timezone",
    "/etc/environment",
    "/etc/lebpkg",
    NULL
};

static int upd_path_is_preserved(const char *dst_path, const char *mountpoint)
{
    char full[MAX_PATH];
    int i;

    for (i = 0; upd_preserve_paths[i]; i++) {
        snprintf(full, sizeof(full), "%s%s", mountpoint, upd_preserve_paths[i]);
        if (strcmp(dst_path, full) == 0) return 1;
        if (strncmp(dst_path, full, strlen(full)) == 0 &&
            dst_path[strlen(full)] == '/') return 1;
    }

    if (!upd_selected[UPD_BOOT]) {
        snprintf(full, sizeof(full), "%s/boot", mountpoint);
        if (strcmp(dst_path, full) == 0) return 1;
        if (strncmp(dst_path, full, strlen(full)) == 0 &&
            dst_path[strlen(full)] == '/') return 1;
    }

    if (!upd_selected[UPD_GRUB_CODE]) {
        snprintf(full, sizeof(full), "%s/boot/grub/i386-pc", mountpoint);
        if (strcmp(dst_path, full) == 0) return 1;
        if (strncmp(dst_path, full, strlen(full)) == 0 &&
            dst_path[strlen(full)] == '/') return 1;
    }

    if (!upd_selected[UPD_GRUB_CFG]) {
        snprintf(full, sizeof(full), "%s/boot/grub/grub.cfg", mountpoint);
        if (strcmp(dst_path, full) == 0) return 1;
    }

    return 0;
}

static int inst_update_dir_recursive(const char *src, const char *dst, const char *mountpoint)
{
    int fd;
    int dst_fd;
    char name[256];
    unsigned int type;
    unsigned int idx;
    char src_path[MAX_PATH];
    char dst_path[MAX_PATH];
    int errors;
    int slen;
    int dlen;

    vfs_mkdir(dst, 0755);

    dst_fd = vfs_open(dst, 0);
    fd = vfs_open(src, 0);
    if (fd < 0) {
        if (dst_fd >= 0) {
            vfs_close_fd(dst_fd);
        }
        return -1;
    }

    errors = 0;
    for (idx = 0; ; idx++) {
        if (vfs_readdir(fd, name, &type, idx) != 0) break;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        slen = snprintf(src_path, sizeof(src_path), "%s/%s", src, name);
        if (slen < 0 || slen >= (int)sizeof(src_path)) {
            errors++;
            continue;
        }
        dlen = snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, name);
        if (dlen < 0 || dlen >= (int)sizeof(dst_path)) {
            errors++;
            continue;
        }

        if (strcmp(name, "lebinstaller") == 0) continue;

        if (upd_path_is_preserved(dst_path, mountpoint)) continue;

        if (type == 2) {
            if (inst_update_dir_recursive(src_path, dst_path, mountpoint) < 0)
                errors++;
        } else if (type == 6) {
            inst_copy_current(src_path);
            if (inst_copy_symlink_vfs(src_path, dst_path) < 0)
                errors++;
            copy_done++;
            inst_copy_progress(src_path);
        } else {
            inst_copy_current(src_path);
            if (inst_copy_file_vfs(src_path, dst_path) < 0)
                errors++;
            copy_done++;
            inst_copy_progress(src_path);
        }
    }
    vfs_close_fd(fd);
    if (dst_fd >= 0) {
        vfs_close_fd(dst_fd);
    }
    return (errors > 0) ? -1 : 0;
}

static int step_update_items(void)
{
    static const char *upd_names[UPD_COUNT] = {
        "Core system files (/bin, /lib, /sbin, /init)",
        "Kernel and boot files (/boot, preserving GRUB config)",
        "Development headers (/usr/include)",
        "Terminal database (/usr/share/terminfo)",
        "Package database records",
        "GRUB boot code and modules",
        "GRUB configuration file"
    };
    int tmp[UPD_COUNT];
    int i;

    for (i = 0; i < UPD_COUNT; i++) tmp[i] = upd_selected[i];
    if (lebui_checklist_auto("Update Selection", upd_names, tmp, UPD_COUNT,
                      " <Space> Toggle  <Enter> Confirm  <Esc> Cancel", term_sz.rows, term_sz.cols) < 0)
        return -1;
    for (i = 0; i < UPD_COUNT; i++) upd_selected[i] = tmp[i] ? 1 : 0;
    return 0;
}

static int step_do_update(int disk_idx, int part_idx)
{
    disk_info_t *d;
    part_info_t *p;
    char mountpoint[MAX_PATH];
    char logbuf[64];
    char donemsg[256];
    char old_ver[32];
    char new_ver[32];
    char path[MAX_PATH];
    char src[MAX_PATH];
    char dst[MAX_PATH];
    static const char *core_dirs[] = {
        "bin", "lib", "sbin", NULL
    };
    int i;
    int upgraded_pkgs;
    int kept_pkgs;
    int did_work;

    d = &disks[disk_idx];
    p = &d->parts[part_idx];
    did_work = 0;

    lebui_progress_reset(&prog_st);
    lebui_progress_init(&prog_st, "Updating", term_sz.rows, term_sz.cols);
    lebui_progress_update(&prog_st, "Preparing...", 0);

    snprintf(mountpoint, sizeof(mountpoint), "/tmp/lebupdate");
    vfs_mkdir("/tmp", 0755);
    vfs_mkdir(mountpoint, 0755);
    inst_umount_partition(mountpoint);

    lebui_progress_update(&prog_st, "Mounting partition...", 5);
    snprintf(logbuf, sizeof(logbuf), "Mounting %s...", p->devpath);
    lebui_progress_log(&prog_st, logbuf);
    if (inst_mount_partition(p->devpath, mountpoint) != 0) {
        lebui_msgbox_auto("Error", "Failed to mount partition.", term_sz.rows, term_sz.cols);
        return -1;
    }
    lebui_progress_log(&prog_st, "Partition mounted.");

    old_ver[0] = '\0';
    inst_read_pkg_version(mountpoint, "lebirun-base", old_ver, sizeof(old_ver));
    new_ver[0] = '\0';
    inst_read_pkg_version_file(LEBPKG_INSTALLED_DIR "/lebirun-base", new_ver, sizeof(new_ver));

    lebui_progress_update(&prog_st, "Counting files...", 10);
    copy_total = 0;
    if (upd_selected[UPD_CORE]) {
        copy_total++;
        for (i = 0; core_dirs[i]; i++) {
            snprintf(path, sizeof(path), "/%s", core_dirs[i]);
            copy_total += inst_count_dir_entries(path);
        }
    }
    if (upd_selected[UPD_BOOT]) {
        copy_total += inst_count_dir_entries("/boot");
    }
    if (upd_selected[UPD_USR_INC]) {
        copy_total += inst_count_dir_entries("/usr/include");
    }
    if (upd_selected[UPD_TERMINFO]) {
        copy_total += inst_count_dir_entries("/usr/share/terminfo");
    }
    if (copy_total < 1) copy_total = 100;
    copy_done = 0;
    copy_last_pct = -1;

    if (upd_selected[UPD_CORE]) {
        lebui_progress_log(&prog_st, "Updating core system files...");
        snprintf(src, sizeof(src), "/init");
        snprintf(dst, sizeof(dst), "%s/init", mountpoint);
        if (inst_copy_file_vfs(src, dst) == 0) {
            copy_done++;
        }
        for (i = 0; core_dirs[i]; i++) {
            snprintf(src, sizeof(src), "/%s", core_dirs[i]);
            snprintf(dst, sizeof(dst), "%s/%s", mountpoint, core_dirs[i]);
            inst_update_dir_recursive(src, dst, mountpoint);
        }
        did_work = 1;
    }

    if (upd_selected[UPD_BOOT]) {
        lebui_progress_log(&prog_st, "Updating boot files...");
        snprintf(src, sizeof(src), "/boot");
        snprintf(dst, sizeof(dst), "%s/boot", mountpoint);
        inst_update_dir_recursive(src, dst, mountpoint);
        did_work = 1;
    }

    if (upd_selected[UPD_USR_INC]) {
        lebui_progress_log(&prog_st, "Updating development headers...");
        snprintf(src, sizeof(src), "/usr/include");
        snprintf(dst, sizeof(dst), "%s/usr/include", mountpoint);
        inst_update_dir_recursive(src, dst, mountpoint);
        did_work = 1;
    }

    if (upd_selected[UPD_TERMINFO]) {
        lebui_progress_log(&prog_st, "Updating terminal database...");
        snprintf(src, sizeof(src), "/usr/share/terminfo");
        snprintf(dst, sizeof(dst), "%s/usr/share/terminfo", mountpoint);
        inst_update_dir_recursive(src, dst, mountpoint);
        did_work = 1;
    }

    if (did_work) {
        lebui_progress_log(&prog_st, "Selected files updated.");
    }

    if (upd_selected[UPD_GRUB_CODE]) {
        lebui_progress_update(&prog_st, "Updating bootloader...", 96);
        lebui_progress_log(&prog_st, "Updating GRUB boot code and modules...");
        snprintf(dst, sizeof(dst), "%s/boot/grub/i386-pc", mountpoint);
        vfs_mkdir(dst, 0755);
        inst_copy_dir_recursive("/boot/grub/i386-pc", dst, NULL);
        if (inst_install_grub_mbr(d->devpath, p->number) < 0)
            lebui_progress_log(&prog_st, "Warning: GRUB boot code update failed.");
        else
            lebui_progress_log(&prog_st, "GRUB boot code updated.");
    }

    if (upd_selected[UPD_GRUB_CFG]) {
        lebui_progress_update(&prog_st, "Updating GRUB config...", 97);
        lebui_progress_log(&prog_st, "Updating GRUB configuration...");
        if (inst_write_grub_config(mountpoint, p->devpath) < 0)
            lebui_progress_log(&prog_st, "Warning: GRUB config update failed.");
        else
            lebui_progress_log(&prog_st, "GRUB configuration updated.");
    }

    free(copy_buf);
    copy_buf = NULL;
    copy_buf_size = 0;

    if (upd_selected[UPD_PKGDB]) {
        lebui_progress_update(&prog_st, "Updating package database...", 98);
        lebui_progress_log(&prog_st, "Updating package database...");
        upgraded_pkgs = inst_upgrade_pkg_db_from_iso(mountpoint, &kept_pkgs);
        snprintf(logbuf, sizeof(logbuf), "Package records upgraded: %d", upgraded_pkgs);
        lebui_progress_log(&prog_st, logbuf);
        snprintf(logbuf, sizeof(logbuf), "Package records kept: %d", kept_pkgs);
        lebui_progress_log(&prog_st, logbuf);
    }

    lebui_progress_log(&prog_st, "Unmounting...");
    if (inst_umount_partition(mountpoint) != 0) {
        lebui_progress_log(&prog_st, "Warning: unmount failed.");
    }

    lebui_progress_update(&prog_st, "Update complete!", 100);
    lebui_progress_log(&prog_st, "Done!");

    if (old_ver[0] != '\0' && new_ver[0] != '\0' && strcmp(old_ver, new_ver) != 0)
        snprintf(donemsg, sizeof(donemsg),
                 "Lebirun updated on %s from %s to %s. Reboot to apply changes.",
                 p->devpath, old_ver, new_ver);
    else if (new_ver[0] != '\0')
        snprintf(donemsg, sizeof(donemsg),
                 "Lebirun updated on %s to version %s. Reboot to apply changes.",
                 p->devpath, new_ver);
    else
        snprintf(donemsg, sizeof(donemsg),
                 "Lebirun updated on %s. Reboot to apply changes.",
                 p->devpath);
    lebui_msgbox_auto("Complete", donemsg, term_sz.rows, term_sz.cols);
    return 0;
}

static const char *g_tab_names[] = { "Install", "Update" };

static void attach_tabbar(int active_tab, int cols)
{
    lebui_tabbar_attach(g_tab_names, 2, active_tab, cols);
}

static int run_install_page(void)
{
    static const char *step_names[] = {
        "Select Disk",
        "Select Partition",
        "Format Partition",
        "Package Selection",
        "User Accounts",
        "Root Password",
        "Select Timezone",
        "Install"
    };
    int status[STEP_COUNT];
    char labels[STEP_COUNT][56];
    char *items[STEP_COUNT];
    int disk_idx;
    int part_idx;
    int do_format;
    int tz_idx;
    int sel;
    int i;
    int ret;
    char ubuf[32];

    for (i = 0; i < STEP_COUNT; i++) status[i] = STEP_NONE;
    disk_idx = -1;
    part_idx = -1;
    do_format = 0;
    tz_idx = 0;
    user_count = 0;
    root_password[0] = '\0';

    for (;;) {
        attach_tabbar(0, term_sz.cols);

        for (i = 0; i < STEP_COUNT; i++) {
            if (i == STEP_INSTALL) {
                if (status[STEP_DISK] == STEP_DONE && status[STEP_PART] == STEP_DONE)
                    snprintf(labels[i], sizeof(labels[i]), "  [>] %s", step_names[i]);
                else
                    snprintf(labels[i], sizeof(labels[i]), "  [ ] %s  (need disk & partition)", step_names[i]);
            } else if (status[i] == STEP_DONE) {
                if (i == STEP_DISK)
                    snprintf(labels[i], sizeof(labels[i]), "  [*] %s  (%s)", step_names[i], disks[disk_idx].devpath);
                else if (i == STEP_PART)
                    snprintf(labels[i], sizeof(labels[i]), "  [*] %s  (%s)", step_names[i], disks[disk_idx].parts[part_idx].devpath);
                else if (i == STEP_FORMAT)
                    snprintf(labels[i], sizeof(labels[i]), "  [*] %s  (%s)", step_names[i], do_format ? "ext4" : "No");
                else if (i == STEP_PKGS) {
                    int npkg;
                    npkg = pkg_selected[PKG_C_HDR] + pkg_selected[PKG_C_LIB];
                    snprintf(labels[i], sizeof(labels[i]), "  [*] %s  (core +%d)", step_names[i], npkg);
                } else if (i == STEP_USER) {
                    snprintf(ubuf, sizeof(ubuf), "%d user(s)", user_count);
                    snprintf(labels[i], sizeof(labels[i]), "  [*] %s  (%s)", step_names[i], ubuf);
                } else if (i == STEP_TZ)
                    snprintf(labels[i], sizeof(labels[i]), "  [*] %s  (%s)", step_names[i], timezones[tz_idx]);
                else if (i == STEP_ROOTPW)
                    snprintf(labels[i], sizeof(labels[i]), "  [*] %s  (set)", step_names[i]);
                else
                    snprintf(labels[i], sizeof(labels[i]), "  [*] %s", step_names[i]);
            } else {
                if (i == STEP_FORMAT || i == STEP_USER || i == STEP_TZ || i == STEP_ROOTPW || i == STEP_PKGS)
                    snprintf(labels[i], sizeof(labels[i]), "  [ ] %s  (optional)", step_names[i]);
                else
                    snprintf(labels[i], sizeof(labels[i]), "  [ ] %s", step_names[i]);
            }
            items[i] = labels[i];
        }

        sel = lebui_menu_auto("Installation Steps", (const char **)items, STEP_COUNT,
                        " \x18\x19 Move  <Enter> Select  <Esc> Quit", term_sz.rows, term_sz.cols);

        if (sel == LEBUI_KEY_TAB) return LEBUI_KEY_TAB;

        if (sel < 0) {
            if (lebui_confirm_auto("Quit", "Exit the installer?", term_sz.rows, term_sz.cols)) {
                return -1;
            }
            continue;
        }

        switch (sel) {
        case STEP_DISK:
            ret = step_disk(&disk_idx);
            if (ret == 0) {
                status[STEP_DISK] = STEP_DONE;
                status[STEP_PART] = STEP_NONE;
                part_idx = -1;
            }
            break;

        case STEP_PART:
            if (status[STEP_DISK] != STEP_DONE) {
                lebui_msgbox_auto("Error", "Select a disk first.", term_sz.rows, term_sz.cols);
                break;
            }
            ret = step_partition(disk_idx, &part_idx);
            if (ret == 0)
                status[STEP_PART] = STEP_DONE;
            break;

        case STEP_FORMAT:
            ret = step_format(&do_format);
            if (ret == 0)
                status[STEP_FORMAT] = STEP_DONE;
            break;

        case STEP_PKGS:
            ret = step_packages();
            if (ret == 0)
                status[STEP_PKGS] = STEP_DONE;
            break;

        case STEP_USER:
            step_user_setup();
            if (user_count > 0)
                status[STEP_USER] = STEP_DONE;
            else
                status[STEP_USER] = STEP_NONE;
            break;

        case STEP_ROOTPW:
            ret = step_rootpw();
            if (ret == 0)
                status[STEP_ROOTPW] = STEP_DONE;
            break;

        case STEP_TZ:
            ret = step_timezone(&tz_idx);
            if (ret == 0)
                status[STEP_TZ] = STEP_DONE;
            break;

        case STEP_INSTALL:
            if (status[STEP_DISK] != STEP_DONE || status[STEP_PART] != STEP_DONE) {
                lebui_msgbox_auto("Error", "Select a disk and partition first.", term_sz.rows, term_sz.cols);
                break;
            }
            if (!lebui_confirm_auto("Confirm", "Proceed with installation?", term_sz.rows, term_sz.cols))
                break;
            ret = step_do_install(disk_idx, part_idx, do_format, tz_idx);
            if (ret == 0) {
                for (i = 0; i < user_count; i++)
                    memset(users[i].password, 0, sizeof(users[i].password));
                memset(root_password, 0, sizeof(root_password));
                return 0;
            }
            break;
        }
    }
}

static int run_update_page(void)
{
    static const char *upd_step_names[] = {
        "Select Disk",
        "Select Partition",
        "Update Selection",
        "Update"
    };
    int status[UPSTEP_COUNT];
    char labels[UPSTEP_COUNT][56];
    char *items[UPSTEP_COUNT];
    int disk_idx;
    int part_idx;
    int sel;
    int i;
    int ret;
    int selected_count;

    for (i = 0; i < UPSTEP_COUNT; i++) status[i] = STEP_NONE;
    disk_idx = -1;
    part_idx = -1;

    for (;;) {
        attach_tabbar(1, term_sz.cols);

        selected_count = 0;
        for (i = 0; i < UPD_COUNT; i++) {
            if (upd_selected[i]) selected_count++;
        }

        for (i = 0; i < UPSTEP_COUNT; i++) {
            if (i == UPSTEP_DO) {
                if (status[UPSTEP_DISK] == STEP_DONE && status[UPSTEP_PART] == STEP_DONE)
                    snprintf(labels[i], sizeof(labels[i]), "  [>] %s", upd_step_names[i]);
                else
                    snprintf(labels[i], sizeof(labels[i]), "  [ ] %s  (need disk & partition)", upd_step_names[i]);
            } else if (status[i] == STEP_DONE) {
                if (i == UPSTEP_DISK)
                    snprintf(labels[i], sizeof(labels[i]), "  [*] %s  (%s)", upd_step_names[i], disks[disk_idx].devpath);
                else if (i == UPSTEP_PART)
                    snprintf(labels[i], sizeof(labels[i]), "  [*] %s  (%s)", upd_step_names[i], disks[disk_idx].parts[part_idx].devpath);
                else if (i == UPSTEP_ITEMS)
                    snprintf(labels[i], sizeof(labels[i]), "  [*] %s  (%d item(s))", upd_step_names[i], selected_count);
                else
                    snprintf(labels[i], sizeof(labels[i]), "  [*] %s", upd_step_names[i]);
            } else {
                if (i == UPSTEP_ITEMS)
                    snprintf(labels[i], sizeof(labels[i]), "  [ ] %s  (%d default item(s))", upd_step_names[i], selected_count);
                else
                    snprintf(labels[i], sizeof(labels[i]), "  [ ] %s", upd_step_names[i]);
            }
            items[i] = labels[i];
        }

        sel = lebui_menu_auto("Update Steps", (const char **)items, UPSTEP_COUNT,
                        " \x18\x19 Move  <Enter> Select  <Tab> Switch  <Esc> Quit", term_sz.rows, term_sz.cols);

        if (sel == LEBUI_KEY_TAB) return LEBUI_KEY_TAB;

        if (sel < 0) {
            if (lebui_confirm_auto("Quit", "Exit the installer?", term_sz.rows, term_sz.cols)) {
                return -1;
            }
            continue;
        }

        switch (sel) {
        case UPSTEP_DISK:
            ret = step_disk(&disk_idx);
            if (ret == 0) {
                status[UPSTEP_DISK] = STEP_DONE;
                status[UPSTEP_PART] = STEP_NONE;
                part_idx = -1;
            }
            break;

        case UPSTEP_PART:
            if (status[UPSTEP_DISK] != STEP_DONE) {
                lebui_msgbox_auto("Error", "Select a disk first.", term_sz.rows, term_sz.cols);
                break;
            }
            ret = step_partition(disk_idx, &part_idx);
            if (ret == 0)
                status[UPSTEP_PART] = STEP_DONE;
            break;

        case UPSTEP_ITEMS:
            ret = step_update_items();
            if (ret == 0)
                status[UPSTEP_ITEMS] = STEP_DONE;
            break;

        case UPSTEP_DO:
            if (status[UPSTEP_DISK] != STEP_DONE || status[UPSTEP_PART] != STEP_DONE) {
                lebui_msgbox_auto("Error", "Select a disk and partition first.", term_sz.rows, term_sz.cols);
                break;
            }
            selected_count = 0;
            for (i = 0; i < UPD_COUNT; i++) {
                if (upd_selected[i]) selected_count++;
            }
            if (selected_count == 0) {
                lebui_msgbox_auto("Error", "Select at least one update item.", term_sz.rows, term_sz.cols);
                break;
            }
            if (!lebui_confirm_auto("Confirm",
                    "Update selected items? User data and local config will be preserved.",
                    term_sz.rows, term_sz.cols))
                break;
            ret = step_do_update(disk_idx, part_idx);
            if (ret == 0)
                return 0;
            break;
        }
    }
}

int main(int argc, char **argv)
{
    int active_page;
    int ret;

    (void)argc;
    (void)argv;

    setvbuf(stdout, NULL, _IOFBF, 8192);

    lebui_get_size(&term_sz);
    lebui_raw_enable();
    printf("\033[?1049h");
    fflush(stdout);
    lebui_hide_cursor();

    if (getuid() != 0) {
        lebui_msgbox_auto("Error", "This installer must be run as root.", term_sz.rows, term_sz.cols);
        cleanup_exit();
        return 1;
    }

    lebui_progress_reset(&prog_st);
    lebui_progress_init(&prog_st, "Scanning", term_sz.rows, term_sz.cols);
    lebui_progress_update(&prog_st, "Scanning for disks...", 0);

    if (inst_enumerate_disks() <= 0) {
        lebui_msgbox_auto("Error", "No disks found.", term_sz.rows, term_sz.cols);
        cleanup_exit();
        return 1;
    }

    active_page = 0;

    for (;;) {
        lebui_clear();
        if (active_page == 0) {
            ret = run_install_page();
        } else {
            ret = run_update_page();
        }

        if (ret == LEBUI_KEY_TAB) {
            active_page = 1 - active_page;
            continue;
        }

        cleanup_exit();
        lebui_tabbar_detach();
        return (ret == 0) ? 0 : 0;
    }
}
