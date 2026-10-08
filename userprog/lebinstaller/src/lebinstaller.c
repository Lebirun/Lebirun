#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <crypt.h>
#include <lebirun.h>
#include <lebirun/syscall.h>
#include <lebui.h>

#define SECTOR_SIZE  512
#define BUF_SIZE     4096
#define MAX_PATH     256
#define MAX_LINE     128
#define LEBPKG_INSTALLED_DIR "/etc/lebpkg/installed"
#define MBR_SIG      0xAA55

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
    int      part_capacity;
    part_info_t *parts;
} disk_info_t;

static disk_info_t *disks;
static int disk_count;
static int disk_capacity;
static lebui_size_t term_sz;
static char boot_error[128];

#define BOOT_BIOS 0
#define BOOT_UEFI 1
#define BOOT_BOTH 2

static int boot_mode;
static char boot_esp[32];

static void wiz_prog_init(const char *title);
static void wiz_prog_update(const char *msg, int pct);
static void wiz_prog_log(const char *msg);

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

    if (count > UINT32_MAX / SECTOR_SIZE) { vfs_close_fd(fd); return -1; }
    total = count * SECTOR_SIZE;
    if ((uint64_t)lba * SECTOR_SIZE > (uint64_t)INT64_MAX) { vfs_close_fd(fd); return -1; }
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

static int inst_reserve_disks(int need)
{
    disk_info_t *new_disks;
    int new_cap;
    int i;

    if (disk_capacity >= need) return 0;

    new_cap = disk_capacity ? disk_capacity * 2 : 4;
    while (new_cap < need) new_cap *= 2;

    new_disks = (disk_info_t *)realloc(disks, (size_t)new_cap * sizeof(disk_info_t));
    if (!new_disks) return -1;

    disks = new_disks;
    for (i = disk_capacity; i < new_cap; i++)
        memset(&disks[i], 0, sizeof(disks[i]));
    disk_capacity = new_cap;
    return 0;
}

static int inst_reserve_parts(disk_info_t *disk, int need)
{
    part_info_t *new_parts;
    int new_cap;
    int i;

    if (disk->part_capacity >= need) return 0;

    new_cap = disk->part_capacity ? disk->part_capacity * 2 : 4;
    while (new_cap < need) new_cap *= 2;

    new_parts = (part_info_t *)realloc(disk->parts, (size_t)new_cap * sizeof(part_info_t));
    if (!new_parts) return -1;

    disk->parts = new_parts;
    for (i = disk->part_capacity; i < new_cap; i++)
        memset(&disk->parts[i], 0, sizeof(disk->parts[i]));
    disk->part_capacity = new_cap;
    return 0;
}

static part_info_t *inst_add_part(disk_info_t *disk)
{
    part_info_t *part;

    if (inst_reserve_parts(disk, disk->part_count + 1) < 0) return NULL;

    part = &disk->parts[disk->part_count];
    memset(part, 0, sizeof(*part));
    disk->part_count++;
    return part;
}

static int inst_guid_is_zero(const uint8_t *g)
{
    int i;

    for (i = 0; i < 16; i++) {
        if (g[i]) return 0;
    }
    return 1;
}

static uint8_t inst_guid_to_mbr(const uint8_t *g)
{
    static const uint8_t esp[16] = {
        0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
    };
    static const uint8_t linux_fs[16] = {
        0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
        0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
    };
    static const uint8_t swap[16] = {
        0x6D, 0xFD, 0x57, 0x06, 0xAB, 0xA4, 0xC4, 0x43,
        0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F
    };
    static const uint8_t basic_data[16] = {
        0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
        0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
    };

    if (memcmp(g, esp, 16) == 0) return 0xEF;
    if (memcmp(g, linux_fs, 16) == 0) return 0x83;
    if (memcmp(g, swap, 16) == 0) return 0x82;
    if (memcmp(g, basic_data, 16) == 0) return 0x07;
    return 0x00;
}

static int inst_scan_gpt(disk_info_t *disk)
{
    uint8_t hdr[SECTOR_SIZE];
    uint8_t *entries;
    uint64_t entries_lba;
    uint64_t num_entries;
    uint64_t entry_size;
    uint64_t entries_bytes;
    uint64_t entries_sectors;
    uint64_t i;
    int ret;

    ret = inst_disk_read(disk->devpath, 0, 1, hdr);
    if (ret < SECTOR_SIZE) return -1;
    if (hdr[510] != 0x55 || hdr[511] != 0xAA) return -1;
    ret = inst_disk_read(disk->devpath, 1, 1, hdr);
    if (ret < SECTOR_SIZE) return -1;
    if (memcmp(hdr, "EFI PART", 8) != 0) return -1;
    entries_lba = (uint64_t)hdr[72] | ((uint64_t)hdr[73] << 8) |
                  ((uint64_t)hdr[74] << 16) | ((uint64_t)hdr[75] << 24) |
                  ((uint64_t)hdr[76] << 32) | ((uint64_t)hdr[77] << 40) |
                  ((uint64_t)hdr[78] << 48) | ((uint64_t)hdr[79] << 56);
    num_entries = (uint64_t)hdr[80] | ((uint64_t)hdr[81] << 8) |
                  ((uint64_t)hdr[82] << 16) | ((uint64_t)hdr[83] << 24);
    entry_size = (uint64_t)hdr[84] | ((uint64_t)hdr[85] << 8) |
                 ((uint64_t)hdr[86] << 16) | ((uint64_t)hdr[87] << 24);
    if (!num_entries || entry_size < 128) return -1;
    if (num_entries > 1024) num_entries = 1024;
    if (entry_size > 512) return -1;
    entries_bytes = num_entries * entry_size;
    entries_sectors = (entries_bytes + SECTOR_SIZE - 1) / SECTOR_SIZE;
    if (!entries_lba || entries_lba >= disk->disk_sectors) return -1;
    if (entries_sectors > disk->disk_sectors - entries_lba) return -1;
    if (entries_lba > 0xFFFFFFFFu || entries_sectors > 0xFFFFFFFFu)
        return -1;
    entries = (uint8_t *)malloc((size_t)(entries_sectors * SECTOR_SIZE));
    if (!entries) return -1;
    ret = inst_disk_read(disk->devpath, (uint32_t)entries_lba,
                         (uint32_t)entries_sectors, entries);
    if ((uint64_t)ret < entries_sectors * SECTOR_SIZE) {
        free(entries);
        return -1;
    }
    disk->part_count = 0;
    for (i = 0; i < num_entries; i++) {
        uint8_t *e = entries + i * entry_size;
        uint64_t start;
        uint64_t end;
        part_info_t *part;

        if (inst_guid_is_zero(e)) continue;
        start = (uint64_t)e[32] | ((uint64_t)e[33] << 8) |
                ((uint64_t)e[34] << 16) | ((uint64_t)e[35] << 24) |
                ((uint64_t)e[36] << 32) | ((uint64_t)e[37] << 40) |
                ((uint64_t)e[38] << 48) | ((uint64_t)e[39] << 56);
        end = (uint64_t)e[40] | ((uint64_t)e[41] << 8) |
              ((uint64_t)e[42] << 16) | ((uint64_t)e[43] << 24) |
              ((uint64_t)e[44] << 32) | ((uint64_t)e[45] << 40) |
              ((uint64_t)e[46] << 48) | ((uint64_t)e[47] << 56);
        if (!start || !end || end < start) continue;
        if (start >= disk->disk_sectors) continue;
        if (end - start + 1 > disk->disk_sectors - start) continue;
        part = inst_add_part(disk);
        if (!part) break;
        part->valid = 1;
        part->number = (int)i + 1;
        part->start_lba = start;
        part->sector_count = end - start + 1;
        part->mbr_type = inst_guid_to_mbr(e);
        snprintf(part->devpath, sizeof(part->devpath), "%s%d", disk->devpath,
                 (int)i + 1);
    }
    free(entries);
    return 0;
}

static void inst_scan_disk(disk_info_t *disk)
{
    uint8_t sector0[SECTOR_SIZE];
    mbr_t *mbr;
    part_info_t *part;
    int ret;
    int i;
    int stat_fd;
    int devfd;
    char name[64];
    unsigned int dtype;
    unsigned int didx;
    int dlen;
    int part_number;
    int digit;
    char partpath[64];
    const char *suffix;
    uint64_t psize;
    uint64_t ptype;
    uint64_t stat_size;
    uint64_t stat_type;

    disk->part_count = 0;
    disk->disk_sectors = 0;

    stat_fd = vfs_open(disk->devpath, 0);
    if (stat_fd >= 0) {
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
                part = inst_add_part(disk);
                if (!part) return;
                part->valid = 1;
                part->number = i + 1;
                part->start_lba = mbr->parts[i].lba_start;
                part->sector_count = mbr->parts[i].sector_count;
                part->mbr_type = mbr->parts[i].type;
                snprintf(part->devpath, sizeof(part->devpath), "%s%d", disk->devpath, i + 1);
            }
        }
    }

    if (disk->part_count == 1 && disk->parts[0].mbr_type == 0xEE)
        inst_scan_gpt(disk);

    if (disk->part_count > 0) return;

    dlen = (int)strlen(disk->devname);
    devfd = vfs_open("/dev", 0);
    if (devfd < 0) return;

    for (didx = 0; ; didx++) {
        if (vfs_readdir(devfd, name, &dtype, didx) != 0) break;
        if (strncmp(name, disk->devname, dlen) != 0) continue;
        suffix = name + dlen;
        if (*suffix < '1' || *suffix > '9') continue;
        part_number = 0;
        while (*suffix >= '0' && *suffix <= '9') {
            digit = *suffix - '0';
            if (part_number > (INT32_MAX - digit) / 10) {
                part_number = -1;
                break;
            }
            part_number = part_number * 10 + digit;
            suffix++;
        }
        if (part_number <= 0 || *suffix != '\0') continue;

        snprintf(partpath, sizeof(partpath), "/dev/%s", name);
        stat_fd = vfs_open(partpath, 0);
        if (stat_fd < 0) continue;
        psize = 0;
        ptype = 0;
        vfs_stat(stat_fd, &psize, &ptype);
        vfs_close_fd(stat_fd);

        part = inst_add_part(disk);
        if (!part) break;
        part->valid = 1;
        part->number = part_number;
        part->start_lba = 0;
        part->sector_count = psize / SECTOR_SIZE;
        part->mbr_type = 0x83;
        strncpy(part->devpath, partpath, sizeof(part->devpath) - 1);
    }
    vfs_close_fd(devfd);
}

static int inst_enumerate_disks(void)
{
    int fd;
    char name[64];
    unsigned int type;
    unsigned int idx;
    disk_info_t *disk;

    disk_count = 0;

    fd = vfs_open("/dev", 0);
    if (fd < 0) return -1;

    for (idx = 0; ; idx++) {
        if (vfs_readdir(fd, name, &type, idx) != 0) break;
        if (!inst_is_whole_disk(name)) continue;

        if (inst_reserve_disks(disk_count + 1) < 0) {
            vfs_close_fd(fd);
            return -1;
        }

        disk = &disks[disk_count];
        memset(disk->devname, 0, sizeof(disk->devname));
        memset(disk->devpath, 0, sizeof(disk->devpath));
        strncpy(disk->devname, name, sizeof(disk->devname) - 1);
        snprintf(disk->devpath, sizeof(disk->devpath), "/dev/%s", name);
        disk_count++;
    }
    vfs_close_fd(fd);
    return disk_count;
}

static void inst_scan_disks(void)
{
    int i;

    for (i = 0; i < disk_count; i++)
        inst_scan_disk(&disks[i]);
}

static int copy_overwrite_existing = 1;

static int inst_copy_file_vfs(const char *src, const char *dst)
{
    int fd_in;
    int fd_out;
    int r;
    int result;
    struct stat src_st;
    struct stat dst_st;
    mode_t src_mode;
    off_t copied;

    fd_in = vfs_open(src, 0);
    if (fd_in < 0) return -1;
    if (fstat(fd_in, &src_st) != 0 || src_st.st_size < 0) {
        close(fd_in);
        return -1;
    }
    src_mode = src_st.st_mode & 07777;
    fd_out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, src_mode);
    if (fd_out < 0) {
        close(fd_in);
        return -1;
    }

    copied = 0;
    result = -1;
    for (;;) {
        r = (int)copy_file_range(fd_in, NULL, fd_out, NULL,
                                 (size_t)0x7fffffffU, 0);
        if (r <= 0) break;
        if (r > src_st.st_size - copied) goto out;
        copied += r;
    }
    if (r < 0 || copied != src_st.st_size) goto out;
    if (fstat(fd_out, &dst_st) != 0 || dst_st.st_size != copied)
        goto out;
    result = 0;
out:
    if (close(fd_in) != 0) result = -1;
    if (close(fd_out) != 0) result = -1;
    return result;
}

