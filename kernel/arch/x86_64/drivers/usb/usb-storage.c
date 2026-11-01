#include <lebirun/drivers/usb/xhci.h>
#include <lebirun/blkdev.h>
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <lebirun/mutex.h>
#include <lebirun/devfs.h>
#include <string.h>
#include <stdio.h>

#define MS_CHUNK 0x10000u
#define MS_TIMEOUT 20000
#define MS_STATUS 64
#define MS_SENSE 160
#define MS_DATA 256

enum { BOT_IN, BOT_OUT };
enum { UAS_CMD, UAS_STATUS, UAS_IN, UAS_OUT };

typedef struct ms_lun ms_lun_t;

typedef struct {
    usb_dev_t *usb;
    uint8_t *page;
    ms_lun_t *luns;
    mutex_t lock;
    uint32_t tag;
    uint8_t iface;
    uint8_t uas;
    uint8_t dead;
} ms_dev_t;

struct ms_lun {
    blkdev_t blk;
    ms_dev_t *ms;
    ms_lun_t *next;
    int index;
    uint8_t lun;
};

static void ms_put_be(uint8_t *p, uint64_t v, int bytes) {
    while (bytes--) {
        p[bytes] = (uint8_t)v;
        v >>= 8;
    }
}

static uint64_t ms_get_be(const uint8_t *p, int bytes) {
    uint64_t v = 0;

    while (bytes--) v = (v << 8) | *p++;
    return v;
}

static int ms_xfer(ms_dev_t *m, uint32_t pipe, void *buf, uint32_t len) {
    if (usb_bulk_submit(m->usb, pipe, buf, len) < 0) return -1;
    return usb_bulk_wait(m->usb, pipe, MS_TIMEOUT);
}

static int ms_bot(ms_dev_t *m, uint8_t lun, const uint8_t *cdb, uint8_t len,
                  void *buf, uint32_t n, int in) {
    uint8_t *cbw = m->page;
    uint8_t *csw = m->page + MS_STATUS;
    uint32_t sig = 0x43425355;
    uint32_t tag = ++m->tag;
    int r;
    int i;

    memset(cbw, 0, 31);
    memcpy(cbw, &sig, 4);
    memcpy(cbw + 4, &tag, 4);
    memcpy(cbw + 8, &n, 4);
    cbw[12] = in ? 0x80 : 0;
    cbw[13] = lun;
    cbw[14] = len;
    memcpy(cbw + 15, cdb, len);
    if (ms_xfer(m, BOT_OUT, cbw, 31) != 0) goto reset;
    if (n && ms_xfer(m, in ? BOT_IN : BOT_OUT, buf, n) < 0) goto reset;
    for (i = 0; (r = ms_xfer(m, BOT_IN, csw, 13)) == 1 && i < 1; i++);
    sig = 0x53425355;
    if (r || memcmp(csw, &sig, 4) || memcmp(csw + 4, &tag, 4) || csw[12] > 1)
        goto reset;
    return csw[12];
reset:
    usb_ctrl(m->usb, 0x21, 0xFF, 0, m->iface);
    usb_clear_halt(m->usb, BOT_IN);
    usb_clear_halt(m->usb, BOT_OUT);
    return -1;
}

