#include <lebirun/drivers/nvme/nvme.h>
#include <lebirun/blkdev.h>
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <lebirun/mutex.h>
#include <lebirun/timekeeping.h>
#include <lebirun/lke.h>
#include <lebirun/pci.h>
#include <string.h>
#include <stdio.h>

#define NVME_VIRT (KERNEL_VMA + 0x36F00000ULL)
#define NVME_VIRT_END (KERNEL_VMA + 0x37000000ULL)
#define NVME_QD 2
#define NVME_PRP_OFF 0x800

#define NVME_CAP  0x00
#define NVME_VS   0x08
#define NVME_CC   0x14
#define NVME_CSTS 0x1C
#define NVME_AQA  0x24
#define NVME_ASQ  0x28
#define NVME_ACQ  0x30

typedef struct {
    uint8_t *sq;
    volatile uint32_t *cq;
    volatile uint32_t *sq_db;
    volatile uint32_t *cq_db;
    uint16_t tail;
    uint16_t head;
    uint16_t cid;
    uint8_t phase;
} nvme_queue_t;

typedef struct {
    uint64_t regs;
    uint64_t mem;
    uint64_t *prp;
    uint64_t max_bytes;
    nvme_queue_t ioq;
    mutex_t lock;
} nvme_ctrl_t;

typedef struct {
    blkdev_t blk;
    nvme_ctrl_t *ctrl;
    uint32_t nsid;
} nvme_ns_t;

static uint64_t nvme_virt_next = NVME_VIRT;
static uint32_t nvme_count;