static int inst_verify_kernel(const char *dst)
{
    int fd_in;
    int fd_out;
    int result;
    ssize_t received;
    ssize_t received_out;
    size_t compared;
    size_t request;
    off_t remaining;
    struct stat src_st;
    struct stat dst_st;
    uint8_t *buffer;

    result = -1;
    fd_in = -1;
    fd_out = -1;
    buffer = malloc(BUF_SIZE * 2);
    if (!buffer) return -1;
    fd_in = open("/boot/lebirun.kernel", O_RDONLY);
    fd_out = open(dst, O_RDONLY);
    if (fd_in < 0 || fd_out < 0) goto out;
    if (fsync(fd_out) != 0) goto out;
    if (fstat(fd_in, &src_st) != 0 || fstat(fd_out, &dst_st) != 0 ||
        src_st.st_size <= 0 || src_st.st_size != dst_st.st_size)
        goto out;
    remaining = src_st.st_size;
    while (remaining > 0) {
        request = remaining > BUF_SIZE ? BUF_SIZE : (size_t)remaining;
        received = read(fd_in, buffer, request);
        if (received <= 0) goto out;
        compared = 0;
        while (compared < (size_t)received) {
            received_out = read(fd_out, buffer + BUF_SIZE + compared,
                                (size_t)received - compared);
            if (received_out <= 0) goto out;
            compared += received_out;
        }
        if (memcmp(buffer, buffer + BUF_SIZE, (size_t)received) != 0)
            goto out;
        remaining -= received;
    }
    result = 0;
out:
    if (fd_in >= 0 && close(fd_in) != 0) result = -1;
    if (fd_out >= 0 && close(fd_out) != 0) result = -1;
    free(buffer);
    return result;
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

static int inst_count_dir_entries(const char *path, const char *skip)
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
        if (skip && strcmp(sub, skip) == 0) continue;
        if (inst_pkg_skip(sub)) continue;
        if (type == 2) {
            count += inst_count_dir_entries(sub, skip);
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
static char copy_error[192];

static void inst_set_copy_error(const char *kind, const char *path)
{
    if (copy_error[0] != '\0') return;
    snprintf(copy_error, sizeof(copy_error), "%s: %s", kind, path);
}

static void inst_copy_progress(const char *path)
{
    int pct;

    if (copy_total <= 0) {
        return;
    }

    pct = 10 + (copy_done * 85) / copy_total;
    if (pct > 95) pct = 95;
    if (pct != copy_last_pct) {
        copy_last_pct = pct;
        wiz_prog_update(path, pct);
    }
}

static void inst_copy_current(const char *path)
{
    int pct;

    if (copy_total <= 0) {
        return;
    }

    pct = 10 + (copy_done * 85) / copy_total;
    if (pct > 95) pct = 95;
    if (pct != copy_last_pct) {
        copy_last_pct = pct;
        wiz_prog_update(path, pct);
    }
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
    char name[256];
    unsigned int type;
    unsigned int idx;
    char src_path[MAX_PATH];
    char dst_path[MAX_PATH];
    int errors;
    int slen;
    int dlen;

    vfs_mkdir(dst, 0755);

    fd = vfs_open(src, 0);
    if (fd < 0) {
        if (copy_error[0] == '\0')
            snprintf(copy_error, sizeof(copy_error), "Directory open failed (%d): %s", fd, src);
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
            if (inst_copy_symlink_vfs(src_path, dst_path) < 0) {
                inst_set_copy_error("Symlink failed", src_path);
                errors++;
            }
            copy_done++;
            inst_copy_progress(src_path);
        } else {
            inst_copy_current(src_path);
            if (inst_copy_file_vfs(src_path, dst_path) < 0) {
                inst_set_copy_error("File failed", src_path);
                errors++;
            }
            copy_done++;
            inst_copy_progress(src_path);
        }
    }
    vfs_close_fd(fd);
    return (errors > 0) ? -1 : 0;
}

static int inst_mount_partition_as(const char *devpath, const char *mountpoint,
                                     const char *fstype)
{
    int ret;
    int pid;

    pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        int nfd;
        char *argv[6];
        nfd = open("/dev/null", O_WRONLY);
        if (nfd >= 0) {
            dup2(nfd, 1);
            dup2(nfd, 2);
            close(nfd);
        }
        argv[0] = "mount";
        argv[1] = "-t";
        argv[2] = (char *)fstype;
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

static void inst_format_process_line(char *line, char *error,
                                     size_t error_size)
{
    size_t len;

    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[len - 1] = '\0';
        len--;
    }
    if (line[0] == '\0') return;
    if (error && error_size > 0 &&
        (strstr(line, "lformat.ext4:") ||
         strstr(line, "Unable to start lformat.ext4")))
        snprintf(error, error_size, "%s", line);
    wiz_prog_log(line);
}

static int inst_format_ext4(const char *devpath, char *error,
                            size_t error_size)
{
    int pipefd[2];
    int pid;
    int status;
    int wait_result;
    int poll_result;
    int child_done;
    int pipe_open;
    int cancelled;
    int n;
    int i;
    int line_len;
    char read_buf[128];
    char line[192];
    char input_buf[16];
    char *argv[5];
    struct pollfd fds[2];

    if (error && error_size > 0) error[0] = '\0';
    if (pipe(pipefd) != 0) return -1;

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        close(pipefd[1]);
        argv[0] = "lformat.ext4";
        argv[1] = (char *)devpath;
        argv[2] = "-L";
        argv[3] = "LEBIRUN";
        argv[4] = NULL;
        execv("/sbin/lformat.ext4", argv);
        execv("/bin/lformat.ext4", argv);
        execv("/bin/lebu", argv);
        fprintf(stderr, "Unable to start lformat.ext4");
        fflush(stderr);
        _exit(127);
    }

    close(pipefd[1]);
    status = 0;
    child_done = 0;
    pipe_open = 1;
    cancelled = 0;
    line_len = 0;

    while (!child_done || pipe_open) {
        wait_result = waitpid(pid, &status, WNOHANG);
        if (wait_result == pid) child_done = 1;

        fds[0].fd = pipe_open ? pipefd[0] : -1;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = STDIN_FILENO;
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        poll_result = poll(fds, 2, 100);
        if (poll_result < 0) continue;

        if (fds[1].revents & POLLIN) {
            n = (int)read(STDIN_FILENO, input_buf, sizeof(input_buf));
            for (i = 0; i < n; i++) {
                if (input_buf[i] == 3 || input_buf[i] == 27) {
                    cancelled = 1;
                }
            }
            if (cancelled && !child_done) kill(pid, SIGTERM);
        }

        if (pipe_open &&
            (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            n = (int)read(pipefd[0], read_buf, sizeof(read_buf));
            if (n > 0) {
                for (i = 0; i < n; i++) {
                    if (read_buf[i] == '\n') {
                        line[line_len] = '\0';
                        inst_format_process_line(line, error, error_size);
                        line_len = 0;
                    } else if (line_len < (int)sizeof(line) - 1) {
                        line[line_len++] = read_buf[i];
                    }
                }
            }
            if (n == 0 || (fds[0].revents & POLLERR)) {
                close(pipefd[0]);
                pipe_open = 0;
            }
        }
    }

    if (!child_done) {
        wait_result = waitpid(pid, &status, 0);
        if (wait_result != pid) return -1;
    }
    if (line_len > 0) {
        line[line_len] = '\0';
        inst_format_process_line(line, error, error_size);
    }
    if (cancelled) {
        if (error && error_size > 0)
            snprintf(error, error_size, "Formatting cancelled.");
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

typedef struct {
    const char *name;
    int copy;
    int mode;
} inst_root_entry_t;

static const inst_root_entry_t inst_root_entries[] = {
    { "bin", 1, 0 }, { "boot", 1, 0 }, { "dev", 0, 0755 },
    { "etc", 1, 0 }, { "home", 1, 0 }, { "lib", 1, 0 },
    { "proc", 0, 0755 }, { "root", 1, 0 }, { "sbin", 1, 0 },
    { "sys", 0, 0755 }, { "tmp", 0, 1777 }, { "usr", 1, 0 },
    { "var", 1, 0 }, { "mnt", 0, 0755 }, { "run", 0, 0755 },
    { "srv", 0, 0755 }, { "opt", 0, 0755 }, { "media", 0, 0755 },
    { NULL, 0, 0 }
};

static int inst_copy_rootfs(const char *mountpoint)
{
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
            inst_set_copy_error("File failed", src);
            errors++;
        }
        copy_done++;
        inst_copy_progress(src);
    }

    for (i = 0; inst_root_entries[i].name; i++) {
        if (!inst_root_entries[i].copy) continue;
        snprintf(src, sizeof(src), "/%s", inst_root_entries[i].name);
        snprintf(dst, sizeof(dst), "%s/%s", mountpoint,
                 inst_root_entries[i].name);

        if (inst_copy_dir_recursive(src, dst, mountpoint) < 0) {
            errors++;
        }
    }

    for (i = 0; inst_root_entries[i].name; i++) {
        if (inst_root_entries[i].copy) continue;
        snprintf(dst, sizeof(dst), "%s/%s", mountpoint,
                 inst_root_entries[i].name);
        vfs_mkdir(dst, inst_root_entries[i].mode);
    }

    return (errors > 0) ? -1 : 0;
}

static int inst_count_rootfs(const char *skip)
{
    int total;
    int i;
    char path[MAX_PATH];

    total = 1;
    for (i = 0; inst_root_entries[i].name; i++) {
        if (!inst_root_entries[i].copy) continue;
        snprintf(path, sizeof(path), "/%s", inst_root_entries[i].name);
        total += inst_count_dir_entries(path, skip);
    }
    return total;
}

static int inst_install_grub_mbr(const char *disk_dev, int boot_part_num)
{
    uint8_t *mbr_buf;
    uint8_t *boot_buf;
    uint8_t *core_buf;
    uint8_t *verify_buf;
    int fd_disk;
    int fd_boot;
    int fd_core;
    int r;
    int pi;
    int entry_off;
    uint32_t core_lba;
    int result;
    off_t seek_ret;

    mbr_buf = NULL;
    boot_buf = NULL;
    core_buf = NULL;
    verify_buf = NULL;
    fd_disk = -1;
    fd_boot = -1;
    fd_core = -1;
    result = -1;
    boot_error[0] = '\0';

    mbr_buf = (uint8_t *)malloc(SECTOR_SIZE);
    boot_buf = (uint8_t *)malloc(SECTOR_SIZE);
    core_buf = (uint8_t *)malloc(BUF_SIZE);
    verify_buf = (uint8_t *)malloc(BUF_SIZE);
    if (!mbr_buf || !boot_buf || !core_buf || !verify_buf) {
        snprintf(boot_error, sizeof(boot_error), "boot: allocation failed");
        goto out;
    }

    memset(mbr_buf, 0, SECTOR_SIZE);
    memset(boot_buf, 0, SECTOR_SIZE);
    memset(core_buf, 0, BUF_SIZE);
    memset(verify_buf, 0, BUF_SIZE);

    fd_disk = vfs_open(disk_dev, 2);
    if (fd_disk < 0) {
        snprintf(boot_error, sizeof(boot_error), "boot: open %s failed", disk_dev);
        goto out;
    }

    r = vfs_read_fd(fd_disk, mbr_buf, SECTOR_SIZE);
    if (r < SECTOR_SIZE) {
        snprintf(boot_error, sizeof(boot_error), "boot: read MBR failed");
        goto out;
    }
    vfs_close_fd(fd_disk);
    fd_disk = -1;

    fd_boot = vfs_open("/boot/grub/i386-pc/boot.img", 0);
    if (fd_boot < 0) {
        snprintf(boot_error, sizeof(boot_error), "boot: open boot.img failed");
        goto out;
    }
    r = vfs_read_fd(fd_boot, boot_buf, SECTOR_SIZE);
    if (r < SECTOR_SIZE) {
        snprintf(boot_error, sizeof(boot_error), "boot: read boot.img failed");
        goto out;
    }
    vfs_close_fd(fd_boot);
    fd_boot = -1;

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
    if (fd_disk < 0) {
        snprintf(boot_error, sizeof(boot_error), "boot: reopen %s failed", disk_dev);
        goto out;
    }

    r = vfs_write_fd(fd_disk, mbr_buf, SECTOR_SIZE);
    if (r < SECTOR_SIZE) {
        snprintf(boot_error, sizeof(boot_error), "boot: write MBR failed");
        goto out;
    }

    fd_core = vfs_open("/boot/grub/i386-pc/core.img", 0);
    if (fd_core < 0) {
        snprintf(boot_error, sizeof(boot_error), "boot: open core.img failed");
        goto out;
    }

    seek_ret = lseek(fd_disk, (off_t)SECTOR_SIZE, SEEK_SET);
    if (seek_ret != (off_t)SECTOR_SIZE) {
        snprintf(boot_error, sizeof(boot_error), "boot: seek core area failed");
        goto out;
    }

    while ((r = vfs_read_fd(fd_core, core_buf, BUF_SIZE)) > 0) {
        if (vfs_write_fd(fd_disk, core_buf, r) != r) {
            snprintf(boot_error, sizeof(boot_error), "boot: write core.img failed");
            goto out;
        }
    }
    if (r < 0) {
        snprintf(boot_error, sizeof(boot_error), "boot: read core.img failed");
        goto out;
    }

    vfs_close_fd(fd_core);
    fd_core = -1;

    seek_ret = lseek(fd_disk, (off_t)SECTOR_SIZE, SEEK_SET);
    if (seek_ret != (off_t)SECTOR_SIZE) {
        snprintf(boot_error, sizeof(boot_error), "boot: seek verify failed");
        goto out;
    }
    fd_core = vfs_open("/boot/grub/i386-pc/core.img", 0);
    if (fd_core < 0) {
        snprintf(boot_error, sizeof(boot_error), "boot: open core.img for verify failed");
        goto out;
    }
    while ((r = vfs_read_fd(fd_core, core_buf, BUF_SIZE)) > 0) {
        if (vfs_read_fd(fd_disk, verify_buf, r) != r ||
            memcmp(verify_buf, core_buf, r) != 0) {
            snprintf(boot_error, sizeof(boot_error), "boot: core.img verification failed");
            goto out;
        }
    }
    if (r < 0) {
        snprintf(boot_error, sizeof(boot_error), "boot: read core.img for verify failed");
        goto out;
    }

    result = 0;
    boot_error[0] = '\0';

out:
    if (fd_core >= 0) vfs_close_fd(fd_core);
    if (fd_boot >= 0) vfs_close_fd(fd_boot);
    if (fd_disk >= 0) vfs_close_fd(fd_disk);
    free(mbr_buf);
    free(boot_buf);
    free(core_buf);
    free(verify_buf);
    return result;
}

static int inst_write_grub_config(const char *mountpoint, const char *part_dev);

static int inst_is_gpt(const char *disk_dev)
{
    uint8_t sector0[SECTOR_SIZE];
    int fd;
    int r;

    fd = vfs_open(disk_dev, 0);
    if (fd < 0) return 0;
    r = vfs_read_fd(fd, sector0, SECTOR_SIZE);
    vfs_close_fd(fd);
    if (r < SECTOR_SIZE) return 0;
    if (sector0[510] != 0x55 || sector0[511] != 0xAA) return 0;
    return sector0[446 + 4] == 0xEE;
}

static int inst_find_esp(const char *disk_dev, char *out, size_t out_size)
{
    disk_info_t disk;
    int i;

    memset(&disk, 0, sizeof(disk));
    snprintf(disk.devpath, sizeof(disk.devpath), "%s", disk_dev);
    inst_scan_disk(&disk);
    for (i = 0; i < disk.part_count; i++) {
        if (disk.parts[i].mbr_type == 0xEF && disk.parts[i].valid) {
            snprintf(out, out_size, "%s", disk.parts[i].devpath);
            free(disk.parts);
            return 0;
        }
    }
    free(disk.parts);
    return -1;
}

static int inst_format_fat(const char *devpath)
{
    int pid;
    int status;
    int ret;
    char *argv[5];

    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        argv[0] = "lformat.fat";
        argv[1] = (char *)devpath;
        argv[2] = "-L";
        argv[3] = "ESP";
        argv[4] = NULL;
        execv("/sbin/lformat.fat", argv);
        execv("/bin/lformat.fat", argv);
        execv("/bin/lebu", argv);
        _exit(127);
    }
    ret = waitpid(pid, &status, 0);
    if (ret != pid) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

static int inst_write_grub_config_efi(const char *esp_mount,
                                      const char *part_dev, int part_num,
                                      int is_gpt)
{
    char cfg_path[MAX_PATH];
    char grub_cfg[512];
    int fd;
    int written;

    snprintf(grub_cfg, sizeof(grub_cfg),
        "set timeout=5\n"
        "set default=0\n"
        "search --no-floppy --label LEBIRUN --set=root\n"
        "if [ -z \"$root\" ]; then set root=(hd0,%s%d); fi\n"
        "\n"
        "menuentry \"Lebirun\" {\n"
        "\tmultiboot2 /boot/lebirun.kernel root=%s\n"
        "\tboot\n"
        "}\n",
        is_gpt ? "gpt" : "msdos", part_num, part_dev);

    snprintf(cfg_path, sizeof(cfg_path), "%s/boot/grub/grub.cfg", esp_mount);
    fd = open(cfg_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    written = write(fd, grub_cfg, strlen(grub_cfg));
    if (close(fd) != 0) return -1;
    if (written != (int)strlen(grub_cfg)) return -1;

    snprintf(cfg_path, sizeof(cfg_path), "%s/EFI/BOOT/grub.cfg", esp_mount);
    fd = open(cfg_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    written = write(fd, grub_cfg, strlen(grub_cfg));
    if (close(fd) != 0) return -1;
    return written == (int)strlen(grub_cfg) ? 0 : -1;
}

static int inst_iso_find(const char *cddev, const char *const *parts,
                         int nparts, uint64_t *out_lba, uint64_t *out_size);
static int inst_iso_copy(const char *cddev, uint64_t lba, uint64_t size,
                         const char *dst);
static void inst_clear_boot_code(const char *disk_dev);
static int inst_install_efi(const char *disk_dev, const char *part_dev,
                            int part_num)
{
    char esp_dev[MAX_PATH];
    char esp_mount[] = "/tmp/espinstall";
    char cd_dev[MAX_PATH];
    char dst[MAX_PATH];
    int is_gpt;
    int i;
    int cd_ok;
    uint64_t efi_lba;
    uint64_t efi_size;
    const char *efi_parts[3];

    is_gpt = inst_is_gpt(disk_dev);
    if (boot_esp[0] != '\0') {
        snprintf(esp_dev, sizeof(esp_dev), "%s", boot_esp);
    } else if (inst_find_esp(disk_dev, esp_dev, sizeof(esp_dev)) != 0) {
        if (is_gpt) {
            snprintf(boot_error, sizeof(boot_error),
                     "boot: no ESP found, create one with ldiskutil");
            return -1;
        }
        return 1;
    }

    vfs_mkdir("/tmp", 0755);
    cd_ok = 0;
    for (i = 0; i < 4; i++) {
        snprintf(cd_dev, sizeof(cd_dev), "/dev/sr%d", i);
        efi_parts[0] = "boot";
        efi_parts[1] = "grub";
        efi_parts[2] = "BOOTX64.EFI";
        if (inst_iso_find(cd_dev, efi_parts, 3, &efi_lba,
                          &efi_size) == 0) {
            cd_ok = 1;
            break;
        }
    }
    if (!cd_ok) {
        snprintf(boot_error, sizeof(boot_error),
                 "boot: install media not found");
        return -1;
    }

    vfs_mkdir(esp_mount, 0755);
    inst_umount_partition(esp_mount);
    if (inst_mount_partition_as(esp_dev, esp_mount, "fat") != 0) {
        if (inst_format_fat(esp_dev) != 0) {
            snprintf(boot_error, sizeof(boot_error),
                     "boot: format ESP failed");
            return -1;
        }
        if (inst_mount_partition_as(esp_dev, esp_mount, "fat") != 0) {
            snprintf(boot_error, sizeof(boot_error), "boot: mount ESP failed");
            return -1;
        }
    }

    snprintf(dst, sizeof(dst), "%s/EFI", esp_mount);
    vfs_mkdir(dst, 0755);
    snprintf(dst, sizeof(dst), "%s/EFI/BOOT", esp_mount);
    vfs_mkdir(dst, 0755);
    snprintf(dst, sizeof(dst), "%s/boot", esp_mount);
    vfs_mkdir(dst, 0755);
    snprintf(dst, sizeof(dst), "%s/boot/grub", esp_mount);
    vfs_mkdir(dst, 0755);

    snprintf(dst, sizeof(dst), "%s/EFI/BOOT/BOOTX64.EFI", esp_mount);
    if (inst_iso_copy(cd_dev, efi_lba, efi_size, dst) < 0) {
        snprintf(boot_error, sizeof(boot_error), "boot: copy BOOTX64.EFI failed");
        inst_umount_partition(esp_mount);
        return -1;
    }
    if (inst_write_grub_config_efi(esp_mount, part_dev, part_num,
                                   is_gpt) < 0) {
        snprintf(boot_error, sizeof(boot_error), "boot: write EFI grub.cfg failed");
        inst_umount_partition(esp_mount);
        return -1;
    }
    inst_clear_boot_code(disk_dev);
    inst_umount_partition(esp_mount);
    return 0;
}

static void inst_clear_boot_code(const char *disk_dev) {
    uint8_t sec[SECTOR_SIZE];
    int fd;
    int r;
    int i;
    int dirty;

    fd = vfs_open(disk_dev, 2);
    if (fd < 0) return;
    r = vfs_read_fd(fd, sec, SECTOR_SIZE);
    if (r < SECTOR_SIZE) {
        vfs_close_fd(fd);
        return;
    }
    dirty = 0;
    for (i = 0; i < 440; i++) {
        if (sec[i]) {
            sec[i] = 0;
            dirty = 1;
        }
    }
    if (!dirty) {
        vfs_close_fd(fd);
        return;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        vfs_close_fd(fd);
        return;
    }
    vfs_write_fd(fd, sec, SECTOR_SIZE);
    vfs_close_fd(fd);
}

static int inst_cd_pread(int cdfd, uint64_t lba, void *buf) {
    int r;

    if (lseek(cdfd, (off_t)(lba * 2048), SEEK_SET) < 0) return -1;
    r = vfs_read_fd(cdfd, buf, 2048);
    if (r < 2048) return -1;
    return 0;
}

static uint32_t inst_iso_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int inst_iso_name_match(const uint8_t *raw, uint64_t raw_len,
                               const char *want) {
    uint64_t i;
    uint64_t wl;

    for (wl = 0; want[wl]; wl++) {
    }
    if (raw_len == wl) {
        for (i = 0; i < wl; i++) {
            char c = (char)raw[i];
            char w = want[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            if (w >= 'a' && w <= 'z') w -= 32;
            if (c != w) return 0;
        }
        return 1;
    }
    if (raw_len == wl + 2 && raw[wl] == ';' && raw[wl + 1] == '1') {
        for (i = 0; i < wl; i++) {
            char c = (char)raw[i];
            char w = want[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            if (w >= 'a' && w <= 'z') w -= 32;
            if (c != w) return 0;
        }
        return 1;
    }
    return 0;
}

static int inst_iso_find(const char *cddev, const char *const *parts,
                         int nparts, uint64_t *out_lba, uint64_t *out_size) {
    static uint8_t sec[2048];
    uint32_t extent;
    uint32_t size;
    int level;
    int cdfd;
    int rc;

    cdfd = vfs_open(cddev, 0);
    if (cdfd < 0) return -1;
    rc = -1;
    if (inst_cd_pread(cdfd, 16, sec) != 0) goto out;
    if (sec[0] != 1 || memcmp(sec + 1, "CD001", 5) != 0) goto out;
    extent = inst_iso_le32(sec + 156 + 2);
    size = inst_iso_le32(sec + 156 + 10);
    if (!extent || !size || size > 64 * 1024 * 1024) return -1;
    for (level = 0; level < nparts; level++) {
        uint64_t off = 0;
        uint64_t seclba;
        uint64_t secoff;
        int found = 0;
        while (off < size) {
            uint8_t rec_len;
            uint8_t name_len;
            if (inst_cd_pread(cdfd, (uint64_t)extent + off / 2048,
                              sec) != 0) goto out;
            seclba = off / 2048;
            secoff = off % 2048;
            if (secoff + 33 > 2048) {
                off = (seclba + 1) * 2048;
                continue;
            }
            rec_len = sec[secoff];
            if (!rec_len) {
                off = (seclba + 1) * 2048;
                continue;
            }
            if (rec_len < 33 || off + rec_len > size) goto out;
            if ((uint64_t)secoff + rec_len > 2048) goto out;
            name_len = sec[secoff + 32];
            if (33 + (uint64_t)name_len > rec_len) goto out;
            if (name_len == 1 &&
                (sec[secoff + 33] == 0 || sec[secoff + 33] == 1)) {
                off += rec_len;
                continue;
            }
            if (inst_iso_name_match(sec + secoff + 33, name_len,
                                    parts[level])) {
                extent = inst_iso_le32(sec + secoff + 2);
                size = inst_iso_le32(sec + secoff + 10);
                if (!extent || size > 64 * 1024 * 1024) goto out;
                found = 1;
                break;
            }
            off += rec_len;
        }
        if (!found) goto out;
    }
    *out_lba = extent;
    *out_size = size;
    rc = 0;
out:
    vfs_close_fd(cdfd);
    return rc;
}

static int inst_iso_copy(const char *cddev, uint64_t lba, uint64_t size,
                         const char *dst) {
    static uint8_t buf[32 * 2048];
    int fd;
    int cdfd;
    uint64_t done;
    uint64_t nsec;

    cdfd = vfs_open(cddev, 0);
    if (cdfd < 0) return -1;
    fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        vfs_close_fd(cdfd);
        return -1;
    }
    done = 0;
    while (done < size) {
        uint64_t want;
        uint64_t got;
        nsec = (size - done + 2047) / 2048;
        if (nsec > 32) nsec = 32;
        want = nsec * 2048;
        if (lseek(cdfd, (off_t)((lba + done / 2048) * 2048), SEEK_SET) < 0) {
            close(fd);
            vfs_close_fd(cdfd);
            return -1;
        }
        got = 0;
        while (got < want) {
            int r = vfs_read_fd(cdfd, buf + got, (uint32_t)(want - got));
            if (r <= 0) {
                close(fd);
                vfs_close_fd(cdfd);
                return -1;
            }
            got += (uint64_t)r;
        }
        {
            uint64_t chunk = nsec * 2048;
            uint64_t written = 0;
            int w;
            if (chunk > size - done) chunk = size - done;
            while (written < chunk) {
                w = write(fd, buf + written,
                          (size_t)(chunk - written));
                if (w <= 0) {
                    close(fd);
                    vfs_close_fd(cdfd);
                    return -1;
                }
                written += (uint64_t)w;
            }
            done += chunk;
        }
    }
    vfs_close_fd(cdfd);
    if (close(fd) != 0) return -1;
    return 0;
}

static int inst_install_boot_bios(const char *mountpoint, const char *disk_dev, const char *part_dev, int part_num)
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

    if (inst_write_grub_config(mountpoint, part_dev) < 0) {
        snprintf(boot_error, sizeof(boot_error), "boot: write grub.cfg failed");
        return -1;
    }

    if (inst_install_grub_mbr(disk_dev, part_num) < 0) {
        return -1;
    }

    return 0;
}

static int inst_install_boot(const char *mountpoint, const char *disk_dev, const char *part_dev, int part_num)
{
    char esp_probe[MAX_PATH];

    if (inst_is_gpt(disk_dev)) {
        if (inst_write_grub_config(mountpoint, part_dev) < 0) {
            snprintf(boot_error, sizeof(boot_error), "boot: write grub.cfg failed");
            return -1;
        }
        return inst_install_efi(disk_dev, part_dev, part_num);
    }
    if (boot_mode == BOOT_UEFI) {
        if (inst_write_grub_config(mountpoint, part_dev) < 0) {
            snprintf(boot_error, sizeof(boot_error), "boot: write grub.cfg failed");
            return -1;
        }
        if (boot_esp[0] == '\0' &&
            inst_find_esp(disk_dev, esp_probe, sizeof(esp_probe)) != 0) {
            snprintf(boot_error, sizeof(boot_error),
                     "boot: no ESP found, create one with ldiskutil");
            return -1;
        }
        return inst_install_efi(disk_dev, part_dev, part_num);
    }
    if (boot_mode == BOOT_BOTH) {
        int r = inst_install_boot_bios(mountpoint, disk_dev, part_dev,
                                       part_num);
        if (r == 0 && inst_install_efi(disk_dev, part_dev, part_num) != 0)
            wiz_prog_log("Warning: UEFI boot files failed.");
        return r;
    }
    return inst_install_boot_bios(mountpoint, disk_dev, part_dev, part_num);
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
    fd = open(cfg_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    written = write(fd, grub_cfg, strlen(grub_cfg));
    if (close(fd) != 0) return -1;
    return written == (int)strlen(grub_cfg) ? 0 : -1;
}

#define SALT_LEN 8

static void inst_generate_salt(char *salt, int len)
{
    static const char charset[] =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789./";
    unsigned int seed;
    int fd;
    int i;

    seed = getticks() ^ ((unsigned int)getpid() << 16) ^ (unsigned int)time(0);
    fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        unsigned int rnd = 0;
        if (read(fd, &rnd, sizeof(rnd)) == (int)sizeof(rnd)) seed ^= rnd;
        close(fd);
    }

    for (i = 0; i < len; i++) {
        seed = seed * 1103515245 + 12345;
        seed ^= (unsigned int)i * 2654435761u;
        salt[i] = charset[(seed >> 16) % (sizeof(charset) - 1)];
    }
    salt[len] = '\0';
}

static const char *inst_hash_password(const char *password)
{
    char salt[3 + SALT_LEN + 2];
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

    {
        char hbuf[256];
        const char *hpw = "!";
        if (hashed) {
            size_t hl = strlen(hashed);
            if (hl >= sizeof(hbuf)) hl = sizeof(hbuf) - 1;
            memcpy(hbuf, hashed, hl);
            hbuf[hl] = '\0';
            hpw = hbuf;
            snprintf(line, sizeof(line), "%s:%s:0:0:99999:7:::\n", username, hpw);
        } else {
            snprintf(line, sizeof(line), "%s:%s:0:0:99999:7:::\n", username, hpw);
        }
        wlen = (int)strlen(line);
        vfs_write_fd(fd, line, wlen);
        {
            volatile char *vp = (volatile char *)hbuf;
            for (size_t i = 0; i < sizeof(hbuf); i++) vp[i] = 0;
        }
    }
    vfs_close_fd(fd);

    return 0;
}

static int inst_write_timezone(const char *mountpoint, const char *tz)
{
    char path[MAX_PATH];
    char line[128];
    int fd;
    int len;
    int written;

    snprintf(path, sizeof(path), "%s/etc/timezone", mountpoint);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    len = snprintf(line, sizeof(line), "%s\n", tz);
    if (len < 0 || len >= (int)sizeof(line)) {
        close(fd);
        return -1;
    }
    written = (int)write(fd, line, (size_t)len);
    if (close(fd) < 0 || written != len) return -1;

    snprintf(path, sizeof(path), "%s/etc/environment", mountpoint);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    len = snprintf(line, sizeof(line), "TZ=%s\n", tz);
    if (len < 0 || len >= (int)sizeof(line)) {
        close(fd);
        return -1;
    }
    written = (int)write(fd, line, (size_t)len);
    if (close(fd) < 0 || written != len) return -1;

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

    snprintf(src_path, sizeof(src_path), "%s/%s", LEBPKG_INSTALLED_DIR, pkgname);
    snprintf(db_dir, sizeof(db_dir), "%s/etc/lebpkg", mountpoint);
    vfs_mkdir(db_dir, 0755);
    snprintf(db_dir, sizeof(db_dir), "%s%s", mountpoint, LEBPKG_INSTALLED_DIR);
    vfs_mkdir(db_dir, 0755);
    snprintf(dst_path, sizeof(dst_path), "%s%s/%s", mountpoint, LEBPKG_INSTALLED_DIR, pkgname);
    return inst_copy_file_vfs(src_path, dst_path);
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
    if (fd < 0) {
        inst_set_copy_error("Package database open failed", inst_dir);
        return -1;
    }
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
            if (inst_copy_pkg_db_entry(mountpoint, name) != 0) {
                inst_set_copy_error("Package record failed", target_path);
                vfs_close_fd(fd);
                return -1;
            }
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
    lebui_shutdown();
}


typedef struct {
    char username[64];
    char password[64];
} user_entry_t;

static user_entry_t *users;
static int user_count;
static int user_capacity;
static char root_password[64];

static int inst_reserve_users(int need)
{
    user_entry_t *new_users;
    int new_cap;
    int i;

    if (user_capacity >= need) return 0;

    new_cap = user_capacity ? user_capacity * 2 : 4;
    while (new_cap < need) new_cap *= 2;

    new_users = (user_entry_t *)realloc(users, (size_t)new_cap * sizeof(user_entry_t));
    if (!new_users) return -1;

    users = new_users;
    for (i = user_capacity; i < new_cap; i++)
        memset(&users[i], 0, sizeof(users[i]));
    user_capacity = new_cap;
    return 0;
}

static void attach_tabbar(int active_tab, int cols);

static int inst_set_root_password(const char *mountpoint, const char *password)
{
    char shadow_path[MAX_PATH];
    char *shadow_buf;
    char *new_shadow;
    const char *hashed;
    int shadow_fd;
    int rlen;
    int wlen;
    int new_len;
    int line_start;
    int line_end;
    int j;
    int found_root;
    uint64_t fsize;
    uint64_t ftype;
    size_t new_cap;

    hashed = inst_hash_password(password);
    if (!hashed) return -1;

    snprintf(shadow_path, sizeof(shadow_path), "%s/etc/shadow", mountpoint);
    shadow_fd = vfs_open(shadow_path, 0);
    if (shadow_fd < 0) return -1;

    fsize = 0;
    ftype = 0;
    if (vfs_stat(shadow_fd, &fsize, &ftype) < 0)
        fsize = 4096;
    if (fsize > 1024 * 1024) fsize = 1024 * 1024;

    shadow_buf = (char *)malloc((size_t)fsize + 1);
    if (!shadow_buf) {
        vfs_close_fd(shadow_fd);
        return -1;
    }

    rlen = vfs_read_fd(shadow_fd, shadow_buf, (unsigned int)fsize);
    vfs_close_fd(shadow_fd);
    if (rlen < 0) {
        free(shadow_buf);
        return -1;
    }
    shadow_buf[rlen] = '\0';

    new_cap = (size_t)rlen + strlen(hashed) + 128;
    new_shadow = (char *)malloc(new_cap);
    if (!new_shadow) {
        free(shadow_buf);
        return -1;
    }

    new_len = 0;
    j = 0;
    found_root = 0;
    while (j < rlen) {
        line_start = j;
        while (j < rlen && shadow_buf[j] != '\n') j++;
        line_end = j;
        if (j < rlen) j++;
        if (line_end - line_start >= 5 &&
            memcmp(shadow_buf + line_start, "root:", 5) == 0) {
            wlen = snprintf(new_shadow + new_len, new_cap - (size_t)new_len,
                            "root:%s:0:0:99999:7:::\n", hashed);
            found_root = 1;
        } else {
            wlen = snprintf(new_shadow + new_len, new_cap - (size_t)new_len,
                            "%.*s\n", line_end - line_start, shadow_buf + line_start);
        }
        if (wlen < 0 || (size_t)(new_len + wlen) >= new_cap) {
            free(shadow_buf);
            free(new_shadow);
            return -1;
        }
        new_len += wlen;
    }

    if (!found_root) {
        wlen = snprintf(new_shadow + new_len, new_cap - (size_t)new_len,
                        "root:%s:0:0:99999:7:::\n", hashed);
        if (wlen < 0 || (size_t)(new_len + wlen) >= new_cap) {
            free(shadow_buf);
            free(new_shadow);
            return -1;
        }
        new_len += wlen;
    }

    vfs_unlink(shadow_path);
    vfs_create(shadow_path, 0600);
    shadow_fd = vfs_open(shadow_path, 2);
    if (shadow_fd < 0) {
        free(shadow_buf);
        free(new_shadow);
        return -1;
    }

    wlen = vfs_write_fd(shadow_fd, new_shadow, new_len);
    vfs_close_fd(shadow_fd);
    free(shadow_buf);
    free(new_shadow);
    return (wlen == new_len) ? 0 : -1;
}

typedef struct {
    int drawn;
    int bx;
    int by;
    int bw;
    int log_y;
    int log_h;
    int log_count;
    char log_lines[8][64];
} wiz_prog_state_t;

static wiz_prog_state_t wizprog;

static void wiz_prog_init(const char *title)
{
    int rows = term_sz.rows;
    int cols = term_sz.cols;
    int bw = 56;
    int prog_h = 8;
    int log_h;
    int by;
    int log_y;
    int bx;
    int i;

    wizprog.drawn = 0;
    wizprog.log_count = 0;
    if (!title)
        title = "";
    if (cols < 40)
        return;
    if (bw > cols - 4)
        bw = cols - 4;
    by = 3;
    log_y = by + prog_h + 1;
    log_h = rows - log_y - 2;
    if (log_h < 4)
        log_h = 4;
    if (log_h > 10)
        log_h = 10;
    bx = (cols - bw) / 2 + 1;
    if (bx < 2)
        bx = 2;
    lebui_draw_screen(NULL, " Please wait...", rows, cols);
    lebui_draw_box_shadow(by, bx, prog_h, bw, title);
    lebui_draw_box_shadow(log_y, bx, log_h, bw, "Log");
    for (i = 0; i < 8; i++)
        wizprog.log_lines[i][0] = '\0';
    wizprog.drawn = 1;
    wizprog.bx = bx;
    wizprog.by = by;
    wizprog.bw = bw;
    wizprog.log_y = log_y;
    wizprog.log_h = log_h;
    lebui_flush();
}

static void wiz_prog_update(const char *msg, int pct)
{
    int bar_w;
    int filled;
    int mw;
    int i;

    if (!wizprog.drawn)
        return;
    if (!msg)
        msg = "";
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    bar_w = wizprog.bw - 8;
    if (bar_w < 1)
        bar_w = 1;
    mw = wizprog.bw - 6;
    if (mw < 1)
        mw = 1;
    lebui_goto(wizprog.by + 2, wizprog.bx + 3);
    printf("%s%-*.*s%s", LEBUI_CLR_MENU, mw, mw, msg, LEBUI_CLR_NORMAL);
    filled = (pct * bar_w) / 100;
    if (filled > bar_w)
        filled = bar_w;
    lebui_goto(wizprog.by + 4, wizprog.bx + 4);
    printf("%s", LEBUI_CLR_PROG);
    for (i = 0; i < filled; i++)
        putchar(' ');
    printf("%s", LEBUI_CLR_PROG_BG);
    for (i = filled; i < bar_w; i++)
        putchar(' ');
    lebui_goto(wizprog.by + 5, wizprog.bx + wizprog.bw / 2 - 2);
    printf("%s%3d%%%s", LEBUI_CLR_MENU, pct, LEBUI_CLR_NORMAL);
    lebui_flush();
}

static void wiz_prog_log(const char *msg)
{
    int log_area;
    int i;
    int mw;

    if (!wizprog.drawn || !msg)
        return;
    log_area = wizprog.log_h - 2;
    if (log_area > 8)
        log_area = 8;
    if (log_area < 1)
        return;
    mw = wizprog.bw - 6;
    if (mw < 1)
        mw = 1;
    if (mw > 63)
        mw = 63;
    if (wizprog.log_count < log_area) {
        strncpy(wizprog.log_lines[wizprog.log_count], msg, mw);
        wizprog.log_lines[wizprog.log_count][mw] = '\0';
        wizprog.log_count++;
    } else {
        for (i = 0; i < log_area - 1; i++)
            strcpy(wizprog.log_lines[i], wizprog.log_lines[i + 1]);
        strncpy(wizprog.log_lines[log_area - 1], msg, mw);
        wizprog.log_lines[log_area - 1][mw] = '\0';
    }
    for (i = 0; i < wizprog.log_count && i < log_area; i++) {
        lebui_goto(wizprog.log_y + 1 + i, wizprog.bx + 3);
        printf("%s%-*s%s", LEBUI_CLR_DIM_CLR, mw, wizprog.log_lines[i],
               LEBUI_CLR_NORMAL);
    }
    lebui_flush();
}

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

    if (p->mbr_type == 0xEF) {
        lebui_msgbox_auto("Error",
                          "Refusing to install onto the ESP.",
                          term_sz.rows, term_sz.cols);
        return -1;
    }

    wiz_prog_init("Installing");
    wiz_prog_update("Preparing...", 0);
    usleep(50000);

    if (do_format) {
        wiz_prog_update("Formatting partition...", 0);
        snprintf(fmsg, sizeof(fmsg), "Formatting %s as ext4...", p->devpath);
        wiz_prog_log(fmsg);
        fret = inst_format_ext4(p->devpath, fmsg, sizeof(fmsg));
        if (fret != 0) {
            if (fmsg[0] == '\0')
                snprintf(fmsg, sizeof(fmsg),
                         "Failed to format partition (status=%d, path=%s).",
                         fret, p->devpath);
            lebui_msgbox_auto("Error", fmsg, term_sz.rows, term_sz.cols);
            return -1;
        }
        wiz_prog_log("Format complete.");
    }

    snprintf(mountpoint, sizeof(mountpoint), "/tmp/lebinstall");
    vfs_mkdir("/tmp", 0755);
    vfs_mkdir(mountpoint, 0755);
    inst_umount_partition(mountpoint);

    wiz_prog_update("Mounting partition...", 5);
    snprintf(logbuf, sizeof(logbuf), "Mounting %s...", p->devpath);
    wiz_prog_log(logbuf);
    if (inst_mount_partition_as(p->devpath, mountpoint, "ext4") != 0) {
        lebui_msgbox_auto("Error", "Failed to mount partition.", term_sz.rows, term_sz.cols);
        return -1;
    }
    wiz_prog_log("Partition mounted.");

    wiz_prog_update("Counting files...", 10);
    wiz_prog_log("Counting files to copy...");
    copy_total = inst_count_rootfs(mountpoint);
    if (copy_total < 1) copy_total = 1;
    copy_done = 0;
    copy_last_pct = -1;
    copy_overwrite_existing = 0;
    copy_error[0] = '\0';
    wiz_prog_update("Copying files...", 10);
    wiz_prog_log("Copying rootfs...");
    if (inst_copy_rootfs(mountpoint) < 0) {
        wiz_prog_log("Warning: some files could not be copied.");
        if (copy_error[0] != '\0') wiz_prog_log(copy_error);
    }
    copy_overwrite_existing = 1;
    wiz_prog_log("Rootfs copy complete.");

    wiz_prog_update("Installing bootloader...", 96);
    wiz_prog_log("Installing GRUB bootloader...");
    if (inst_install_boot(mountpoint, d->devpath, p->devpath, p->number) < 0) {
        wiz_prog_log("Warning: bootloader had errors.");
        if (boot_error[0] != '\0') {
            wiz_prog_log(boot_error);
        }
    } else {
        wiz_prog_log("Bootloader installed.");
    }

    for (i = 0; i < user_count; i++) {
        wiz_prog_update("Creating user accounts...", 97);
        snprintf(logbuf, sizeof(logbuf), "Creating user: %s", users[i].username);
        wiz_prog_log(logbuf);
        inst_create_user(mountpoint, users[i].username, users[i].password, 1000 + i);
    }

    if (root_password[0] != '\0') {
        wiz_prog_update("Setting root password...", 98);
        inst_set_root_password(mountpoint, root_password);
        memset(root_password, 0, sizeof(root_password));
        wiz_prog_log("Root password updated.");
    }

    wiz_prog_update("Setting timezone...", 99);
    snprintf(logbuf, sizeof(logbuf), "Timezone: %s", tz_values[tz_idx]);
    wiz_prog_log(logbuf);
    if (inst_write_timezone(mountpoint, tz_values[tz_idx]) < 0) {
        wiz_prog_log("Warning: timezone could not be saved.");
    }

    wiz_prog_update("Writing package database...", 99);
    seeded = inst_seed_pkg_db_from_iso(mountpoint);
    if (seeded > 0) {
        snprintf(logbuf, sizeof(logbuf), "Package records copied: %d", seeded);
        wiz_prog_log(logbuf);
    } else {
        wiz_prog_log("No ISO package records found.");
    }

    wiz_prog_log("Unmounting...");
    if (inst_umount_partition(mountpoint) != 0) {
        wiz_prog_log("Warning: unmount failed.");
    }

    wiz_prog_update("Installation complete!", 100);
    wiz_prog_log("Done!");

    snprintf(donemsg, sizeof(donemsg),
             "Lebirun installed to %s. Reboot to start.",
             p->devpath);
    lebui_msgbox_auto("Complete", donemsg, term_sz.rows, term_sz.cols);

    return 0;
}


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

    snprintf(full, sizeof(full), "%s/boot/grub/i386-pc", mountpoint);
    if (strcmp(dst_path, full) == 0) return 1;
    if (strncmp(dst_path, full, strlen(full)) == 0 &&
        dst_path[strlen(full)] == '/') return 1;

    if (!upd_selected[UPD_GRUB_CFG]) {
        snprintf(full, sizeof(full), "%s/boot/grub/grub.cfg", mountpoint);
        if (strcmp(dst_path, full) == 0) return 1;
    }

    return 0;
}

static int inst_update_dir_recursive(const char *src, const char *dst, const char *mountpoint)
{
    int fd;
    char name[256];
    unsigned int type;
    unsigned int idx;
    char src_path[MAX_PATH];
    char dst_path[MAX_PATH];
    int errors;
    int slen;
    int dlen;

    vfs_mkdir(dst, 0755);

    fd = vfs_open(src, 0);
    if (fd < 0) {
        inst_set_copy_error("Directory open failed", src);
        return -1;
    }

    errors = 0;
    for (idx = 0; ; idx++) {
        if (vfs_readdir(fd, name, &type, idx) != 0) break;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        slen = snprintf(src_path, sizeof(src_path), "%s/%s", src, name);
        if (slen < 0 || slen >= (int)sizeof(src_path)) {
            inst_set_copy_error("Path too long", src);
            errors++;
            continue;
        }
        dlen = snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, name);
        if (dlen < 0 || dlen >= (int)sizeof(dst_path)) {
            inst_set_copy_error("Path too long", src);
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
            if (inst_copy_symlink_vfs(src_path, dst_path) < 0) {
                inst_set_copy_error("Symlink failed", dst_path);
                errors++;
            }
            copy_done++;
            inst_copy_progress(src_path);
        } else {
            inst_copy_current(src_path);
            if (inst_copy_file_vfs(src_path, dst_path) < 0) {
                inst_set_copy_error("File failed", dst_path);
                errors++;
            }
            copy_done++;
            inst_copy_progress(src_path);
        }
    }
    vfs_close_fd(fd);
    return (errors > 0) ? -1 : 0;
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
    copy_error[0] = '\0';

    wiz_prog_init("Updating");
    wiz_prog_update("Preparing...", 0);

    snprintf(mountpoint, sizeof(mountpoint), "/tmp/lebupdate");
    vfs_mkdir("/tmp", 0755);
    vfs_mkdir(mountpoint, 0755);
    inst_umount_partition(mountpoint);

    wiz_prog_update("Mounting partition...", 5);
    snprintf(logbuf, sizeof(logbuf), "Mounting %s...", p->devpath);
    wiz_prog_log(logbuf);
    if (inst_mount_partition_as(p->devpath, mountpoint, "ext4") != 0) {
        lebui_msgbox_auto("Error", "Failed to mount partition.", term_sz.rows, term_sz.cols);
        return -1;
    }
    wiz_prog_log("Partition mounted.");

    old_ver[0] = '\0';
    inst_read_pkg_version(mountpoint, "lebirun-base", old_ver, sizeof(old_ver));
    new_ver[0] = '\0';
    inst_read_pkg_version_file(LEBPKG_INSTALLED_DIR "/lebirun-base", new_ver, sizeof(new_ver));

    wiz_prog_update("Counting files...", 10);
    copy_total = 0;
    if (upd_selected[UPD_CORE]) {
        copy_total++;
        for (i = 0; core_dirs[i]; i++) {
            snprintf(path, sizeof(path), "/%s", core_dirs[i]);
            copy_total += inst_count_dir_entries(path, mountpoint);
        }
    }
    if (upd_selected[UPD_BOOT]) {
        copy_total += inst_count_dir_entries("/boot", "/boot/grub/i386-pc");
    }
    if (upd_selected[UPD_GRUB_CODE]) {
        copy_total += inst_count_dir_entries("/boot/grub/i386-pc", mountpoint);
    }
    if (upd_selected[UPD_USR_INC]) {
        copy_total += inst_count_dir_entries("/usr/include", mountpoint);
    }
    if (upd_selected[UPD_TERMINFO]) {
        copy_total += inst_count_dir_entries("/usr/share/terminfo", mountpoint);
    }
    if (copy_total < 1) copy_total = 1;
    copy_done = 0;
    copy_last_pct = -1;

    wiz_prog_update("Copying files...", 10);
    if (upd_selected[UPD_CORE]) {
        wiz_prog_log("Updating core system files...");
        snprintf(src, sizeof(src), "/init");
        snprintf(dst, sizeof(dst), "%s/init", mountpoint);
        if (inst_copy_file_vfs(src, dst) != 0) {
            inst_set_copy_error("File failed", dst);
            goto failed;
        }
        copy_done++;
        inst_copy_progress(src);
        for (i = 0; core_dirs[i]; i++) {
            snprintf(src, sizeof(src), "/%s", core_dirs[i]);
            snprintf(dst, sizeof(dst), "%s/%s", mountpoint, core_dirs[i]);
            if (inst_update_dir_recursive(src, dst, mountpoint) != 0)
                goto failed;
        }
        did_work = 1;
    }

    if (upd_selected[UPD_BOOT]) {
        wiz_prog_log("Updating boot files...");
        snprintf(src, sizeof(src), "/boot");
        snprintf(dst, sizeof(dst), "%s/boot", mountpoint);
        if (inst_update_dir_recursive(src, dst, mountpoint) != 0)
            goto failed;
        did_work = 1;
    }

    if (upd_selected[UPD_USR_INC]) {
        wiz_prog_log("Updating development headers...");
        snprintf(src, sizeof(src), "/usr/include");
        snprintf(dst, sizeof(dst), "%s/usr/include", mountpoint);
        if (inst_update_dir_recursive(src, dst, mountpoint) != 0)
            goto failed;
        did_work = 1;
    }

    if (upd_selected[UPD_TERMINFO]) {
        wiz_prog_log("Updating terminal database...");
        snprintf(src, sizeof(src), "/usr/share/terminfo");
        snprintf(dst, sizeof(dst), "%s/usr/share/terminfo", mountpoint);
        if (inst_update_dir_recursive(src, dst, mountpoint) != 0)
            goto failed;
        did_work = 1;
    }

    if (did_work) {
        wiz_prog_log("Selected files updated.");
    }

    if (upd_selected[UPD_GRUB_CODE]) {
        wiz_prog_update("Updating bootloader...", 96);
        wiz_prog_log("Updating GRUB boot code and modules...");
        snprintf(dst, sizeof(dst), "%s/boot/grub/i386-pc", mountpoint);
        vfs_mkdir(dst, 0755);
        if (inst_copy_dir_recursive("/boot/grub/i386-pc", dst, NULL) != 0)
            goto failed;
        if (inst_install_grub_mbr(d->devpath, p->number) < 0) {
            inst_set_copy_error("GRUB boot code failed", boot_error);
            goto failed;
        } else {
            wiz_prog_log("GRUB boot code updated.");
        }
    }

    if (upd_selected[UPD_GRUB_CFG]) {
        wiz_prog_update("Updating GRUB config...", 97);
        wiz_prog_log("Updating GRUB configuration...");
        if (inst_write_grub_config(mountpoint, p->devpath) < 0) {
            inst_set_copy_error("GRUB configuration failed", mountpoint);
            goto failed;
        }
        wiz_prog_log("GRUB configuration updated.");
    }

    if (upd_selected[UPD_PKGDB]) {
        wiz_prog_update("Updating package database...", 98);
        wiz_prog_log("Updating package database...");
        upgraded_pkgs = inst_upgrade_pkg_db_from_iso(mountpoint, &kept_pkgs);
        if (upgraded_pkgs < 0) goto failed;
        snprintf(logbuf, sizeof(logbuf), "Package records upgraded: %d", upgraded_pkgs);
        wiz_prog_log(logbuf);
        snprintf(logbuf, sizeof(logbuf), "Package records kept: %d", kept_pkgs);
        wiz_prog_log(logbuf);
    }

    if (upd_selected[UPD_BOOT]) {
        wiz_prog_log("Verifying installed kernel...");
        snprintf(dst, sizeof(dst), "%s/boot/lebirun.kernel", mountpoint);
        if (inst_verify_kernel(dst) != 0) {
            inst_set_copy_error("Kernel verification failed", dst);
            goto failed;
        }
    }

    wiz_prog_log("Unmounting...");
    if (inst_umount_partition(mountpoint) != 0) {
        inst_set_copy_error("Unmount failed", mountpoint);
        goto report_failed;
    }

    wiz_prog_update("Update complete!", 100);
    wiz_prog_log("Done!");

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

failed:
    if (inst_umount_partition(mountpoint) != 0)
        wiz_prog_log("Unmount failed after update error.");
report_failed:
    wiz_prog_log("Update incomplete.");
    if (copy_error[0] != '\0') wiz_prog_log(copy_error);
    snprintf(donemsg, sizeof(donemsg), "Update incomplete. %s",
             copy_error[0] ? copy_error : "A selected update step failed.");
    lebui_msgbox_auto("Update failed", donemsg, term_sz.rows, term_sz.cols);
    return -1;
}