static int ms_uas(ms_dev_t *m, uint8_t lun, const uint8_t *cdb, uint8_t len,
                  void *buf, uint32_t n, int in) {
    uint8_t *iu = m->page;
    uint8_t *st = m->page + MS_STATUS;
    uint32_t data = in ? UAS_IN : UAS_OUT;
    uint32_t i;
    int r;

    memset(iu, 0, 32);
    iu[0] = 1;
    iu[3] = 1;
    iu[9] = lun;
    memcpy(iu + 16, cdb, len);
    memset(st, 0, MS_SENSE - MS_STATUS);
    if (m->uas == 2) {
        r = usb_bulk_submit(m->usb, UAS_STATUS, st, MS_SENSE - MS_STATUS);
        if (!r && n) r = usb_bulk_submit(m->usb, data, buf, n);
        if (!r) r = ms_xfer(m, UAS_CMD, iu, 32);
        if (!r && n) r = usb_bulk_wait(m->usb, data, MS_TIMEOUT);
        if (!r) r = usb_bulk_wait(m->usb, UAS_STATUS, MS_TIMEOUT);
    } else {
        r = ms_xfer(m, UAS_CMD, iu, 32);
        if (!r) r = ms_xfer(m, UAS_STATUS, st, MS_SENSE - MS_STATUS);
        if (!r && n && (st[0] == 6 || st[0] == 7)) {
            r = ms_xfer(m, data, buf, n);
            if (!r) r = ms_xfer(m, UAS_STATUS, st, MS_SENSE - MS_STATUS);
        }
    }
    if (!r && st[0] == 3 && st[3] == 1) return st[6] ? 1 : 0;
    for (i = 0; i < 4; i++)
        usb_clear_halt(m->usb, i);
    return -1;
}

static int ms_cmd(ms_dev_t *m, uint8_t lun, const uint8_t *cdb, uint8_t len,
                  void *buf, uint32_t n, int in) {
    static const uint8_t request_sense[6] = { 0x03, 0, 0, 0, 18, 0 };
    uint8_t *sense = m->page + MS_SENSE;
    int r;

    r = m->uas ? ms_uas(m, lun, cdb, len, buf, n, in) :
                 ms_bot(m, lun, cdb, len, buf, n, in);
    if (r != 1) return r;
    if (m->uas)
        memcpy(sense, m->page + MS_STATUS + 16, 18);
    else if (ms_bot(m, lun, request_sense, 6, sense, 18, 1) != 0)
        memset(sense, 0, 18);
    return 1;
}

static int ms_rw(blkdev_t *dev, uint64_t lba, uint64_t count, void *buf,
                 int wr) {
    ms_lun_t *l = (ms_lun_t *)dev->priv;
    ms_dev_t *m = l->ms;
    uint8_t cdb[16];
    uint8_t *p = (uint8_t *)buf;
    uint64_t max = MS_CHUNK / dev->block_size;
    uint64_t n;
    uint8_t len;
    int r = 0;
    int i;

    mutex_lock(&m->lock);
    if (m->dead) r = -1;
    while (count && !r) {
        n = count < max ? count : max;
        memset(cdb, 0, sizeof(cdb));
        if (lba > 0x100000000ULL - n) {
            cdb[0] = wr ? 0x8A : 0x88;
            ms_put_be(cdb + 2, lba, 8);
            ms_put_be(cdb + 10, n, 4);
            len = 16;
        } else {
            cdb[0] = wr ? 0x2A : 0x28;
            ms_put_be(cdb + 2, lba, 4);
            ms_put_be(cdb + 7, n, 2);
            len = 10;
        }
        for (i = 0; i < 3; i++) {
            r = ms_cmd(m, l->lun, cdb, len, p, (uint32_t)(n * dev->block_size),
                       !wr);
            if (!r) break;
        }
        lba += n;
        count -= n;
        p += n * dev->block_size;
    }
    mutex_unlock(&m->lock);
    return r ? -1 : 0;
}

static int ms_read(blkdev_t *dev, uint64_t lba, uint64_t count, void *buf) {
    return ms_rw(dev, lba, count, buf, 0);
}

static int ms_write(blkdev_t *dev, uint64_t lba, uint64_t count,
                    const void *buf) {
    return ms_rw(dev, lba, count, (void *)buf, 1);
}

static int ms_flush(blkdev_t *dev) {
    ms_lun_t *l = (ms_lun_t *)dev->priv;
    uint8_t cdb[10] = { 0x35 };
    int r;

    mutex_lock(&l->ms->lock);
    r = l->ms->dead ? -1 : ms_cmd(l->ms, l->lun, cdb, sizeof(cdb), NULL, 0, 0);
    mutex_unlock(&l->ms->lock);
    return r < 0 ? -1 : 0;
}

static const blkdev_ops_t ms_disk_ops = { ms_read, ms_write, NULL, ms_flush };
static const blkdev_ops_t ms_cdrom_ops = { ms_read, NULL, NULL, NULL };