static uint32_t KERNEL_INIT nvme_pci(uint8_t bus, uint8_t slot, uint8_t func,
                                     uint8_t off, int wr, uint32_t val) {
    uint32_t addr = 0x80000000U | ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                    (off & 0xFC);

    __asm__ volatile ("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)0xCF8));
    if (wr) {
        __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"((uint16_t)0xCFC));
        return 0;
    }
    __asm__ volatile ("inl %1, %0" : "=a"(val) : "Nd"((uint16_t)0xCFC));
    return val;
}

static inline uint32_t nvme_rd(nvme_ctrl_t *c, uint32_t reg) {
    return *(volatile uint32_t *)(c->regs + reg);
}

static inline void nvme_wr(nvme_ctrl_t *c, uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(c->regs + reg) = val;
}

static inline void nvme_wr64(nvme_ctrl_t *c, uint32_t reg, uint64_t val) {
    *(volatile uint64_t *)(c->regs + reg) = val;
}

static int nvme_expired(uint64_t end, uint64_t spins, uint64_t ms) {
    extern int scheduler_initialized;
    uint64_t flags;

    if (end ? tsc_get_ns() >= end : spins >= ms * 20000) return 1;
    __asm__ volatile ("pushf; pop %0" : "=r"(flags) : : "memory");
    if (spins >= 16384 && !(spins & 4095) && scheduler_initialized &&
        current_task && (flags & 0x200))
        schedule();
    else
        __asm__ volatile ("pause");
    return 0;
}

static int nvme_cmd(nvme_queue_t *q, uint32_t *cmd, uint64_t ms) {
    volatile uint32_t *cqe;
    uint64_t end;
    uint64_t spins;
    uint16_t cid;
    uint16_t got;
    uint32_t status;

    cid = q->cid++;
    cmd[0] = (cmd[0] & 0xFFFF) | ((uint32_t)cid << 16);
    memcpy(q->sq + (uint64_t)q->tail * 64, cmd, 64);
    q->tail = (uint16_t)((q->tail + 1) % NVME_QD);
    __asm__ volatile ("sfence" ::: "memory");
    *q->sq_db = q->tail;
    end = tsc_available() ? tsc_get_ns() + ms * 1000000ULL : 0;
    for (spins = 0;; spins++) {
        cqe = q->cq + (uint64_t)q->head * 4;
        if (((cqe[3] >> 16) & 1) != q->phase) {
            if (nvme_expired(end, spins, ms)) return -1;
            continue;
        }
        got = (uint16_t)cqe[3];
        status = (cqe[3] >> 17) & 0x7FFF;
        if (++q->head == NVME_QD) {
            q->head = 0;
            q->phase ^= 1;
        }
        *q->cq_db = q->head;
        if (got == cid) return status ? -1 : 0;
    }
}

static int nvme_prp(nvme_ctrl_t *c, uint8_t *buf, uint64_t len,
                    uint32_t *cmd) {
    uint64_t phys;
    uint64_t first;
    uint64_t n;

    phys = blkdev_phys(buf);
    if (!phys) return -1;
    cmd[6] = (uint32_t)phys;
    cmd[7] = (uint32_t)(phys >> 32);
    first = PAGE_SIZE - ((uint64_t)(uintptr_t)buf & (PAGE_SIZE - 1));
    if (len <= first) return 0;
    buf += first;
    len -= first;
    if (len <= PAGE_SIZE) {
        phys = blkdev_phys(buf);
    } else {
        for (n = 0; len; n++, buf += PAGE_SIZE,
             len -= len > PAGE_SIZE ? PAGE_SIZE : len) {
            if (!(c->prp[n] = blkdev_phys(buf))) return -1;
        }
        phys = c->mem + NVME_PRP_OFF;
    }
    if (!phys) return -1;
    cmd[8] = (uint32_t)phys;
    cmd[9] = (uint32_t)(phys >> 32);
    return 0;
}

static int nvme_rw(blkdev_t *dev, uint64_t lba, uint64_t count, void *buf,
                   uint32_t opc) {
    nvme_ns_t *ns = (nvme_ns_t *)dev->priv;
    nvme_ctrl_t *c = ns->ctrl;
    uint32_t cmd[16];
    uint64_t max;
    uint64_t n;
    uint8_t *p = (uint8_t *)buf;
    int r = 0;

    max = c->max_bytes / dev->block_size;
    if (max > 65536) max = 65536;
    mutex_lock(&c->lock);
    while (count && !r) {
        n = count < max ? count : max;
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = opc;
        cmd[1] = ns->nsid;
        cmd[10] = (uint32_t)lba;
        cmd[11] = (uint32_t)(lba >> 32);
        cmd[12] = (uint32_t)(n - 1);
        r = nvme_prp(c, p, n * dev->block_size, cmd);
        if (!r) r = nvme_cmd(&c->ioq, cmd, 30000);
        lba += n;
        count -= n;
        p += n * dev->block_size;
    }
    mutex_unlock(&c->lock);
    return r;
}

static int nvme_read(blkdev_t *dev, uint64_t lba, uint64_t count, void *buf) {
    return nvme_rw(dev, lba, count, buf, 0x02);
}

static int nvme_write(blkdev_t *dev, uint64_t lba, uint64_t count,
                      const void *buf) {
    return nvme_rw(dev, lba, count, (void *)buf, 0x01);
}

static int nvme_flush(blkdev_t *dev) {
    nvme_ns_t *ns = (nvme_ns_t *)dev->priv;
    uint32_t cmd[16];
    int r;

    memset(cmd, 0, sizeof(cmd));
    cmd[1] = ns->nsid;
    mutex_lock(&ns->ctrl->lock);
    r = nvme_cmd(&ns->ctrl->ioq, cmd, 30000);
    mutex_unlock(&ns->ctrl->lock);
    return r;
}

static const blkdev_ops_t nvme_ops = {
    nvme_read, nvme_write, NULL, nvme_flush
};

static int KERNEL_INIT nvme_admin(nvme_queue_t *aq, uint32_t opc,
                                  uint32_t nsid, uint64_t phys,
                                  uint32_t cdw10, uint32_t cdw11) {
    uint32_t cmd[16];

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = opc;
    cmd[1] = nsid;
    cmd[6] = (uint32_t)phys;
    cmd[7] = (uint32_t)(phys >> 32);
    cmd[10] = cdw10;
    cmd[11] = cdw11;
    return nvme_cmd(aq, cmd, 5000);
}

static int KERNEL_INIT nvme_wait_ready(nvme_ctrl_t *c, uint32_t want,
                                       uint64_t ms) {
    uint64_t end = tsc_available() ? tsc_get_ns() + ms * 1000000ULL : 0;
    uint64_t spins;

    for (spins = 0; (nvme_rd(c, NVME_CSTS) & 1) != want; spins++) {
        if (nvme_rd(c, NVME_CSTS) & 2) return -1;
        if (nvme_expired(end, spins, ms)) return -1;
    }
    return 0;
}

static void KERNEL_INIT nvme_queue_init(nvme_queue_t *q, uint64_t sq,
                                        uint64_t cq, uint64_t db) {
    q->sq = (uint8_t *)sq;
    q->cq = (volatile uint32_t *)cq;
    q->sq_db = (volatile uint32_t *)db;
    q->phase = 1;
}

static int KERNEL_INIT nvme_add_ns(nvme_ctrl_t *c, uint32_t nsid,
                                   const uint8_t *id) {
    nvme_ns_t *ns;
    uint64_t nsze;
    uint32_t lbaf;
    uint32_t shift;

    memcpy(&nsze, id, sizeof(nsze));
    memcpy(&lbaf, id + 128 + 4 * (id[26] & 0xF), sizeof(lbaf));
    shift = (lbaf >> 16) & 0xFF;
    if (!nsze || (lbaf & 0xFFFF) || shift < 9 || shift > 31 ||
        (1ULL << shift) > c->max_bytes)
        return -1;
    ns = (nvme_ns_t *)kcalloc(1, sizeof(*ns));
    if (!ns) return -1;
    ns->ctrl = c;
    ns->nsid = nsid;
    ns->blk.ops = &nvme_ops;
    ns->blk.priv = ns;
    ns->blk.sectors = nsze << (shift - 9);
    ns->blk.block_size = 1U << shift;
    ns->blk.type = BLKDEV_DISK;
    snprintf(ns->blk.name, sizeof(ns->blk.name), "nvme%un%u", nvme_count,
             nsid);
    if (blkdev_register(&ns->blk) < 0) {
        kfree(ns);
        return -1;
    }
    KERNEL_INIT_LOG("NVMe: %s: %llu MB, %u-byte blocks\n", ns->blk.name,
                    (unsigned long long)(ns->blk.sectors >> 11),
                    ns->blk.block_size);
    return 0;
}

static void KERNEL_INIT nvme_probe(uint8_t bus, uint8_t slot, uint8_t func) {
    nvme_queue_t aq;
    nvme_ctrl_t *c;
    uint64_t bar;
    uint64_t cap;
    uint64_t stride;
    uint64_t pages;
    uint64_t timeout;
    uint64_t tmp;
    uint64_t i;
    uint32_t *list;
    uint32_t nn;
    uint32_t nsid;
    uint8_t *id;
    uint8_t mdts;
    int found;

    bar = nvme_pci(bus, slot, func, 0x10, 0, 0);
    if (bar & 1) return;
    if (((bar >> 1) & 3) == 2)
        bar |= (uint64_t)nvme_pci(bus, slot, func, 0x14, 0, 0) << 32;
    bar &= ~0xFFFULL;
    if (!bar || nvme_virt_next + PAGE_SIZE > NVME_VIRT_END) return;
    nvme_pci(bus, slot, func, 0x04, 1,
             nvme_pci(bus, slot, func, 0x04, 0, 0) | 0x406);
    c = (nvme_ctrl_t *)kcalloc(1, sizeof(*c));
    if (!c) return;
    tmp = 0;
    c->regs = nvme_virt_next;
    vmm_map_page(c->regs, bar, VMM_PTE_PRESENT | VMM_PTE_WRITE | VMM_PTE_PCD);
    pages = 1;
    cap = (uint64_t)nvme_rd(c, NVME_CAP) |
          ((uint64_t)nvme_rd(c, NVME_CAP + 4) << 32);
    stride = 4ULL << ((cap >> 32) & 0xF);
    timeout = (((cap >> 24) & 0xFF) + 1) * 500;
    if (((cap >> 48) & 0xF) || !((cap >> 37) & 1)) goto unmap;
    for (; pages * PAGE_SIZE < 0x1000 + 4 * stride; pages++) {
        if (c->regs + (pages + 1) * PAGE_SIZE > NVME_VIRT_END) goto unmap;
        vmm_map_page(c->regs + pages * PAGE_SIZE, bar + pages * PAGE_SIZE,
                     VMM_PTE_PRESENT | VMM_PTE_WRITE | VMM_PTE_PCD);
    }
    nvme_wr(c, NVME_CC, nvme_rd(c, NVME_CC) & ~1U);
    if (nvme_wait_ready(c, 0, timeout) < 0) goto unmap;
    c->mem = pfa_alloc_contiguous(2);
    tmp = pfa_alloc_contiguous(4);
    if (!c->mem || !tmp) goto free;
    memset((void *)(c->mem + KERNEL_VMA), 0, 2 * PAGE_SIZE);
    memset((void *)(tmp + KERNEL_VMA), 0, 2 * PAGE_SIZE);
    c->prp = (uint64_t *)(c->mem + KERNEL_VMA + NVME_PRP_OFF);
    memset(&aq, 0, sizeof(aq));
    nvme_queue_init(&aq, tmp + KERNEL_VMA, tmp + KERNEL_VMA + PAGE_SIZE,
                    c->regs + 0x1000);
    nvme_queue_init(&c->ioq, c->mem + KERNEL_VMA,
                    c->mem + KERNEL_VMA + PAGE_SIZE,
                    c->regs + 0x1000 + 2 * stride);
    aq.cq_db = (volatile uint32_t *)(c->regs + 0x1000 + stride);
    c->ioq.cq_db = (volatile uint32_t *)(c->regs + 0x1000 + 3 * stride);
    mutex_init(&c->lock);
    nvme_wr(c, NVME_AQA, ((NVME_QD - 1) << 16) | (NVME_QD - 1));
    nvme_wr64(c, NVME_ASQ, tmp);
    nvme_wr64(c, NVME_ACQ, tmp + PAGE_SIZE);
    nvme_wr(c, NVME_CC, 1 | (6 << 16) | (4 << 20));
    if (nvme_wait_ready(c, 1, timeout) < 0) goto disable;
    id = (uint8_t *)(tmp + KERNEL_VMA + 2 * PAGE_SIZE);
    list = (uint32_t *)(id + PAGE_SIZE);
    if (nvme_admin(&aq, 0x06, 0, tmp + 2 * PAGE_SIZE, 1, 0) < 0) goto disable;
    mdts = id[77];
    memcpy(&nn, id + 516, sizeof(nn));
    c->max_bytes = mdts && mdts < 8 ? PAGE_SIZE << mdts : 256 * PAGE_SIZE;
    if (nvme_admin(&aq, 0x05, 0, c->mem + PAGE_SIZE,
                   ((NVME_QD - 1) << 16) | 1, 1) < 0 ||
        nvme_admin(&aq, 0x01, 0, c->mem,
                   ((NVME_QD - 1) << 16) | 1, (1 << 16) | 1) < 0)
        goto disable;
    found = 0;
    if (nvme_rd(c, NVME_VS) >= 0x10100 &&
        nvme_admin(&aq, 0x06, 0, tmp + 3 * PAGE_SIZE, 2, 0) == 0) {
        for (i = 0; i < PAGE_SIZE / 4 && list[i]; i++) {
            if (nvme_admin(&aq, 0x06, list[i], tmp + 2 * PAGE_SIZE, 0, 0) == 0 &&
                nvme_add_ns(c, list[i], id) == 0)
                found++;
        }
    } else {
        for (nsid = 1; nsid && nsid <= nn; nsid++) {
            if (nvme_admin(&aq, 0x06, nsid, tmp + 2 * PAGE_SIZE, 0, 0) == 0 &&
                nvme_add_ns(c, nsid, id) == 0)
                found++;
        }
    }
    if (found) {
        pfa_free_contiguous(tmp, 4);
        KERNEL_INIT_LOG("NVMe: Controller %u at PCI %u:%u.%u, %d namespace(s)\n",
                        nvme_count, bus, slot, func, found);
        nvme_count++;
        nvme_virt_next += pages * PAGE_SIZE;
        return;
    }
disable:
    nvme_wr(c, NVME_CC, nvme_rd(c, NVME_CC) & ~1U);
    nvme_wait_ready(c, 0, timeout);
free:
    if (c->mem) pfa_free_contiguous(c->mem, 2);
    if (tmp) pfa_free_contiguous(tmp, 4);
unmap:
    for (i = 0; i < pages; i++)
        vmm_unmap_page(c->regs + i * PAGE_SIZE);
    kfree(c);
}

int KERNEL_INIT nvme_init(void) {
    static int probed KERNEL_INIT_BSS;
    uint32_t i;
    uint32_t bdf;

    if (probed) return nvme_count ? 0 : -1;
    probed = 1;
    for (i = 0; pci_get(i, &bdf) == 0; i++) {
        if (nvme_pci(PCI_BUS(bdf), PCI_SLOT(bdf), PCI_FUNC(bdf), 0x08, 0, 0) >> 8 ==
            0x010802)
            nvme_probe(PCI_BUS(bdf), PCI_SLOT(bdf), PCI_FUNC(bdf));
    }
    return nvme_count ? 0 : -1;
}

#ifndef __is_kernel
module_init(nvme_init);
LKE_NAME("nvme");
LKE_DESC("NVMe storage driver");
LKE_LICENSE("GPLv2");
LKE_VERSION_STR("1.0.0");
#endif