static const char *g_tab_names[] = { "Install", "Update" };

static void attach_tabbar(int active_tab, int cols)
{
    lebui_tabbar_attach(g_tab_names, 2, active_tab, cols);
}

static const char *ins_wiz_names[] = {
    "Welcome",
    "Disk",
    "Partition",
    "Boot",
    "Packages",
    "Users",
    "Root password",
    "Timezone",
    "Summary",
    "Install"
};

#define INS_WELCOME 0
#define INS_DISK 1
#define INS_PART 2
#define INS_BOOT 3
#define INS_PKGS 4
#define INS_USER 5
#define INS_ROOTPW 6
#define INS_TZ 7
#define INS_SUMMARY 8
#define INS_INSTALL 9
#define INS_COUNT 10

static const char *uw_wiz_names[] = {
    "Welcome",
    "Disk",
    "Partition",
    "Items",
    "Summary",
    "Update"
};

#define UW_WELCOME 0
#define UW_DISK 1
#define UW_PART 2
#define UW_ITEMS 3
#define UW_SUMMARY 4
#define UW_DO 5
#define UW_COUNT 6

#define WIZ_FOCUS_STEPS 0
#define WIZ_FOCUS_CONTENT 1
#define WIZ_HELP "Move:Up/Dn Pane:L/R OK:Enter Tog:Space Back:b Next:n Page:Tab Quit:Esc Scan:r"