static int USB_HOTPLUG_TEXT ms_probe(ms_dev_t *m, uint8_t lun) {
    uint8_t cdb[16];
    uint8_t *d = m->page + MS_DATA;
    ms_lun_t *l;
    uint64_t last;
    uint32_t bs;
    uint8_t type;
    int r;
    int i;

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x12;
    cdb[4] = 36;
    if (ms_cmd(m, lun, cdb, 6, d, 36, 1) != 0) return -1;
    type = d[0] & 0x1F;
    if ((d[0] >> 5) || (type != 0 && type != 5 && type != 7 && type != 0xE))
        return -1;
    for (i = 0, r = 1; i < 25 && r > 0; i++) {
        memset(cdb, 0, sizeof(cdb));
        r = ms_cmd(m, lun, cdb, 6, NULL, 0, 0);
        if (r > 0 && m->page[MS_SENSE + 12] == 0x3A) return -1;
        if (r > 0) xhci_delay(200);
    }
    if (r) return -1;
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;
    if (ms_cmd(m, lun, cdb, 10, d, 8, 1) != 0) return -1;
    last = ms_get_be(d, 4);
    bs = (uint32_t)ms_get_be(d + 4, 4);
    if (last == 0xFFFFFFFFULL) {
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x9E;
        cdb[1] = 0x10;
        cdb[13] = 32;
        if (ms_cmd(m, lun, cdb, 16, d, 32, 1) != 0) return -1;
        last = ms_get_be(d, 8);
        bs = (uint32_t)ms_get_be(d + 8, 4);
    }
    if (bs < 512 || (bs & (bs - 1)) || bs > MS_CHUNK) return -1;
    l = (ms_lun_t *)kcalloc(1, sizeof(*l));
    if (!l) return -1;
    l->ms = m;
    l->lun = lun;
    l->blk.ops = type == 5 ? &ms_cdrom_ops : &ms_disk_ops;
    l->blk.priv = l;
    l->blk.sectors = (last + 1) * (bs / 512);
    l->blk.block_size = bs;
    l->blk.type = type == 5 ? BLKDEV_CDROM : BLKDEV_DISK;
    l->index = blkdev_register(&l->blk);
    if (l->index < 0) {
        kfree(l);
        return -1;
    }
    l->next = m->luns;
    m->luns = l;
    USB_LOG("USB: %s: %llu MB, %u-byte blocks\n", l->blk.name,
            (unsigned long long)(l->blk.sectors >> 11), bs);
    return 0;
}

void *USB_HOTPLUG_TEXT usb_storage_attach(usb_dev_t *usb, uint8_t iface,
                                          int uas, uint8_t maxlun) {
    ms_dev_t *m;
    uint64_t phys;
    uint32_t lun;

    m = (ms_dev_t *)kcalloc(1, sizeof(*m));
    if (!m) return NULL;
    phys = (uint64_t)pmm_alloc_mapped_page();
    if (!phys) {
        kfree(m);
        return NULL;
    }
    m->page = (uint8_t *)(phys + KERNEL_VMA);
    memset(m->page, 0, PAGE_SIZE);
    m->usb = usb;
    m->iface = iface;
    m->uas = (uint8_t)uas;
    mutex_init(&m->lock);
    for (lun = 0; lun <= maxlun; lun++)
        ms_probe(m, (uint8_t)lun);
    if (m->luns) return m;
    pfa_free(phys);
    kfree(m);
    return NULL;
}

void usb_storage_detach(void *storage) {
    ms_dev_t *m = (ms_dev_t *)storage;
    ms_lun_t *l;

    mutex_lock(&m->lock);
    m->dead = 1;
    for (l = m->luns; l; l = l->next) {
        blkdev_unregister((uint64_t)l->index);
        devfs_remove_blkdevs((uint64_t)l->index);
        printf("USB: %s removed\n", l->blk.name);
    }
    pfa_free((uint64_t)(uintptr_t)m->page - KERNEL_VMA);
    m->page = NULL;
    m->usb = NULL;
    mutex_unlock(&m->lock);
}