static const char *wiz_part_type(uint8_t t)
{
    switch (t) {
    case 0x83:
        return "Lebirun";
    case 0x82:
        return "Swap";
    case 0x0B:
    case 0x0C:
        return "FAT32";
    case 0x07:
        return "NTFS";
    case 0xEF:
        return "ESP";
    default:
        return "Other";
    }
}

static void wiz_disk_label(int di, char *buf, int bufsz)
{
    char sizebuf[32];

    inst_format_size(disks[di].disk_sectors, sizebuf, sizeof(sizebuf));
    snprintf(buf, bufsz, "%s  %s  %d part(s)", disks[di].devpath, sizebuf,
             disks[di].part_count);
}

static void wiz_part_label(int di, int pi, char *buf, int bufsz)
{
    char sizebuf[32];

    inst_format_size(disks[di].parts[pi].sector_count, sizebuf,
                     sizeof(sizebuf));
    snprintf(buf, bufsz, "%s  %s  %s", disks[di].parts[pi].devpath, sizebuf,
             wiz_part_type(disks[di].parts[pi].mbr_type));
}

static int wiz_rescan(int *disk_idx, int *part_idx)
{
    char disksave[32];
    char partsave[32];
    int i;
    int j;

    disksave[0] = '\0';
    partsave[0] = '\0';
    if (*disk_idx >= 0 && *disk_idx < disk_count)
        snprintf(disksave, sizeof(disksave), "%s",
                 disks[*disk_idx].devpath);
    if (*disk_idx >= 0 && *disk_idx < disk_count &&
        *part_idx >= 0 && *part_idx < disks[*disk_idx].part_count)
        snprintf(partsave, sizeof(partsave), "%s",
                 disks[*disk_idx].parts[*part_idx].devpath);

    if (inst_enumerate_disks() <= 0) {
        *disk_idx = -1;
        *part_idx = -1;
        return 0;
    }
    inst_scan_disks();

    *disk_idx = -1;
    *part_idx = -1;
    for (i = 0; i < disk_count; i++) {
        if (disksave[0] && strcmp(disks[i].devpath, disksave) == 0) {
            *disk_idx = i;
            break;
        }
    }
    if (*disk_idx >= 0 && partsave[0]) {
        for (j = 0; j < disks[*disk_idx].part_count; j++) {
            if (strcmp(disks[*disk_idx].parts[j].devpath, partsave) == 0) {
                *part_idx = j;
                break;
            }
        }
    }
    return disk_count;
}

static void wiz_side_row(int y, int x, int w, int i, const char **names,
                         char status[][24], int cur, int cursor, int focused,
                         const int *done, const int *locked);
static void wiz_content_row(int y, int x, int w, char **labels,
                            const int *checks, int idx, int count, int sel,
                            int focused);
static void wiz_draw_sidebar(int y, int x, int h, int w, const char **names,
                             char status[][24], int count, int cur, int cursor,
                             int focused, const int *done, const int *locked,
                             const char *title)
{
    int inner;
    int i;

    lebui_draw_box_shadow(y, x, h, w, title);
    inner = h - 2;
    for (i = 0; i < inner && i < count; i++)
        wiz_side_row(y + 1 + i, x, w, i, names, status, cur, cursor, focused,
                     done, locked);
    for (; i < inner; i++) {
        lebui_goto(y + 1 + i, x + 1);
        printf("%s%-*s", LEBUI_CLR_MENU, w - 2, "");
    }
    printf("%s", LEBUI_CLR_NORMAL);
}

static void wiz_draw_rows(int y, int x, int h, int w, char **labels,
                          const int *checks, int count, int sel, int scroll,
                          int focused)
{
    int i;

    for (i = 0; i < h; i++)
        wiz_content_row(y + i, x, w, labels, checks, scroll + i, count, sel,
                        focused);
    printf("%s", LEBUI_CLR_NORMAL);
}

static int wiz_draw_info(int y, int x, int w, const char **lines, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        lebui_goto(y + i, x + 1);
        printf("%s  %-*.*s", LEBUI_CLR_MENU, w - 4, w - 4, lines[i]);
    }
    printf("%s", LEBUI_CLR_NORMAL);
    return y + n;
}

typedef struct {
    int cur;
    int focus;
    int side;
    int sel;
    int scr;
} wiz_snap_t;

static int wiz_stained;

static void wiz_panes(int rows, int cols, int *sy, int *sx, int *sh, int *sw,
                      int *cy, int *cx, int *ch, int *cw)
{
    int side_w;

    side_w = cols - 48;
    if (side_w > 32)
        side_w = 32;
    if (side_w < 18)
        side_w = 18;
    *sy = 3;
    *sx = 2;
    *sh = rows - 4;
    if (*sh < 12)
        *sh = 12;
    *sw = side_w;
    *cy = 3;
    *cx = *sx + *sw + 1;
    *ch = *sh;
    *cw = cols - *cx - 1;
    if (*cw < 24)
        *cw = 24;
}

static void wiz_side_row(int y, int x, int w, int i, const char **names,
                         char status[][24], int cur, int cursor, int focused,
                         const int *done, const int *locked)
{
    char line[64];
    char mark;
    const char *color;

    if (locked[i])
        mark = '-';
    else if (i == cur)
        mark = '>';
    else if (done[i])
        mark = '*';
    else
        mark = ' ';
    if (focused == WIZ_FOCUS_STEPS && i == cursor)
        color = LEBUI_CLR_SELECT;
    else if (i == cur)
        color = LEBUI_CLR_SELECT;
    else if (i == cursor)
        color = LEBUI_CLR_DIM_CLR;
    else if (locked[i])
        color = LEBUI_CLR_DIM_CLR;
    else
        color = LEBUI_CLR_MENU;
    if (status[i][0])
        snprintf(line, sizeof(line), "%c %-13s %s", mark, names[i], status[i]);
    else
        snprintf(line, sizeof(line), "%c %s", mark, names[i]);
    lebui_goto(y, x + 1);
    printf("%s  %-*.*s%s", color, w - 4, w - 4, line, LEBUI_CLR_NORMAL);
}

static void wiz_content_row(int y, int x, int w, char **labels,
                            const int *checks, int idx, int count, int sel,
                            int focused)
{
    int maxw;

    maxw = w - 2;
    if (maxw < 4)
        maxw = 4;
    lebui_goto(y, x + 1);
    if (idx >= count) {
        printf("%s%-*s%s", LEBUI_CLR_MENU, maxw, "", LEBUI_CLR_NORMAL);
        return;
    }
    if (idx == sel && focused == WIZ_FOCUS_CONTENT)
        printf("%s", LEBUI_CLR_SELECT);
    else if (idx == sel)
        printf("%s", LEBUI_CLR_DIM_CLR);
    else
        printf("%s", LEBUI_CLR_MENU);
    if (checks) {
        if (checks[idx] < 0)
            printf("  (*) %-*.*s", maxw - 6, maxw - 6, labels[idx]);
        else
            printf("  [%c] %-*.*s", checks[idx] ? 'x' : ' ', maxw - 6,
                   maxw - 6, labels[idx]);
    } else {
        printf("  %-*.*s", maxw - 2, maxw - 2, labels[idx]);
    }
    printf("%s", LEBUI_CLR_NORMAL);
}

static void wiz_dlg_geo(int *bx, int *by, int *bw, int *bh)
{
    int rows = term_sz.rows;
    int cols = term_sz.cols;
    int sy, sx, sh, sw, cy, cx, ch, cw;

    wiz_panes(rows, cols, &sy, &sx, &sh, &sw, &cy, &cx, &ch, &cw);
    *bw = cw - 8;
    if (*bw < 30)
        *bw = 30;
    if (*bw > 52)
        *bw = 52;
    if (*bw > cw - 4)
        *bw = cw - 4;
    *bh = 9;
    *bx = cx + (cw - *bw) / 2;
    if (*bx < cx + 1)
        *bx = cx + 1;
    *by = cy + (ch - *bh) / 2;
    if (*by < cy + 1)
        *by = cy + 1;
}

static void wiz_dlg_frame(const char *title, int *bx, int *by, int *bw,
                          int *bh, const char *msg)
{
    const char *nl = strchr(msg, '\n');
    int l1 = nl ? (int)(nl - msg) : (int)strlen(msg);
    int l2 = nl ? (int)strlen(nl + 1) : 0;
    int r;

    wiz_dlg_geo(bx, by, bw, bh);
    lebui_draw_box_shadow(*by, *bx, *bh, *bw, title);
    for (r = 1; r < *bh - 1; r++) {
        lebui_goto(*by + r, *bx + 1);
        printf("%s%-*s%s", LEBUI_CLR_MENU, *bw - 2, "", LEBUI_CLR_NORMAL);
    }
    lebui_goto(*by + 2, *bx + 2);
    printf("%s%.*s%s", LEBUI_CLR_MENU, *bw - 4 > l1 ? l1 : *bw - 4, msg,
           LEBUI_CLR_NORMAL);
    if (nl) {
        lebui_goto(*by + 3, *bx + 2);
        printf("%s%.*s%s", LEBUI_CLR_MENU, *bw - 4 > l2 ? l2 : *bw - 4,
               nl + 1, LEBUI_CLR_NORMAL);
    }
    lebui_flush();
}

static int wiz_confirm(const char *title, const char *msg)
{
    int bx, by, bw, bh;
    int sel = 0;
    int key;

    wiz_stained = 1;
    wiz_dlg_frame(title, &bx, &by, &bw, &bh, msg);
    for (;;) {
        lebui_goto(by + 5, bx + bw / 2 - 9);
        if (sel == 0)
            printf("%s< Yes  >%s  %s[  No  ]%s", LEBUI_CLR_BTN_SEL,
                   LEBUI_CLR_MENU, LEBUI_CLR_BTN, LEBUI_CLR_NORMAL);
        else
            printf("%s[ Yes  ]%s  %s<  No  >%s", LEBUI_CLR_BTN,
                   LEBUI_CLR_MENU, LEBUI_CLR_BTN_SEL, LEBUI_CLR_NORMAL);
        lebui_flush();
        key = lebui_read_key();
        if (key == LEBUI_KEY_TAB || key == LEBUI_KEY_LEFT ||
            key == LEBUI_KEY_RIGHT)
            sel = !sel;
        else if (key == LEBUI_KEY_ENTER)
            return (sel == 0) ? 1 : 0;
        else if (key == LEBUI_KEY_ESC)
            return 0;
        else if (key == 'y' || key == 'Y')
            return 1;
        else if (key == 'n' || key == 'N')
            return 0;
    }
}

static void wiz_msgbox(const char *title, const char *msg)
{
    int bx, by, bw, bh;
    int key;

    wiz_stained = 1;
    wiz_dlg_frame(title, &bx, &by, &bw, &bh, msg);
    lebui_goto(by + 5, bx + bw / 2 - 4);
    printf("%s< OK >%s", LEBUI_CLR_BTN_SEL, LEBUI_CLR_NORMAL);
    lebui_flush();
    for (;;) {
        key = lebui_read_key();
        if (key == LEBUI_KEY_ENTER || key == LEBUI_KEY_ESC)
            return;
    }
}

static int wiz_input(const char *title, const char *prompt, char *buf,
                     int maxlen, int hidden)
{
    int bw, bh, bx, by, fw;
    int len = 0;
    int cur = 0;
    int key;
    int i;
    int start;
    int visible;
    int r;

    wiz_stained = 1;
    buf[0] = '\0';
    wiz_dlg_geo(&bx, &by, &bw, &bh);
    fw = bw - 6;
    if (fw < 1)
        return -1;
    lebui_draw_box_shadow(by, bx, bh, bw, title);
    for (r = 1; r < bh - 1; r++) {
        lebui_goto(by + r, bx + 1);
        printf("%s%-*s%s", LEBUI_CLR_MENU, bw - 2, "", LEBUI_CLR_NORMAL);
    }
    lebui_goto(by + 2, bx + 2);
    printf("%s%.*s%s", LEBUI_CLR_MENU, bw - 4, prompt, LEBUI_CLR_NORMAL);
    lebui_goto(by + 6, bx + bw / 2 - 4);
    printf("%s< OK >%s", LEBUI_CLR_BTN, LEBUI_CLR_NORMAL);
    lebui_flush();
    for (;;) {
        start = 0;
        if (cur >= fw)
            start = cur - fw + 1;
        visible = len - start;
        if (visible > fw)
            visible = fw;
        lebui_goto(by + 4, bx + 3);
        printf("%s", LEBUI_CLR_INPUT);
        if (hidden) {
            for (i = 0; i < visible; i++)
                putchar('*');
        } else {
            for (i = 0; i < visible; i++)
                putchar(buf[start + i]);
        }
        for (i = visible; i < fw; i++)
            putchar(' ');
        printf("%s", LEBUI_CLR_NORMAL);
        lebui_goto(by + 4, bx + 3 + cur - start);
        lebui_show_cursor();
        lebui_flush();
        key = lebui_read_key();
        lebui_hide_cursor();
        if (key == LEBUI_KEY_ENTER) {
            buf[len] = '\0';
            return len;
        } else if (key == LEBUI_KEY_ESC) {
            return -1;
        } else if (key == LEBUI_KEY_LEFT && cur > 0) {
            cur--;
        } else if (key == LEBUI_KEY_RIGHT && cur < len) {
            cur++;
        } else if (key == LEBUI_KEY_HOME) {
            cur = 0;
        } else if (key == LEBUI_KEY_END) {
            cur = len;
        } else if (key == LEBUI_KEY_BKSP && cur > 0) {
            memmove(&buf[cur - 1], &buf[cur], len - cur + 1);
            len--;
            cur--;
        } else if (key == LEBUI_KEY_DELETE && cur < len) {
            memmove(&buf[cur], &buf[cur + 1], len - cur);
            len--;
        } else if (key >= 32 && key < 127 && len < maxlen - 1) {
            memmove(&buf[cur + 1], &buf[cur], len - cur + 1);
            buf[cur] = (char)key;
            len++;
            cur++;
        }
        buf[len] = '\0';
    }
}

static int wiz_rootpw(void)
{
    char pw1[64];
    char pw2[64];

    for (;;) {
        if (wiz_input("Root Password", "Enter root password (empty=none):",
                      pw1, sizeof(pw1), 1) < 0)
            return -1;
        if (pw1[0] == '\0') {
            root_password[0] = '\0';
            return 0;
        }
        if (wiz_input("Root Password", "Confirm root password:", pw2,
                      sizeof(pw2), 1) < 0)
            return -1;
        if (strcmp(pw1, pw2) != 0) {
            wiz_msgbox("Error", "Passwords do not match.");
            continue;
        }
        strncpy(root_password, pw1, sizeof(root_password) - 1);
        root_password[sizeof(root_password) - 1] = '\0';
        memset(pw1, 0, sizeof(pw1));
        memset(pw2, 0, sizeof(pw2));
        return 0;
    }
}

static int wiz_add_user(void)
{
    char password2[64];
    int i;

    if (inst_reserve_users(user_count + 1) < 0) {
        wiz_msgbox("Error", "Could not allocate user entry.");
        return 0;
    }
    users[user_count].username[0] = '\0';
    users[user_count].password[0] = '\0';
    if (wiz_input("New User", "Enter username:", users[user_count].username,
                  sizeof(users[user_count].username), 0) < 0)
        return 0;
    if (users[user_count].username[0] == '\0') {
        wiz_msgbox("Error", "Username cannot be empty.");
        return 0;
    }
    for (i = 0; i < user_count; i++) {
        if (strcmp(users[i].username, users[user_count].username) == 0) {
            wiz_msgbox("Error", "That username already exists.");
            return 0;
        }
    }
    for (;;) {
        if (wiz_input("New User", "Enter password (empty=none):",
                      users[user_count].password,
                      sizeof(users[user_count].password), 1) < 0)
            return 0;
        if (users[user_count].password[0] == '\0') {
            user_count++;
            return 1;
        }
        if (wiz_input("New User", "Confirm password:", password2,
                       sizeof(password2), 1) < 0)
            break;
        if (strcmp(users[user_count].password, password2) != 0) {
            wiz_msgbox("Error", "Passwords do not match.");
            continue;
        }
        memset(password2, 0, sizeof(password2));
        user_count++;
        return 1;
    }
    memset(users[user_count].password, 0,
           sizeof(users[user_count].password));
    memset(password2, 0, sizeof(password2));
    return 0;
}

static int wiz_esp_part(int di, int n)
{
    int k;
    int c = 0;

    for (k = 0; k < disks[di].part_count; k++) {
        if (disks[di].parts[k].mbr_type != 0xEF)
            continue;
        if (c == n)
            return k;
        c++;
    }
    return -1;
}

static int wiz_esp_count(int di)
{
    int k;
    int c = 0;

    for (k = 0; k < disks[di].part_count; k++) {
        if (disks[di].parts[k].mbr_type == 0xEF)
            c++;
    }
    return c;
}

static int wiz_boot_ready(int di)
{
    int k;

    if (di < 0 || di >= disk_count)
        return 0;
    if (!inst_is_gpt(disks[di].devpath) && boot_mode == BOOT_BIOS)
        return 1;
    if (boot_esp[0] != '\0') {
        for (k = 0; k < disks[di].part_count; k++) {
            if (strcmp(disks[di].parts[k].devpath, boot_esp) == 0 &&
                disks[di].parts[k].mbr_type == 0xEF)
                return 1;
        }
        return 0;
    }
    return wiz_esp_count(di) > 0;
}

static void wiz_boot_short(int di, int gpt, char *buf, int bufsz)
{
    if (di < 0 || di >= disk_count) {
        buf[0] = '\0';
        return;
    }
    if (!gpt && boot_mode == BOOT_BIOS) {
        snprintf(buf, bufsz, "BIOS");
        return;
    }
    if (boot_esp[0] != '\0' && strncmp(boot_esp, "/dev/", 5) == 0)
        snprintf(buf, bufsz, "%s %s", boot_mode == BOOT_BOTH ? "B+U" : "UEFI",
                 boot_esp + 5);
    else if (boot_esp[0] != '\0')
        snprintf(buf, bufsz, "%s %s", boot_mode == BOOT_BOTH ? "B+U" : "UEFI",
                 boot_esp);
    else
        snprintf(buf, bufsz, "UEFI auto");
}

static void wiz_boot_summary(int di, int gpt, char *buf, int bufsz)
{
    if (di < 0 || di >= disk_count) {
        snprintf(buf, bufsz, "-");
        return;
    }
    if (gpt || boot_mode == BOOT_UEFI)
        snprintf(buf, bufsz, "UEFI via %s", boot_esp[0] ? boot_esp : "auto ESP");
    else if (boot_mode == BOOT_BOTH)
        snprintf(buf, bufsz, "BIOS+UEFI (%s)",
                 boot_esp[0] ? boot_esp : "auto ESP");
    else
        snprintf(buf, bufsz, "BIOS (MBR)");
}

static void wiz_frame_empty(int y, int x, int h, int w, const char *title)
{
    int i;
    int r;
    int tw;

    if (h < 2 || w < 2)
        return;
    lebui_goto(y, x);
    printf("%s+", LEBUI_CLR_BORDER);
    for (i = 0; i < w - 2; i++)
        putchar('-');
    putchar('+');
    if (title && w > 4) {
        tw = (int)strlen(title);
        if (tw > w - 4)
            tw = w - 4;
        lebui_goto(y, x + (w - tw - 2) / 2);
        printf("%s %.*s ", LEBUI_CLR_TITLE, tw, title);
    }
    for (r = 1; r < h - 1; r++) {
        lebui_goto(y + r, x);
        printf("%s|", LEBUI_CLR_BORDER);
        lebui_goto(y + r, x + w - 1);
        printf("%s|", LEBUI_CLR_BORDER);
    }
    lebui_goto(y + h - 1, x);
    printf("%s+", LEBUI_CLR_BORDER);
    for (i = 0; i < w - 2; i++)
        putchar('-');
    putchar('+');
    for (r = 1; r < h; r++) {
        lebui_goto(y + r, x + w);
        printf("%s ", LEBUI_CLR_SHADOW);
    }
    lebui_goto(y + h, x + 1);
    printf("%s", LEBUI_CLR_SHADOW);
    for (i = 0; i < w; i++)
        putchar(' ');
    printf("%s", LEBUI_CLR_NORMAL);
}

static int run_install_page(void)
{
    int disk_idx = -1;
    int part_idx = -1;
    int tz_idx = 12;
    int boot_done = 0;
    int boot_sub = 0;
    int disk_gpt = 0;
    int wel_done = 0;
    int pkgs_done = 0;
    int root_done = 0;
    int tz_done = 0;
    int cur = INS_WELCOME;
    int focus = WIZ_FOCUS_CONTENT;
    int side_cur = INS_WELCOME;
    int csel[INS_COUNT] = { 0 };
    int cscr[INS_COUNT] = { 0 };
    int tz_count = 0;

    while (timezones[tz_count])
        tz_count++;
    if (tz_idx >= tz_count)
        tz_idx = 0;
    boot_mode = BOOT_BIOS;
    boot_esp[0] = '\0';
    disk_gpt = 0;

    wiz_snap_t snap;
    int need_full = 1;
    int need_dirty = 0;
    int full;
    int stepch;

    snap.cur = -1;
    snap.focus = -1;
    snap.side = -1;
    snap.sel = -1;
    snap.scr = -1;

    for (;;) {
        int rows = term_sz.rows;
        int cols = term_sz.cols;
        int locked[INS_COUNT];
        int done[INS_COUNT];
        char status[INS_COUNT][24];
        int disk_ok;
        int part_ok;
        int i;
        int sy, sx, sh, sw, cy, cx, ch, cw;
        int ix, iw, iy, list_h, row;
        int nitems = 0;
        int infon = 0;
        int need_free = 0;
        char **draw_items = NULL;
        char (*draw_labels)[64] = NULL;
        int *draw_checks = NULL;
        const char *hdrs[2];
        char hdrbuf[2][64];
        int hdr_n = 0;
        char ctitle[48];
        char smbuf[8][64];
        char *smitems[8];
        char sum[12][64];
        const char *sums[12];
        int key;
        int pchk[PKG_COUNT];
        int bchk[3];
        int echk[16];

        disk_ok = disk_idx >= 0 && disk_idx < disk_count;
        part_ok = disk_ok && part_idx >= 0 &&
                  part_idx < disks[disk_idx].part_count;

        for (i = 0; i < INS_COUNT; i++) {
            locked[i] = 0;
            done[i] = 0;
            status[i][0] = '\0';
        }
        locked[INS_PART] = !disk_ok;
        locked[INS_BOOT] = !disk_ok;
        locked[INS_PKGS] = !part_ok;
        locked[INS_USER] = !part_ok;
        locked[INS_ROOTPW] = !part_ok;
        locked[INS_TZ] = !part_ok;
        locked[INS_SUMMARY] = !part_ok;
        locked[INS_INSTALL] = !part_ok;
        done[INS_WELCOME] = wel_done;
        done[INS_DISK] = disk_ok;
        done[INS_PART] = part_ok;
        done[INS_BOOT] = boot_done;
        done[INS_PKGS] = pkgs_done;
        done[INS_USER] = user_count > 0;
        done[INS_ROOTPW] = root_done;
        done[INS_TZ] = tz_done;

        if (disk_ok)
            snprintf(status[INS_DISK], sizeof(status[INS_DISK]), "%s",
                     disks[disk_idx].devpath);
        if (part_ok)
            snprintf(status[INS_PART], sizeof(status[INS_PART]), "%s",
                     disks[disk_idx].parts[part_idx].devpath);
        if (disk_ok)
            wiz_boot_short(disk_idx, disk_gpt, status[INS_BOOT],
                           sizeof(status[INS_BOOT]));
        snprintf(status[INS_PKGS], sizeof(status[INS_PKGS]), "core+%d",
                 pkg_selected[PKG_C_HDR] + pkg_selected[PKG_C_LIB]);
        if (user_count > 0)
            snprintf(status[INS_USER], sizeof(status[INS_USER]), "%d",
                     user_count);
        else
            snprintf(status[INS_USER], sizeof(status[INS_USER]), "none");
        if (root_done)
            snprintf(status[INS_ROOTPW], sizeof(status[INS_ROOTPW]), "%s",
                     root_password[0] ? "set" : "none");
        snprintf(status[INS_TZ], sizeof(status[INS_TZ]), "%s",
                 timezones[tz_idx]);

        wiz_panes(rows, cols, &sy, &sx, &sh, &sw, &cy, &cx, &ch, &cw);

        full = need_full;
        stepch = (!full && snap.cur != cur);
        need_full = 0;

        ix = cx + 2;
        iw = cw - 4;
        if (iw < 10)
            iw = 10;
        iy = cy + 2;

        if (full) {
            attach_tabbar(0, cols);
            lebui_draw_screen(NULL, WIZ_HELP, rows, cols);
        }
        if (full || stepch) {
            if (full)
                wiz_draw_sidebar(sy, sx, sh, sw, ins_wiz_names, status, INS_COUNT,
                                 cur, side_cur, focus, done, locked, "Install");
            else {
                for (i = 0; i < INS_COUNT; i++)
                    wiz_side_row(sy + 1 + i, sx, sw, i, ins_wiz_names, status,
                                 cur, side_cur, focus, done, locked);
                printf("%s", LEBUI_CLR_NORMAL);
            }
            snprintf(ctitle, sizeof(ctitle), "Step %d/%d: %s", cur + 1,
                     INS_COUNT, ins_wiz_names[cur]);
            if (full)
                lebui_draw_box_shadow(cy, cx, ch, cw, ctitle);
            else
                wiz_frame_empty(cy, cx, ch, cw, ctitle);
        }

        switch (cur) {
        case INS_WELCOME: {
            static const char *wlines[] = {
                "Welcome to the Lebirun installer.",
                "",
                "This wizard guides you through disk",
                "selection, packages, users and",
                "timezone, then installs the system.",
                "",
                "Pick a step on the left, or press",
                "Next to continue."
            };
            for (i = 0; i < 8; i++) {
                snprintf(sum[i], sizeof(sum[i]), "%s", wlines[i]);
                sums[i] = sum[i];
            }
            infon = 8;
            snprintf(smbuf[0], sizeof(smbuf[0]), "Continue");
            smitems[0] = smbuf[0];
            draw_items = smitems;
            nitems = 1;
            break;
        }
        case INS_DISK: {
            int k;

            draw_labels = malloc(((size_t)disk_count + 1) *
                                   sizeof(*draw_labels));
            draw_items = malloc(((size_t)disk_count + 1) *
                                sizeof(*draw_items));
            if (!draw_labels || !draw_items) {
                free(draw_labels);
                free(draw_items);
                draw_labels = NULL;
                draw_items = NULL;
                nitems = 0;
                break;
            }
            need_free = 1;
            for (k = 0; k < disk_count; k++) {
                wiz_disk_label(k, draw_labels[k],
                               sizeof(draw_labels[k]));
                draw_items[k] = draw_labels[k];
            }
            snprintf(draw_labels[disk_count],
                     sizeof(draw_labels[disk_count]), "[ Rescan disks ]");
            draw_items[disk_count] = draw_labels[disk_count];
            nitems = disk_count + 1;
            break;
        }
        case INS_PART: {
            int k;

            snprintf(hdrbuf[0], sizeof(hdrbuf[0]), "Disk: %s",
                     disk_ok ? disks[disk_idx].devpath : "-");
            hdrs[0] = hdrbuf[0];
            hdr_n = 1;
            if (!disk_ok) {
                nitems = 0;
                break;
            }
            if (disks[disk_idx].part_count == 0) {
                snprintf(smbuf[0], sizeof(smbuf[0]),
                         "(no partitions - use ldiskutil)");
                smitems[0] = smbuf[0];
                draw_items = smitems;
                nitems = 1;
                break;
            }
            draw_labels = malloc((size_t)disks[disk_idx].part_count *
                                 sizeof(*draw_labels));
            draw_items = malloc((size_t)disks[disk_idx].part_count *
                                sizeof(*draw_items));
            if (!draw_labels || !draw_items) {
                free(draw_labels);
                free(draw_items);
                draw_labels = NULL;
                draw_items = NULL;
                nitems = 0;
                break;
            }
            need_free = 1;
            for (k = 0; k < disks[disk_idx].part_count; k++) {
                wiz_part_label(disk_idx, k, draw_labels[k],
                               sizeof(draw_labels[k]));
                draw_items[k] = draw_labels[k];
            }
            nitems = disks[disk_idx].part_count;
            break;
        }
        case INS_BOOT: {
            int k;
            int nesp;
            int gpt;

            if (!disk_ok) {
                nitems = 0;
                break;
            }
            gpt = disk_gpt;
            snprintf(hdrbuf[0], sizeof(hdrbuf[0]), "Table: %s",
                     gpt ? "GPT" : "MBR");
            hdrs[0] = hdrbuf[0];
            hdr_n = 1;
            nesp = wiz_esp_count(disk_idx);
            if (!gpt && boot_sub == 0) {
                int n = 0;
                snprintf(smbuf[n], sizeof(smbuf[n]),
                         "BIOS boot (GRUB on disk)");
                smitems[n] = smbuf[n];
                bchk[n] = (boot_mode == BOOT_BIOS);
                n++;
                if (nesp > 0) {
                    snprintf(smbuf[n], sizeof(smbuf[n]),
                             "UEFI boot (from ESP)");
                    smitems[n] = smbuf[n];
                    bchk[n] = (boot_mode == BOOT_UEFI);
                    n++;
                    snprintf(smbuf[n], sizeof(smbuf[n]),
                             "BIOS + UEFI (both)");
                    smitems[n] = smbuf[n];
                    bchk[n] = (boot_mode == BOOT_BOTH);
                    n++;
                }
                draw_items = smitems;
                draw_checks = bchk;
                nitems = n;
            } else {
                if (nesp == 0) {
                    snprintf(smbuf[0], sizeof(smbuf[0]),
                             "(no ESP - create one with ldiskutil)");
                    smitems[0] = smbuf[0];
                    draw_items = smitems;
                    nitems = 1;
                } else {
                    if (nesp > 16)
                        nesp = 16;
                    draw_labels = malloc((size_t)nesp * sizeof(*draw_labels));
                    draw_items = malloc((size_t)nesp * sizeof(*draw_items));
                    if (!draw_labels || !draw_items) {
                        free(draw_labels);
                        free(draw_items);
                        draw_labels = NULL;
                        draw_items = NULL;
                        nitems = 0;
                        break;
                    }
                    need_free = 1;
                    for (k = 0; k < nesp; k++) {
                        int pi = wiz_esp_part(disk_idx, k);
                        wiz_part_label(disk_idx, pi, draw_labels[k],
                                       sizeof(draw_labels[k]));
                        draw_items[k] = draw_labels[k];
                        if (boot_esp[0] != '\0')
                            echk[k] = (strcmp(disks[disk_idx].parts[pi].devpath,
                                              boot_esp) == 0);
                        else
                            echk[k] = (k == 0);
                    }
                    draw_checks = echk;
                    nitems = nesp;
                }
            }
            break;
        }
        case INS_PKGS: {
            static const char *pnames[] = {
                "Core system (required)",
                "C development headers",
                "C development libraries"
            };
            snprintf(hdrbuf[0], sizeof(hdrbuf[0]),
                     "Space toggles, Enter confirms.");
            hdrs[0] = hdrbuf[0];
            hdr_n = 1;
            for (i = 0; i < PKG_COUNT; i++) {
                snprintf(smbuf[i], sizeof(smbuf[i]), "%s", pnames[i]);
                smitems[i] = smbuf[i];
            }
            draw_items = smitems;
            pchk[0] = -1;
            pchk[1] = pkg_selected[PKG_C_HDR];
            pchk[2] = pkg_selected[PKG_C_LIB];
            draw_checks = pchk;
            nitems = PKG_COUNT;
            break;
        }
        case INS_USER: {
            int k;

            snprintf(hdrbuf[0], sizeof(hdrbuf[0]), "Users: %d (optional)",
                     user_count);
            hdrs[0] = hdrbuf[0];
            hdr_n = 1;
            draw_labels = malloc(((size_t)user_count + 1) *
                                 sizeof(*draw_labels));
            draw_items = malloc(((size_t)user_count + 1) *
                                sizeof(*draw_items));
            if (!draw_labels || !draw_items) {
                free(draw_labels);
                free(draw_items);
                draw_labels = NULL;
                draw_items = NULL;
                nitems = 0;
                break;
            }
            need_free = 1;
            for (k = 0; k < user_count; k++) {
                snprintf(draw_labels[k], sizeof(draw_labels[k]), "%s",
                         users[k].username);
                draw_items[k] = draw_labels[k];
            }
            snprintf(draw_labels[user_count],
                     sizeof(draw_labels[user_count]), "Add new user...");
            draw_items[user_count] = draw_labels[user_count];
            nitems = user_count + 1;
            break;
        }
        case INS_ROOTPW:
            snprintf(hdrbuf[0], sizeof(hdrbuf[0]), "Status: %s",
                     root_password[0] ? "set" : "none (optional)");
            hdrs[0] = hdrbuf[0];
            hdr_n = 1;
            snprintf(smbuf[0], sizeof(smbuf[0]),
                     "Set / change root password...");
            snprintf(smbuf[1], sizeof(smbuf[1]),
                     "Clear password (no password)");
            smitems[0] = smbuf[0];
            smitems[1] = smbuf[1];
            draw_items = smitems;
            nitems = 2;
            break;
        case INS_TZ: {
            int k;

            draw_labels = malloc((size_t)tz_count * sizeof(*draw_labels));
            draw_items = malloc((size_t)tz_count * sizeof(*draw_items));
            if (!draw_labels || !draw_items) {
                free(draw_labels);
                free(draw_items);
                draw_labels = NULL;
                draw_items = NULL;
                nitems = 0;
                break;
            }
            need_free = 1;
            for (k = 0; k < tz_count; k++) {
                snprintf(draw_labels[k], sizeof(draw_labels[k]), "%s%s",
                         timezones[k], k == tz_idx ? "  (current)" : "");
                draw_items[k] = draw_labels[k];
            }
            nitems = tz_count;
            break;
        }
        case INS_SUMMARY: {
            char ulist[64];
            int ulen = 0;
            int sn = 0;
            int k;

            ulist[0] = '\0';
            if (user_count == 0) {
                snprintf(ulist, sizeof(ulist), "(none)");
            } else {
                for (k = 0; k < user_count; k++) {
                    int w = snprintf(ulist + ulen, sizeof(ulist) - ulen,
                                     "%s%s", k ? ", " : "",
                                     users[k].username);
                    if (w < 0 || w >= (int)sizeof(ulist) - ulen)
                        break;
                    ulen += w;
                }
            }
            snprintf(sum[sn], sizeof(sum[sn]), "Review your choices.");
            sums[sn] = sum[sn];
            sn++;
            snprintf(sum[sn], sizeof(sum[sn]), " ");
            sums[sn] = sum[sn];
            sn++;
            snprintf(sum[sn], sizeof(sum[sn]), "Disk:      %s",
                     disk_ok ? disks[disk_idx].devpath : "-");
            sums[sn] = sum[sn];
            sn++;
            snprintf(sum[sn], sizeof(sum[sn]), "Partition: %s",
                     part_ok ? disks[disk_idx].parts[part_idx].devpath : "-");
            sums[sn] = sum[sn];
            sn++;
            snprintf(sum[sn], sizeof(sum[sn]), "Format:    ext4 (erases data)");
            sums[sn] = sum[sn];
            sn++;
            snprintf(sum[sn], sizeof(sum[sn]), "Packages:  core + %d extra",
                     pkg_selected[PKG_C_HDR] + pkg_selected[PKG_C_LIB]);
            sums[sn] = sum[sn];
            sn++;
            snprintf(sum[sn], sizeof(sum[sn]), "Users:     %s", ulist);
            sums[sn] = sum[sn];
            sn++;
            snprintf(sum[sn], sizeof(sum[sn]), "Root:      %s",
                     root_done ? (root_password[0] ? "set" : "none") : "none");
            sums[sn] = sum[sn];
            sn++;
            snprintf(sum[sn], sizeof(sum[sn]), "Timezone:  %s",
                     timezones[tz_idx]);
            sums[sn] = sum[sn];
            sn++;
            wiz_boot_summary(disk_idx, disk_gpt, hdrbuf[0], sizeof(hdrbuf[0]));
            snprintf(sum[sn], sizeof(sum[sn]), "Boot:      %s", hdrbuf[0]);
            sums[sn] = sum[sn];
            sn++;
            infon = sn;
            snprintf(smbuf[0], sizeof(smbuf[0]), "Continue to Install");
            smitems[0] = smbuf[0];
            draw_items = smitems;
            nitems = 1;
            break;
        }
        case INS_INSTALL:
            snprintf(sum[0], sizeof(sum[0]), "Ready to install Lebirun.");
            sums[0] = sum[0];
            snprintf(sum[1], sizeof(sum[1]), " ");
            sums[1] = sum[1];
            snprintf(sum[2], sizeof(sum[2]), "Target: %s on %s",
                     part_ok ? disks[disk_idx].parts[part_idx].devpath : "-",
                     disk_ok ? disks[disk_idx].devpath : "-");
            sums[2] = sum[2];
            snprintf(sum[3], sizeof(sum[3]), "Format:  ext4 (erases all data)");
            sums[3] = sum[3];
            wiz_boot_summary(disk_idx, disk_gpt, hdrbuf[0], sizeof(hdrbuf[0]));
            snprintf(sum[4], sizeof(sum[4]), "Boot: %s", hdrbuf[0]);
            sums[4] = sum[4];
            snprintf(sum[5], sizeof(sum[5]), " ");
            sums[5] = sum[5];
            snprintf(sum[6], sizeof(sum[6]), "Select Start to begin.");
            sums[6] = sum[6];
            infon = 7;
            snprintf(smbuf[0], sizeof(smbuf[0]), "Start installation");
            smitems[0] = smbuf[0];
            draw_items = smitems;
            nitems = 1;
            break;
        default:
            nitems = 0;
            break;
        }

        row = iy;
        if (full || stepch || need_dirty || wiz_stained) {
            if (infon > 0)
                row = wiz_draw_info(iy, ix, iw, sums, infon) + 1;
            else if (hdr_n > 0)
                row = wiz_draw_info(iy, ix, iw, hdrs, hdr_n) + 1;
        } else {
            if (infon > 0)
                row = iy + infon + 1;
            else if (hdr_n > 0)
                row = iy + hdr_n + 1;
        }
        if (nitems <= 0) {
            csel[cur] = 0;
            cscr[cur] = 0;
        } else {
            if (csel[cur] >= nitems)
                csel[cur] = nitems - 1;
            if (csel[cur] < 0)
                csel[cur] = 0;
        }
        list_h = cy + ch - 3 - row;
        if (list_h < 1)
            list_h = 1;
        if (cscr[cur] > csel[cur])
            cscr[cur] = csel[cur];
        if (cscr[cur] < csel[cur] - list_h + 1)
            cscr[cur] = csel[cur] - list_h + 1;
        if (cscr[cur] < 0)
            cscr[cur] = 0;
        if (full || stepch || need_dirty || wiz_stained) {
            if (!full && !stepch) {
                lebui_goto(iy - 1, cx + 1);
                printf("%s%-*s%s", LEBUI_CLR_MENU, cw - 4, "", LEBUI_CLR_NORMAL);
            }
            wiz_draw_rows(row, ix, list_h, iw, draw_items, draw_checks, nitems,
                          csel[cur], cscr[cur], focus);
            need_dirty = 0;
            wiz_stained = 0;
        } else if (snap.focus != focus) {
            wiz_content_row(row + csel[cur] - cscr[cur], ix, iw, draw_items,
                            draw_checks, csel[cur], nitems, csel[cur], focus);
            wiz_content_row(row + snap.sel - snap.scr, ix, iw, draw_items,
                            draw_checks, snap.sel, nitems, csel[cur], focus);
            wiz_side_row(sy + 1 + snap.side, sx, sw, snap.side, ins_wiz_names, status,
                         cur, side_cur, focus, done, locked);
            wiz_side_row(sy + 1 + side_cur, sx, sw, side_cur, ins_wiz_names, status,
                         cur, side_cur, focus, done, locked);
            wiz_side_row(sy + 1 + cur, sx, sw, cur, ins_wiz_names, status,
                         cur, side_cur, focus, done, locked);
            printf("%s", LEBUI_CLR_NORMAL);
        } else if (focus == WIZ_FOCUS_CONTENT) {
            if (snap.scr != cscr[cur]) {
                wiz_draw_rows(row, ix, list_h, iw, draw_items, draw_checks,
                              nitems, csel[cur], cscr[cur], focus);
            } else {
                wiz_content_row(row + snap.sel - cscr[cur], ix, iw, draw_items,
                                draw_checks, snap.sel, nitems, csel[cur],
                                focus);
                wiz_content_row(row + csel[cur] - cscr[cur], ix, iw,
                                draw_items, draw_checks, csel[cur], nitems,
                                csel[cur], focus);
                printf("%s", LEBUI_CLR_NORMAL);
            }
        } else {
            wiz_side_row(sy + 1 + snap.side, sx, sw, snap.side, ins_wiz_names, status,
                         cur, side_cur, focus, done, locked);
            wiz_side_row(sy + 1 + side_cur, sx, sw, side_cur, ins_wiz_names, status,
                         cur, side_cur, focus, done, locked);
            printf("%s", LEBUI_CLR_NORMAL);
        }
        if (full || stepch) {
            lebui_goto(cy + ch - 2, cx + 2);
            printf("%s b:Back n:Next  %d/%d  %d/%d%s", LEBUI_CLR_DIM_CLR,
                   cur + 1, INS_COUNT, nitems ? csel[cur] + 1 : 0, nitems,
                   LEBUI_CLR_NORMAL);
        }
        printf("%s", LEBUI_CLR_NORMAL);
        lebui_flush();
        if (need_free) {
            free(draw_labels);
            free(draw_items);
        }

        snap.cur = cur;
        snap.focus = focus;
        snap.side = side_cur;
        snap.sel = csel[cur];
        snap.scr = cscr[cur];

        key = lebui_read_key();

        if (key == LEBUI_KEY_TAB)
            return LEBUI_KEY_TAB;
        if (key == LEBUI_KEY_UP) {
            if (focus == WIZ_FOCUS_STEPS) {
                if (side_cur > 0)
                    side_cur--;
            } else if (csel[cur] > 0) {
                csel[cur]--;
            }
        } else if (key == LEBUI_KEY_DOWN) {
            if (focus == WIZ_FOCUS_STEPS) {
                if (side_cur < INS_COUNT - 1)
                    side_cur++;
            } else if (csel[cur] < nitems - 1) {
                csel[cur]++;
            }
        } else if (key == LEBUI_KEY_PGUP) {
            if (focus == WIZ_FOCUS_STEPS) {
                side_cur -= list_h;
                if (side_cur < 0)
                    side_cur = 0;
            } else {
                csel[cur] -= list_h;
                if (csel[cur] < 0)
                    csel[cur] = 0;
            }
        } else if (key == LEBUI_KEY_PGDN) {
            if (focus == WIZ_FOCUS_STEPS) {
                side_cur += list_h;
                if (side_cur > INS_COUNT - 1)
                    side_cur = INS_COUNT - 1;
            } else {
                csel[cur] += list_h;
                if (csel[cur] > nitems - 1)
                    csel[cur] = nitems - 1;
            }
        } else if (key == LEBUI_KEY_HOME) {
            if (focus == WIZ_FOCUS_STEPS)
                side_cur = 0;
            else
                csel[cur] = 0;
        } else if (key == LEBUI_KEY_END) {
            if (focus == WIZ_FOCUS_STEPS)
                side_cur = INS_COUNT - 1;
            else
                csel[cur] = nitems - 1;
        } else if (key == LEBUI_KEY_LEFT) {
            if (focus == WIZ_FOCUS_CONTENT) {
                focus = WIZ_FOCUS_STEPS;
                side_cur = cur;
            } else if (cur == INS_BOOT && boot_sub == 1) {
                boot_sub = 0;
                need_dirty = 1;
            } else {
                int j = cur - 1;
                while (j > 0 && locked[j])
                    j--;
                cur = j;
                focus = WIZ_FOCUS_CONTENT;
                side_cur = cur;
            }
        } else if (key == LEBUI_KEY_RIGHT) {
            if (focus == WIZ_FOCUS_STEPS) {
                focus = WIZ_FOCUS_CONTENT;
            } else {
                int j = cur + 1;
                while (j < INS_COUNT && locked[j])
                    j++;
                if (j < INS_COUNT) {
                    if (cur == INS_WELCOME)
                        wel_done = 1;
                    cur = j;
                    focus = WIZ_FOCUS_CONTENT;
                    side_cur = cur;
                } else if (cur == INS_INSTALL && part_ok) {
                    goto try_install;
                } else if (cur == INS_DISK) {
                    wiz_msgbox("Error", "Select a disk first.");
                } else if (cur == INS_PART) {
                    wiz_msgbox("Error", "Select a partition first.");
                }
            }
        } else if (key == 'b' || key == 'B' || key == LEBUI_KEY_BKSP) {
            if (cur == INS_BOOT && boot_sub == 1) {
                boot_sub = 0;
                need_dirty = 1;
            } else if (cur > 0) {
                int j = cur - 1;
                while (j > 0 && locked[j])
                    j--;
                cur = j;
                focus = WIZ_FOCUS_CONTENT;
                side_cur = cur;
            }
        } else if (key == 'n' || key == 'N') {
            int j = cur + 1;
            while (j < INS_COUNT && locked[j])
                j++;
            if (j < INS_COUNT) {
                if (cur == INS_WELCOME)
                    wel_done = 1;
                cur = j;
                focus = WIZ_FOCUS_CONTENT;
                side_cur = cur;
            } else if (cur == INS_INSTALL && part_ok) {
                goto try_install;
            } else if (cur == INS_DISK) {
                wiz_msgbox("Error", "Select a disk first.");
            } else if (cur == INS_PART) {
                wiz_msgbox("Error", "Select a partition first.");
            }
        } else if (key == ' ') {
            if (focus == WIZ_FOCUS_CONTENT && cur == INS_PKGS) {
                int s = csel[cur];
                if (s == PKG_C_HDR || s == PKG_C_LIB) {
                    pkg_selected[s] = !pkg_selected[s];
                    pkgs_done = 1;
                    need_dirty = 1;
                }
            }
        } else if ((key == 'r' || key == 'R') && cur == INS_DISK) {
            focus = WIZ_FOCUS_CONTENT;
            csel[cur] = nitems - 1;
        } else if (key == LEBUI_KEY_ENTER) {
            int ret;
            if (focus == WIZ_FOCUS_STEPS) {
                if (locked[side_cur]) {
                    wiz_msgbox("Locked",
                                      "Finish the earlier steps first.");
                } else {
                    cur = side_cur;
                    focus = WIZ_FOCUS_CONTENT;
                }
                continue;
            }
            switch (cur) {
            case INS_WELCOME:
                wel_done = 1;
                cur = INS_DISK;
                break;
            case INS_DISK:
                if (csel[cur] < disk_count) {
                    if (csel[cur] != disk_idx) {
                        disk_idx = csel[cur];
                        part_idx = -1;
                        csel[INS_PART] = 0;
                        cscr[INS_PART] = 0;
                        disk_gpt = inst_is_gpt(disks[disk_idx].devpath);
                        boot_mode = disk_gpt ? BOOT_UEFI : BOOT_BIOS;
                        boot_esp[0] = '\0';
                        boot_done = 0;
                        boot_sub = 0;
                    }
                    cur = INS_PART;
                } else {
                    char rmsg[64];
                    int n = wiz_rescan(&disk_idx, &part_idx);
                    if (disk_idx < 0) {
                        cur = INS_DISK;
                        boot_mode = BOOT_BIOS;
                        disk_gpt = 0;
                        boot_esp[0] = '\0';
                        boot_done = 0;
                        boot_sub = 0;
                    } else if (part_idx < 0 && cur > INS_PART) {
                        cur = INS_PART;
                    }
                    if (disk_idx >= 0 && boot_esp[0] != '\0') {
                        int k;
                        int kept = 0;
                        for (k = 0; k < disks[disk_idx].part_count; k++) {
                            if (strcmp(disks[disk_idx].parts[k].devpath, boot_esp) == 0 &&
                                disks[disk_idx].parts[k].mbr_type == 0xEF)
                                kept = 1;
                        }
                        if (!kept) {
                            boot_esp[0] = '\0';
                            boot_done = 0;
                            if (cur > INS_BOOT)
                                cur = INS_BOOT;
                        }
                    }
                    if (disk_idx >= 0)
                        disk_gpt = inst_is_gpt(disks[disk_idx].devpath);
                    snprintf(rmsg, sizeof(rmsg), "Found %d disk(s).", n);
                    wiz_msgbox("Rescan", rmsg);
                }
                break;
            case INS_PART:
                if (!disk_ok || disks[disk_idx].part_count == 0) {
                    wiz_msgbox("No Partitions",
                                      "No partitions here. Use ldiskutil.");
                } else if (disks[disk_idx].parts[csel[cur]].mbr_type == 0xEF) {
                    wiz_msgbox("Error",
                                      "The ESP holds the bootloader.\nUse a Lebirun partition or make one first.");
                } else {
                    part_idx = csel[cur];
                    cur = INS_BOOT;
                }
                break;
            case INS_BOOT: {
                int gpt = disk_ok && disk_gpt;
                int nesp = disk_ok ? wiz_esp_count(disk_idx) : 0;
                if (!disk_ok)
                    break;
                if (!gpt && boot_sub == 0) {
                    if (csel[cur] == 0) {
                        boot_mode = BOOT_BIOS;
                        boot_esp[0] = '\0';
                        boot_done = 1;
                        cur = INS_PKGS;
                    } else {
                        boot_mode = (csel[cur] == 1) ? BOOT_UEFI : BOOT_BOTH;
                        if (nesp == 1) {
                            int pi = wiz_esp_part(disk_idx, 0);
                            snprintf(boot_esp, sizeof(boot_esp), "%s",
                                     disks[disk_idx].parts[pi].devpath);
                            boot_done = 1;
                            cur = INS_PKGS;
                        } else {
                            boot_sub = 1;
                            need_dirty = 1;
                        }
                    }
                } else {
                    if (nesp == 0) {
                        wiz_msgbox("No ESP",
                                   "Create an ESP with ldiskutil first.\nThen rescan from the Disk step.");
                    } else if (csel[cur] < nesp) {
                        int pi = wiz_esp_part(disk_idx, csel[cur]);
                        snprintf(boot_esp, sizeof(boot_esp), "%s",
                                 disks[disk_idx].parts[pi].devpath);
                        if (gpt)
                            boot_mode = BOOT_UEFI;
                        boot_done = 1;
                        cur = INS_PKGS;
                    }
                }
                break;
            }
            case INS_PKGS:
                pkgs_done = 1;
                cur = INS_USER;
                break;
            case INS_USER:
                if (csel[cur] < user_count) {
                    if (wiz_confirm("Remove User",
                                           users[csel[cur]].username)) {
                        int k;
                        for (k = csel[cur]; k < user_count - 1; k++)
                            users[k] = users[k + 1];
                        user_count--;
                        memset(&users[user_count], 0,
                               sizeof(users[user_count]));
                        if (csel[cur] >= user_count && csel[cur] > 0)
                            csel[cur]--;
                    }
                } else {
                    wiz_add_user();
                }
                break;
            case INS_ROOTPW:
                if (csel[cur] == 0) {
                    if (wiz_rootpw() == 0)
                        root_done = 1;
                } else {
                    if (wiz_confirm("Root Password",
                                           "Clear the root password?")) {
                        memset(root_password, 0, sizeof(root_password));
                        root_done = 1;
                    }
                }
                break;
            case INS_TZ:
                tz_idx = csel[cur];
                tz_done = 1;
                cur = INS_SUMMARY;
                break;
            case INS_SUMMARY:
                cur = INS_INSTALL;
                break;
            case INS_INSTALL:
            try_install:
                if (!wiz_boot_ready(disk_idx)) {
                    wiz_msgbox("Boot", "UEFI boot needs an ESP.\nCreate one with ldiskutil first.");
                    break;
                }
                if (wiz_confirm("Confirm",
                                       "Proceed with installation?")) {
                    ret = step_do_install(disk_idx, part_idx, 1,
                                          tz_idx);
                    need_full = 1;
                    if (ret == 0) {
                        for (i = 0; i < user_count; i++)
                            memset(users[i].password, 0,
                                   sizeof(users[i].password));
                        memset(root_password, 0, sizeof(root_password));
                        return 0;
                    }
                }
                break;
            }
            side_cur = cur;
        } else if (key == LEBUI_KEY_ESC || key == 'q' || key == 'Q') {
            if (cur == INS_BOOT && boot_sub == 1) {
                boot_sub = 0;
                need_dirty = 1;
            } else if (cur == INS_WELCOME) {
                if (wiz_confirm("Quit", "Exit the installer?"))
                    return -1;
            } else {
                int j = cur - 1;
                while (j > 0 && locked[j])
                    j--;
                cur = j;
                focus = WIZ_FOCUS_CONTENT;
                side_cur = cur;
            }
        }
    }
}

static const char *uw_item_names[] = {
    "Core system files (/bin, /lib, /sbin, /init)",
    "Kernel and boot files (/boot)",
    "Development headers (/usr/include)",
    "Terminal database (/usr/share/terminfo)",
    "Package database records",
    "GRUB boot code and modules",
    "GRUB configuration file"
};

static int run_update_page(void)
{
    int disk_idx = -1;
    int part_idx = -1;
    int wel_done = 0;
    int cur = UW_WELCOME;
    int focus = WIZ_FOCUS_CONTENT;
    int side_cur = UW_WELCOME;
    int csel[UW_COUNT] = { 0 };
    int cscr[UW_COUNT] = { 0 };

    wiz_snap_t snap;
    int need_full = 1;
    int need_dirty = 0;
    int full;
    int stepch;

    snap.cur = -1;
    snap.focus = -1;
    snap.side = -1;
    snap.sel = -1;
    snap.scr = -1;

    for (;;) {
        int rows = term_sz.rows;
        int cols = term_sz.cols;
        int locked[UW_COUNT];
        int done[UW_COUNT];
        char status[UW_COUNT][24];
        int disk_ok;
        int part_ok;
        int i;
        int sy, sx, sh, sw, cy, cx, ch, cw;
        int ix, iw, iy, list_h, row;
        int nitems = 0;
        int infon = 0;
        int need_free = 0;
        char **draw_items = NULL;
        char (*draw_labels)[64] = NULL;
        int *draw_checks = NULL;
        const char *hdrs[2];
        char hdrbuf[2][64];
        int hdr_n = 0;
        char ctitle[48];
        char smbuf[8][64];
        char *smitems[8];
        char sum[12][64];
        const char *sums[12];
        int key;
        int uchk[UPD_COUNT];
        int sel_count = 0;

        disk_ok = disk_idx >= 0 && disk_idx < disk_count;
        part_ok = disk_ok && part_idx >= 0 &&
                  part_idx < disks[disk_idx].part_count;

        for (i = 0; i < UW_COUNT; i++) {
            locked[i] = 0;
            done[i] = 0;
            status[i][0] = '\0';
        }
        locked[UW_PART] = !disk_ok;
        locked[UW_ITEMS] = !part_ok;
        locked[UW_SUMMARY] = !part_ok;
        locked[UW_DO] = !part_ok;
        done[UW_WELCOME] = wel_done;
        done[UW_DISK] = disk_ok;
        done[UW_PART] = part_ok;
        done[UW_ITEMS] = 1;

        for (i = 0; i < UPD_COUNT; i++) {
            if (upd_selected[i])
                sel_count++;
        }

        if (disk_ok)
            snprintf(status[UW_DISK], sizeof(status[UW_DISK]), "%s",
                     disks[disk_idx].devpath);
        if (part_ok)
            snprintf(status[UW_PART], sizeof(status[UW_PART]), "%s",
                     disks[disk_idx].parts[part_idx].devpath);
        snprintf(status[UW_ITEMS], sizeof(status[UW_ITEMS]), "%d item(s)",
                 sel_count);

        wiz_panes(rows, cols, &sy, &sx, &sh, &sw, &cy, &cx, &ch, &cw);

        full = need_full;
        stepch = (!full && snap.cur != cur);
        need_full = 0;

        ix = cx + 2;
        iw = cw - 4;
        if (iw < 10)
            iw = 10;
        iy = cy + 2;

        if (full) {
            attach_tabbar(1, cols);
            lebui_draw_screen(NULL, WIZ_HELP, rows, cols);
        }
        if (full || stepch) {
            if (full)
                wiz_draw_sidebar(sy, sx, sh, sw, uw_wiz_names, status, UW_COUNT,
                                 cur, side_cur, focus, done, locked, "Update");
            else {
                for (i = 0; i < UW_COUNT; i++)
                    wiz_side_row(sy + 1 + i, sx, sw, i, uw_wiz_names, status,
                                 cur, side_cur, focus, done, locked);
                printf("%s", LEBUI_CLR_NORMAL);
            }
            snprintf(ctitle, sizeof(ctitle), "Step %d/%d: %s", cur + 1,
                     UW_COUNT, uw_wiz_names[cur]);
            if (full)
                lebui_draw_box_shadow(cy, cx, ch, cw, ctitle);
            else
                wiz_frame_empty(cy, cx, ch, cw, ctitle);
        }

        switch (cur) {
        case UW_WELCOME: {
            static const char *wlines[] = {
                "Welcome to the Lebirun updater.",
                "",
                "This wizard updates an installed",
                "system. User data and local config",
                "are preserved.",
                "",
                "Pick a step on the left, or press",
                "Next to continue."
            };
            for (i = 0; i < 8; i++) {
                snprintf(sum[i], sizeof(sum[i]), "%s", wlines[i]);
                sums[i] = sum[i];
            }
            infon = 8;
            snprintf(smbuf[0], sizeof(smbuf[0]), "Continue");
            smitems[0] = smbuf[0];
            draw_items = smitems;
            nitems = 1;
            break;
        }
        case UW_DISK: {
            int k;

            draw_labels = malloc(((size_t)disk_count + 1) *
                                 sizeof(*draw_labels));
            draw_items = malloc(((size_t)disk_count + 1) *
                                sizeof(*draw_items));
            if (!draw_labels || !draw_items) {
                free(draw_labels);
                free(draw_items);
                draw_labels = NULL;
                draw_items = NULL;
                nitems = 0;
                break;
            }
            need_free = 1;
            for (k = 0; k < disk_count; k++) {
                wiz_disk_label(k, draw_labels[k],
                               sizeof(draw_labels[k]));
                draw_items[k] = draw_labels[k];
            }
            snprintf(draw_labels[disk_count],
                     sizeof(draw_labels[disk_count]), "[ Rescan disks ]");
            draw_items[disk_count] = draw_labels[disk_count];
            nitems = disk_count + 1;
            break;
        }
        case UW_PART: {
            int k;

            snprintf(hdrbuf[0], sizeof(hdrbuf[0]), "Disk: %s",
                     disk_ok ? disks[disk_idx].devpath : "-");
            hdrs[0] = hdrbuf[0];
            hdr_n = 1;
            if (!disk_ok) {
                nitems = 0;
                break;
            }
            if (disks[disk_idx].part_count == 0) {
                snprintf(smbuf[0], sizeof(smbuf[0]),
                         "(no partitions on this disk)");
                smitems[0] = smbuf[0];
                draw_items = smitems;
                nitems = 1;
                break;
            }
            draw_labels = malloc((size_t)disks[disk_idx].part_count *
                                 sizeof(*draw_labels));
            draw_items = malloc((size_t)disks[disk_idx].part_count *
                                sizeof(*draw_items));
            if (!draw_labels || !draw_items) {
                free(draw_labels);
                free(draw_items);
                draw_labels = NULL;
                draw_items = NULL;
                nitems = 0;
                break;
            }
            need_free = 1;
            for (k = 0; k < disks[disk_idx].part_count; k++) {
                wiz_part_label(disk_idx, k, draw_labels[k],
                               sizeof(draw_labels[k]));
                draw_items[k] = draw_labels[k];
            }
            nitems = disks[disk_idx].part_count;
            break;
        }
        case UW_ITEMS:
            snprintf(hdrbuf[0], sizeof(hdrbuf[0]),
                     "Space toggles, Enter confirms.");
            hdrs[0] = hdrbuf[0];
            hdr_n = 1;
            for (i = 0; i < UPD_COUNT; i++) {
                snprintf(smbuf[i], sizeof(smbuf[i]), "%s", uw_item_names[i]);
                smitems[i] = smbuf[i];
                uchk[i] = upd_selected[i];
            }
            draw_items = smitems;
            draw_checks = uchk;
            nitems = UPD_COUNT;
            break;
        case UW_SUMMARY: {
            int sn = 0;

            snprintf(sum[sn], sizeof(sum[sn]), "Review the update.");
            sums[sn] = sum[sn];
            sn++;
            snprintf(sum[sn], sizeof(sum[sn]), " ");
            sums[sn] = sum[sn];
            sn++;
            snprintf(sum[sn], sizeof(sum[sn]), "Target: %s on %s",
                     part_ok ? disks[disk_idx].parts[part_idx].devpath : "-",
                     disk_ok ? disks[disk_idx].devpath : "-");
            sums[sn] = sum[sn];
            sn++;
            snprintf(sum[sn], sizeof(sum[sn]), "Items:  %d selected",
                     sel_count);
            sums[sn] = sum[sn];
            sn++;
            snprintf(sum[sn], sizeof(sum[sn]),
                     "User data and config preserved.");
            sums[sn] = sum[sn];
            sn++;
            infon = sn;
            snprintf(smbuf[0], sizeof(smbuf[0]), "Continue to Update");
            smitems[0] = smbuf[0];
            draw_items = smitems;
            nitems = 1;
            break;
        }
        case UW_DO:
            snprintf(sum[0], sizeof(sum[0]), "Ready to update Lebirun.");
            sums[0] = sum[0];
            snprintf(sum[1], sizeof(sum[1]), " ");
            sums[1] = sum[1];
            snprintf(sum[2], sizeof(sum[2]), "Target: %s on %s",
                     part_ok ? disks[disk_idx].parts[part_idx].devpath : "-",
                     disk_ok ? disks[disk_idx].devpath : "-");
            sums[2] = sum[2];
            snprintf(sum[3], sizeof(sum[3]), "Items:  %d selected",
                     sel_count);
            sums[3] = sum[3];
            snprintf(sum[4], sizeof(sum[4]), " ");
            sums[4] = sum[4];
            snprintf(sum[5], sizeof(sum[5]), "Select Start to begin.");
            sums[5] = sum[5];
            infon = 6;
            snprintf(smbuf[0], sizeof(smbuf[0]), "Start update");
            smitems[0] = smbuf[0];
            draw_items = smitems;
            nitems = 1;
            break;
        default:
            nitems = 0;
            break;
        }

        row = iy;
        if (full || stepch || need_dirty || wiz_stained) {
            if (infon > 0)
                row = wiz_draw_info(iy, ix, iw, sums, infon) + 1;
            else if (hdr_n > 0)
                row = wiz_draw_info(iy, ix, iw, hdrs, hdr_n) + 1;
        } else {
            if (infon > 0)
                row = iy + infon + 1;
            else if (hdr_n > 0)
                row = iy + hdr_n + 1;
        }
        if (nitems <= 0) {
            csel[cur] = 0;
            cscr[cur] = 0;
        } else {
            if (csel[cur] >= nitems)
                csel[cur] = nitems - 1;
            if (csel[cur] < 0)
                csel[cur] = 0;
        }
        list_h = cy + ch - 3 - row;
        if (list_h < 1)
            list_h = 1;
        if (cscr[cur] > csel[cur])
            cscr[cur] = csel[cur];
        if (cscr[cur] < csel[cur] - list_h + 1)
            cscr[cur] = csel[cur] - list_h + 1;
        if (cscr[cur] < 0)
            cscr[cur] = 0;
        if (full || stepch || need_dirty || wiz_stained) {
            if (!full && !stepch) {
                lebui_goto(iy - 1, cx + 1);
                printf("%s%-*s%s", LEBUI_CLR_MENU, cw - 4, "", LEBUI_CLR_NORMAL);
            }
            wiz_draw_rows(row, ix, list_h, iw, draw_items, draw_checks, nitems,
                          csel[cur], cscr[cur], focus);
            need_dirty = 0;
            wiz_stained = 0;
        } else if (snap.focus != focus) {
            wiz_content_row(row + csel[cur] - cscr[cur], ix, iw, draw_items,
                            draw_checks, csel[cur], nitems, csel[cur], focus);
            wiz_content_row(row + snap.sel - snap.scr, ix, iw, draw_items,
                            draw_checks, snap.sel, nitems, csel[cur], focus);
            wiz_side_row(sy + 1 + snap.side, sx, sw, snap.side, uw_wiz_names, status,
                         cur, side_cur, focus, done, locked);
            wiz_side_row(sy + 1 + side_cur, sx, sw, side_cur, uw_wiz_names, status,
                         cur, side_cur, focus, done, locked);
            wiz_side_row(sy + 1 + cur, sx, sw, cur, uw_wiz_names, status,
                         cur, side_cur, focus, done, locked);
            printf("%s", LEBUI_CLR_NORMAL);
        } else if (focus == WIZ_FOCUS_CONTENT) {
            if (snap.scr != cscr[cur]) {
                wiz_draw_rows(row, ix, list_h, iw, draw_items, draw_checks,
                              nitems, csel[cur], cscr[cur], focus);
            } else {
                wiz_content_row(row + snap.sel - cscr[cur], ix, iw, draw_items,
                                draw_checks, snap.sel, nitems, csel[cur],
                                focus);
                wiz_content_row(row + csel[cur] - cscr[cur], ix, iw,
                                draw_items, draw_checks, csel[cur], nitems,
                                csel[cur], focus);
                printf("%s", LEBUI_CLR_NORMAL);
            }
        } else {
            wiz_side_row(sy + 1 + snap.side, sx, sw, snap.side, uw_wiz_names, status,
                         cur, side_cur, focus, done, locked);
            wiz_side_row(sy + 1 + side_cur, sx, sw, side_cur, uw_wiz_names, status,
                         cur, side_cur, focus, done, locked);
            printf("%s", LEBUI_CLR_NORMAL);
        }
        if (full || stepch) {
            lebui_goto(cy + ch - 2, cx + 2);
            printf("%s b:Back n:Next  %d/%d  %d/%d%s", LEBUI_CLR_DIM_CLR,
                   cur + 1, UW_COUNT, nitems ? csel[cur] + 1 : 0, nitems,
                   LEBUI_CLR_NORMAL);
        }
        printf("%s", LEBUI_CLR_NORMAL);
        lebui_flush();
        if (need_free) {
            free(draw_labels);
            free(draw_items);
        }

        snap.cur = cur;
        snap.focus = focus;
        snap.side = side_cur;
        snap.sel = csel[cur];
        snap.scr = cscr[cur];

        key = lebui_read_key();

        if (key == LEBUI_KEY_TAB)
            return LEBUI_KEY_TAB;
        if (key == LEBUI_KEY_UP) {
            if (focus == WIZ_FOCUS_STEPS) {
                if (side_cur > 0)
                    side_cur--;
            } else if (csel[cur] > 0) {
                csel[cur]--;
            }
        } else if (key == LEBUI_KEY_DOWN) {
            if (focus == WIZ_FOCUS_STEPS) {
                if (side_cur < UW_COUNT - 1)
                    side_cur++;
            } else if (csel[cur] < nitems - 1) {
                csel[cur]++;
            }
        } else if (key == LEBUI_KEY_PGUP) {
            if (focus == WIZ_FOCUS_STEPS) {
                side_cur -= list_h;
                if (side_cur < 0)
                    side_cur = 0;
            } else {
                csel[cur] -= list_h;
                if (csel[cur] < 0)
                    csel[cur] = 0;
            }
        } else if (key == LEBUI_KEY_PGDN) {
            if (focus == WIZ_FOCUS_STEPS) {
                side_cur += list_h;
                if (side_cur > UW_COUNT - 1)
                    side_cur = UW_COUNT - 1;
            } else {
                csel[cur] += list_h;
                if (csel[cur] > nitems - 1)
                    csel[cur] = nitems - 1;
            }
        } else if (key == LEBUI_KEY_HOME) {
            if (focus == WIZ_FOCUS_STEPS)
                side_cur = 0;
            else
                csel[cur] = 0;
        } else if (key == LEBUI_KEY_END) {
            if (focus == WIZ_FOCUS_STEPS)
                side_cur = UW_COUNT - 1;
            else
                csel[cur] = nitems - 1;
        } else if (key == LEBUI_KEY_LEFT) {
            if (focus == WIZ_FOCUS_CONTENT) {
                focus = WIZ_FOCUS_STEPS;
                side_cur = cur;
            } else {
                int j = cur - 1;
                while (j > 0 && locked[j])
                    j--;
                cur = j;
                focus = WIZ_FOCUS_CONTENT;
                side_cur = cur;
            }
        } else if (key == LEBUI_KEY_RIGHT) {
            if (focus == WIZ_FOCUS_STEPS) {
                focus = WIZ_FOCUS_CONTENT;
            } else {
                int j = cur + 1;
                while (j < UW_COUNT && locked[j])
                    j++;
                if (j < UW_COUNT) {
                    if (cur == UW_WELCOME)
                        wel_done = 1;
                    cur = j;
                    focus = WIZ_FOCUS_CONTENT;
                    side_cur = cur;
                } else if (cur == UW_DO && part_ok) {
                    goto try_update;
                } else if (cur == UW_DISK) {
                    wiz_msgbox("Error", "Select a disk first.");
                } else if (cur == UW_PART) {
                    wiz_msgbox("Error", "Select a partition first.");
                }
            }
        } else if (key == 'b' || key == 'B' || key == LEBUI_KEY_BKSP) {
            if (cur > 0) {
                int j = cur - 1;
                while (j > 0 && locked[j])
                    j--;
                cur = j;
                focus = WIZ_FOCUS_CONTENT;
                side_cur = cur;
            }
        } else if (key == 'n' || key == 'N') {
            int j = cur + 1;
            while (j < UW_COUNT && locked[j])
                j++;
            if (j < UW_COUNT) {
                if (cur == UW_WELCOME)
                    wel_done = 1;
                cur = j;
                focus = WIZ_FOCUS_CONTENT;
                side_cur = cur;
            } else if (cur == UW_DO && part_ok) {
                goto try_update;
            } else if (cur == UW_DISK) {
                wiz_msgbox("Error", "Select a disk first.");
            } else if (cur == UW_PART) {
                wiz_msgbox("Error", "Select a partition first.");
            }
        } else if (key == ' ') {
            if (focus == WIZ_FOCUS_CONTENT && cur == UW_ITEMS) {
                int s = csel[cur];
                if (s >= 0 && s < UPD_COUNT) {
                    upd_selected[s] = !upd_selected[s];
                    need_dirty = 1;
                }
            }
        } else if ((key == 'r' || key == 'R') && cur == UW_DISK) {
            focus = WIZ_FOCUS_CONTENT;
            csel[cur] = nitems - 1;
        } else if (key == LEBUI_KEY_ENTER) {
            if (focus == WIZ_FOCUS_STEPS) {
                if (locked[side_cur]) {
                    wiz_msgbox("Locked",
                                      "Finish the earlier steps first.");
                } else {
                    cur = side_cur;
                    focus = WIZ_FOCUS_CONTENT;
                }
                continue;
            }
            switch (cur) {
            case UW_WELCOME:
                wel_done = 1;
                cur = UW_DISK;
                break;
            case UW_DISK:
                if (csel[cur] < disk_count) {
                    if (csel[cur] != disk_idx) {
                        disk_idx = csel[cur];
                        part_idx = -1;
                        csel[UW_PART] = 0;
                        cscr[UW_PART] = 0;
                    }
                    cur = UW_PART;
                } else {
                    char rmsg[64];
                    int n = wiz_rescan(&disk_idx, &part_idx);
                    if (disk_idx < 0)
                        cur = UW_DISK;
                    else if (part_idx < 0 && cur > UW_PART)
                        cur = UW_PART;
                    snprintf(rmsg, sizeof(rmsg), "Found %d disk(s).", n);
                    wiz_msgbox("Rescan", rmsg);
                }
                break;
            case UW_PART:
                if (!disk_ok || disks[disk_idx].part_count == 0) {
                    wiz_msgbox("No Partitions",
                                      "No partitions here. Use ldiskutil.");
                } else if (disks[disk_idx].parts[csel[cur]].mbr_type == 0xEF) {
                    wiz_msgbox("Error",
                                      "The ESP holds the bootloader.\nUse a Lebirun partition or make one first.");
                } else {
                    part_idx = csel[cur];
                    cur = UW_ITEMS;
                }
                break;
            case UW_ITEMS:
                for (i = 0; i < UPD_COUNT; i++) {
                    if (upd_selected[i])
                        break;
                }
                if (i >= UPD_COUNT) {
                    wiz_msgbox("Error",
                                      "Select at least one update item.");
                } else {
                    cur = UW_SUMMARY;
                }
                break;
            case UW_SUMMARY:
                cur = UW_DO;
                break;
            case UW_DO:
            try_update: {
                int k;
                int cnt = 0;
                for (k = 0; k < UPD_COUNT; k++) {
                    if (upd_selected[k])
                        cnt++;
                }
                if (cnt == 0) {
                    wiz_msgbox("Error",
                                      "Select at least one update item.");
                    break;
                }
                if (wiz_confirm("Confirm",
                                       "Update selected items?")) {
                    need_full = 1;
                    if (step_do_update(disk_idx, part_idx) == 0)
                        return 0;
                }
                break;
            }
            }
            side_cur = cur;
        } else if (key == LEBUI_KEY_ESC || key == 'q' || key == 'Q') {
            if (cur == UW_WELCOME) {
                if (wiz_confirm("Quit", "Exit the installer?"))
                    return -1;
            } else {
                int j = cur - 1;
                while (j > 0 && locked[j])
                    j--;
                cur = j;
                focus = WIZ_FOCUS_CONTENT;
                side_cur = cur;
            }
        }
    }
}

int main(int argc, char **argv)
{
    int active_page;
    int scan_result;
    int ret;

    (void)argc;
    (void)argv;

    setvbuf(stdout, NULL, _IOFBF, 8192);

    if (getuid() != 0) {
        fprintf(stderr, "lebinstaller: must be run as root\n");
        return 1;
    }

    scan_result = inst_enumerate_disks();
    if (scan_result > 0)
        inst_scan_disks();

    lebui_get_size(&term_sz);
    if (lebui_init() != LEBUI_RESULT_OK) {
        fprintf(stderr, "lebinstaller: cannot initialize terminal\n");
        return 1;
    }
    if (atexit(lebui_shutdown) != 0) {
        cleanup_exit();
        fprintf(stderr, "lebinstaller: cannot register terminal cleanup\n");
        return 1;
    }

    if (scan_result <= 0) {
        lebui_msgbox_auto("Error", "No disks found.", term_sz.rows,
                          term_sz.cols);
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
