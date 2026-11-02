#include <string.h>
#include <lebirun/fs/exfat.h>
#include <lebirun/mem_map.h>
#include <lebirun/vfs.h>
#include <lebirun/ramfs.h>
#include <lebirun/blkdev.h>
#include <lebirun/mutex.h>
#include <lebirun/fsname.h>

#define EX_EOC 0xFFFFFFFFu
#define EX_DIR 0x10
#define EX_ARCH 0x20

typedef struct {
    uint32_t first;
    uint8_t nofat;
    uint64_t size;
    uint64_t ci;
    uint32_t cc;
} ex_stream_t;

typedef struct {
    uint64_t dev;
    uint64_t part;
    uint32_t bps;
    uint32_t spc;
    uint64_t bpc;
    uint64_t fat;
    uint64_t heap;
    uint32_t count;
    uint32_t hint;
    uint32_t nfree;
    ex_stream_t root;
    ex_stream_t bmp;
    uint16_t *up;
    uint32_t nup;
    uint8_t *buf;
    uint64_t buf_lba;
    int dirty;
    int vdirty;
    mutex_t lock;
    uint16_t name[FSNAME_MAX];
    char u8[FSNAME_MAX * 3 + 1];
} ex_fs_t;

typedef struct {
    uint64_t idx;
    uint8_t fe[32];
    ex_stream_t st;
    uint64_t valid;
    uint16_t hash;
    uint8_t len;
} ex_ent_t;

typedef struct {
    vfs_node_t vfs;
    ex_fs_t *fs;
    ex_stream_t st;
    uint64_t valid;
    ex_stream_t pd;
    uint64_t idx;
    int root;
} ex_node_t;

static const vfs_node_ops_t ex_ops;
static vfs_fs_type_t ex_fs_type;

static uint64_t rd(const uint8_t *p, int n) {
    uint64_t v = 0;

    while (n--) v = v << 8 | p[n];
    return v;
}

static void wr(uint8_t *p, uint64_t v, int n) {
    while (n--) {
        *p++ = (uint8_t)v;
        v >>= 8;
    }
}

static uint16_t ex_sum(uint16_t s, uint8_t b) {
    return (uint16_t)((s >> 1 | s << 15) + b);
}

static int ex_ok(ex_fs_t *fs, uint64_t c) {
    return c >= 2 && c < (uint64_t)fs->count + 2;
}

static uint64_t ex_lba(ex_fs_t *fs, uint32_t c) {
    return fs->heap + (uint64_t)(c - 2) * fs->spc;
}

static int ex_flag(ex_fs_t *fs, int on) {
    blkdev_t *d = blkdev_get(fs->dev);
    uint8_t *b = (uint8_t *)kmalloc(BLKDEV_SECTOR_SIZE);
    int r = -1;

    if (b && !blkdev_read(d, fs->part, 1, b)) {
        b[106] = (uint8_t)(on ? b[106] | 2 : b[106] & ~2);
        if (!on && fs->nfree != UINT32_MAX)
            b[112] = (uint8_t)((uint64_t)(fs->count - fs->nfree) * 100 / fs->count);
        r = blkdev_write(d, fs->part, 1, b);
        blkdev_flush(d);
    }
    kfree(b);
    return r;
}

static int ex_dev(ex_fs_t *fs, uint64_t lba, uint64_t n, void *b, int w) {
    blkdev_t *d = blkdev_get(fs->dev);
    uint64_t k = fs->bps / BLKDEV_SECTOR_SIZE;

    if (w && !fs->vdirty) {
        if (ex_flag(fs, 1)) return -1;
        fs->vdirty = 1;
    }
    return w ? blkdev_write(d, fs->part + lba * k, n * k, b) :
               blkdev_read(d, fs->part + lba * k, n * k, b);
}

static int ex_sync(ex_fs_t *fs) {
    if (!fs->dirty) return 0;
    fs->dirty = 0;
    if (!ex_dev(fs, fs->buf_lba, 1, fs->buf, 1)) return 0;
    fs->buf_lba = ~0ULL;
    return -1;
}

static uint8_t *ex_get(ex_fs_t *fs, uint64_t lba, int read) {
    if (fs->buf_lba == lba) return fs->buf;
    if (ex_sync(fs)) return NULL;
    fs->buf_lba = ~0ULL;
    if (read && ex_dev(fs, lba, 1, fs->buf, 0)) return NULL;
    fs->buf_lba = lba;
    return fs->buf;
}

static int ex_io(ex_fs_t *fs, uint64_t lba, uint64_t n, uint8_t *b, int w) {
    if (fs->buf_lba - lba < n) {
        if (w) {
            fs->dirty = 0;
            fs->buf_lba = ~0ULL;
        } else if (ex_sync(fs)) {
            return -1;
        }
    }
    return ex_dev(fs, lba, n, b, w);
}

static int ex_done(ex_fs_t *fs, int r) {
    if (ex_sync(fs) && !r) r = -1;
    mutex_unlock(&fs->lock);
    return r;
}

static uint8_t *ex_fatp(ex_fs_t *fs, uint32_t c) {
    uint8_t *p = ex_get(fs, fs->fat + (uint64_t)c * 4 / fs->bps, 1);

    return p ? p + (uint64_t)c * 4 % fs->bps : NULL;
}

static uint32_t ex_fat(ex_fs_t *fs, uint32_t c) {
    uint8_t *p = ex_fatp(fs, c);

    return p ? (uint32_t)rd(p, 4) : 0;
}

static int ex_fat_set(ex_fs_t *fs, uint32_t c, uint32_t v) {
    uint8_t *p = ex_fatp(fs, c);

    if (!p) return -1;
    wr(p, v, 4);
    fs->dirty = 1;
    return 0;
}

static uint32_t ex_clus(ex_fs_t *fs, ex_stream_t *st, uint64_t idx) {
    uint32_t n;

    if (!ex_ok(fs, st->first)) return 0;
    if (st->nofat)
        return idx < (st->size + fs->bpc - 1) / fs->bpc &&
               ex_ok(fs, st->first + idx) ? (uint32_t)(st->first + idx) : 0;
    if (!st->cc || idx < st->ci) {
        st->ci = 0;
        st->cc = st->first;
    }
    while (st->ci < idx) {
        n = ex_fat(fs, st->cc);
        if (!ex_ok(fs, n)) return 0;
        st->cc = n;
        st->ci++;
    }
    return st->cc;
}

static int ex_bit(ex_fs_t *fs, uint32_t c, int set) {
    uint64_t bit = c - 2;
    uint64_t by = bit >> 3;
    uint32_t cl = ex_clus(fs, &fs->bmp, by / fs->bpc);
    uint8_t *p;
    int old;

    if (!cl || !(p = ex_get(fs, ex_lba(fs, cl) + by % fs->bpc / fs->bps, 1)))
        return -1;
    p += by % fs->bps;
    old = *p >> (bit & 7) & 1;
    if (set >= 0 && old != set) {
        *p ^= (uint8_t)(1 << (bit & 7));
        fs->dirty = 1;
        if (fs->nfree != UINT32_MAX) set ? fs->nfree-- : fs->nfree++;
    }
    return old;
}

static uint32_t ex_alloc(ex_fs_t *fs, uint32_t prev) {
    uint32_t c = fs->hint;
    uint32_t i;
    int b;

    for (i = 0; i < fs->count; i++, c++) {
        if (!ex_ok(fs, c)) c = 2;
        if ((b = ex_bit(fs, c, -1)) < 0) return 0;
        if (b) continue;
        if (ex_bit(fs, c, 1) < 0 || ex_fat_set(fs, c, EX_EOC) ||
            (prev && ex_fat_set(fs, prev, c))) {
            ex_bit(fs, c, 0);
            return 0;
        }
        fs->hint = c + 1;
        return c;
    }
    return 0;
}

static void ex_free(ex_fs_t *fs, ex_stream_t *st, uint64_t keep) {
    uint64_t n = (st->size + fs->bpc - 1) / fs->bpc;
    uint64_t i;
    uint32_t c = st->first;
    uint32_t next;
    int nofat = st->nofat;

    st->cc = 0;
    if (!keep) {
        st->first = 0;
        st->nofat = 0;
    } else {
        if (keep >= n || !(next = ex_clus(fs, st, keep - 1))) return;
        c = nofat ? next + 1 : ex_fat(fs, next);
        if (!nofat) ex_fat_set(fs, next, EX_EOC);
    }
    for (i = keep; i < n && ex_ok(fs, c); i++) {
        next = nofat ? c + 1 : ex_fat(fs, c);
        ex_bit(fs, c, 0);
        c = next;
    }
}

static int ex_grow(ex_fs_t *fs, ex_stream_t *st, uint64_t size) {
    uint64_t have = (st->size + fs->bpc - 1) / fs->bpc;
    uint64_t need = (size + fs->bpc - 1) / fs->bpc;
    uint64_t old = st->size;
    uint64_t i;
    uint32_t last = have ? ex_clus(fs, st, have - 1) : 0;
    uint32_t c;

    if (need <= have) return 0;
    if (have && !last) return -1;
    if (st->nofat) {
        for (i = 0; i < have; i++)
            if (ex_fat_set(fs, st->first + i,
                           i + 1 < have ? st->first + i + 1 : EX_EOC))
                return -1;
        st->nofat = 0;
    }
    for (i = have; i < need; i++) {
        if (!(c = ex_alloc(fs, last))) {
            st->size = i * fs->bpc;
            ex_free(fs, st, have);
            st->size = old;
            return -1;
        }
        if (!last) st->first = c;
        last = c;
    }
    return 0;
}

static int ex_zero(ex_fs_t *fs, uint32_t c) {
    uint8_t *p;
    uint32_t i;

    if (!ex_ok(fs, c)) return -1;
    for (i = 0; i < fs->spc; i++) {
        if (!(p = ex_get(fs, ex_lba(fs, c) + i, 0))) return -1;
        memset(p, 0, fs->bps);
        fs->dirty = 1;
    }
    return 0;
}

static uint16_t ex_up(ex_fs_t *fs, uint16_t c) {
    uint32_t lo = 0;
    uint32_t hi = fs->nup;
    uint32_t m;

    if (!fs->up) return c >= 'a' && c <= 'z' ? c - 32 : c;
    while (lo < hi) {
        m = (lo + hi) / 2;
        if (fs->up[m * 2] < c) lo = m + 1;
        else hi = m;
    }
    return lo < fs->nup && fs->up[lo * 2] == c ? fs->up[lo * 2 + 1] : c;
}

static uint16_t ex_hash(ex_fs_t *fs, const uint16_t *s, int n) {
    uint16_t h = 0;
    uint16_t c;

    while (n--) {
        c = ex_up(fs, *s++);
        h = ex_sum(ex_sum(h, (uint8_t)c), (uint8_t)(c >> 8));
    }
    return h;
}

static void ex_upcase(ex_fs_t *fs, ex_stream_t *st) {
    uint64_t pos;
    uint32_t idx = 0;
    uint32_t cap = 0;
    uint32_t c;
    uint16_t v;
    uint16_t *t;
    uint8_t *p;
    int skip = 0;

    for (pos = 0; pos + 1 < st->size && idx < 0x10000; pos += 2) {
        if (!(c = ex_clus(fs, st, pos / fs->bpc)) ||
            !(p = ex_get(fs, ex_lba(fs, c) + pos % fs->bpc / fs->bps, 1)))
            goto fail;
        v = (uint16_t)rd(p + pos % fs->bps, 2);
        if (skip) {
            idx += v;
            skip = 0;
        } else if (v == idx) {
            idx++;
        } else if (v == 0xFFFF) {
            skip = 1;
        } else {
            if (fs->nup == cap) {
                cap = cap ? cap * 2 : 256;
                if (!(t = (uint16_t *)krealloc(fs->up, cap * 4))) goto fail;
                fs->up = t;
            }
            fs->up[fs->nup * 2] = (uint16_t)idx++;
            fs->up[fs->nup++ * 2 + 1] = v;
        }
    }
    if (fs->nup && (t = (uint16_t *)krealloc(fs->up, fs->nup * 4)))
        fs->up = t;
    return;
fail:
    kfree(fs->up);
    fs->up = NULL;
    fs->nup = 0;
}

static uint32_t ex_ts(uint64_t t) {
    int64_t z = (int64_t)(t / 86400) + 719468;
    int64_t era = z / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    int64_t y = yoe + era * 400 + (m <= 2);
    uint32_t s = (uint32_t)(t % 86400);

    if (y < 1980) return 0x00210000;
    if (y > 2107) return 0xFF9F0000;
    return (uint32_t)(y - 1980) << 25 | (uint32_t)m << 21 | (uint32_t)d << 16 |
           (s / 3600) << 11 | (s / 60 % 60) << 5 | (s % 60 / 2);
}

static uint64_t ex_unix(uint32_t ts) {
    int64_t m = ts >> 21 & 15;
    int64_t y = (ts >> 25) + 1980 - (m <= 2);
    int64_t era = y / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + (ts >> 16 & 31) - 1;
    int64_t days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;

    if (!m || days < 0) return 0;
    return (uint64_t)days * 86400 + (ts >> 11 & 31) * 3600 +
           (ts >> 5 & 63) * 60 + (ts & 31) * 2;
}

static uint8_t *ex_ent(ex_fs_t *fs, ex_stream_t *d, uint64_t i) {
    uint64_t pos = i * 32;
    uint32_t c;
    uint8_t *p;

    if (pos >= d->size || !(c = ex_clus(fs, d, pos / fs->bpc))) return NULL;
    p = ex_get(fs, ex_lba(fs, c) + pos % fs->bpc / fs->bps, 1);
    return p ? p + pos % fs->bps : NULL;
}

static int ex_next(ex_fs_t *fs, ex_stream_t *d, uint64_t *idx, ex_ent_t *e) {
    uint8_t *p;
    uint64_t i;

    for (i = *idx;; i++) {
        if (!(p = ex_ent(fs, d, i))) return i * 32 >= d->size ? 1 : -1;
        if (!p[0]) return 1;
        if (p[0] != 0x85 || p[1] < 2) continue;
        e->idx = i;
        memcpy(e->fe, p, 32);
        if (!(p = ex_ent(fs, d, i + 1)) || p[0] != 0xC0 || !p[3]) continue;
        e->st.first = (uint32_t)rd(p + 20, 4);
        e->st.nofat = p[1] >> 1 & 1;
        e->st.size = rd(p + 24, 8);
        e->st.cc = 0;
        e->valid = rd(p + 8, 8);
        e->hash = (uint16_t)rd(p + 4, 2);
        e->len = p[3];
        *idx = i + 1 + e->fe[1];
        return 0;
    }
}

static int ex_name(ex_fs_t *fs, ex_stream_t *d, const ex_ent_t *e,
                   uint16_t *out, const uint16_t *want) {
    uint8_t *p;
    uint16_t c;
    int k;
    int j;
    int n = 0;

    for (k = 0; n < e->len; k++) {
        if (k + 2 > e->fe[1] || !(p = ex_ent(fs, d, e->idx + 2 + k)) ||
            p[0] != 0xC1)
            return -1;
        for (j = 0; j < 15 && n < e->len; j++, n++) {
            c = (uint16_t)rd(p + 2 + j * 2, 2);
            if (out) out[n] = c;
            else if (ex_up(fs, c) != ex_up(fs, want[n])) return 1;
        }
    }
    return 0;
}

static int ex_find(ex_fs_t *fs, ex_stream_t *d, const uint16_t *w, int wl,
                   ex_ent_t *e) {
    uint64_t i = 0;
    uint16_t h = ex_hash(fs, w, wl);
    int r;

    while (!(r = ex_next(fs, d, &i, e)))
        if (e->len == wl && e->hash == h && !ex_name(fs, d, e, NULL, w))
            return 0;
    return r;
}

static int ex_fix(ex_fs_t *fs, ex_stream_t *d, uint64_t at) {
    uint8_t *p;
    uint16_t s = 0;
    int n;
    int k;
    int j;

    if (!(p = ex_ent(fs, d, at))) return -1;
    n = p[1];
    for (k = 0; k <= n; k++) {
        if (!(p = ex_ent(fs, d, at + k))) return -1;
        for (j = 0; j < 32; j++)
            if (k || (j != 2 && j != 3)) s = ex_sum(s, p[j]);
    }
    if (!(p = ex_ent(fs, d, at))) return -1;
    wr(p + 2, s, 2);
    fs->dirty = 1;
    return 0;
}

static int ex_del(ex_fs_t *fs, ex_stream_t *d, const ex_ent_t *e) {
    uint8_t *p;
    int k;

    for (k = 0; k <= e->fe[1]; k++) {
        if (!(p = ex_ent(fs, d, e->idx + k))) return -1;
        p[0] &= 0x7F;
        fs->dirty = 1;
    }
    return 0;
}

static int ex_load(ex_node_t *n) {
    ex_fs_t *fs = n->fs;
    uint8_t *p;
    uint32_t first;
    uint64_t size;
    int nofat;

    if (n->root) {
        n->st = fs->root;
        return 0;
    }
    if (!(p = ex_ent(fs, &n->pd, n->idx)) || p[0] != 0x85 ||
        !(p = ex_ent(fs, &n->pd, n->idx + 1)) || p[0] != 0xC0)
        return -1;
    first = (uint32_t)rd(p + 20, 4);
    nofat = p[1] >> 1 & 1;
    size = rd(p + 24, 8);
    if (first != n->st.first || nofat != n->st.nofat || size < n->st.size)
        n->st.cc = 0;
    n->st.first = first;
    n->st.nofat = (uint8_t)nofat;
    n->st.size = size;
    n->valid = rd(p + 8, 8);
    if (VFS_GET_TYPE(n->vfs.flags) == VFS_FILE) n->vfs.length = size;
    return 0;
}

static int ex_store(ex_node_t *n) {
    ex_fs_t *fs = n->fs;
    uint8_t *p;

    if (n->root) {
        fs->root = n->st;
        return 0;
    }
    if (!(p = ex_ent(fs, &n->pd, n->idx + 1)) || p[0] != 0xC0) return -1;
    p[1] = (uint8_t)(1 | n->st.nofat << 1);
    wr(p + 8, n->valid, 8);
    wr(p + 20, n->st.first, 4);
    wr(p + 24, n->st.size, 8);
    fs->dirty = 1;
    if (!(p = ex_ent(fs, &n->pd, n->idx)) || p[0] != 0x85) return -1;
    wr(p + 12, ex_ts(n->vfs.mtime), 4);
    p[21] = 0;
    fs->dirty = 1;
    return ex_fix(fs, &n->pd, n->idx);
}

static int ex_add(ex_node_t *dn, const uint16_t *nm, int len,
                  const uint8_t *fe, const ex_stream_t *st, uint64_t valid) {
    ex_fs_t *fs = dn->fs;
    int cnt = 2 + (len + 14) / 15;
    int run = 0;
    int k;
    int j;
    uint64_t i;
    uint64_t old;
    uint8_t *p;

    for (i = 0; run < cnt; i++) {
        if (i * 32 >= dn->st.size) {
            old = dn->st.size;
            if (ex_grow(fs, &dn->st, old + fs->bpc)) return -1;
            dn->st.size += fs->bpc;
            if (ex_zero(fs, ex_clus(fs, &dn->st, old / fs->bpc))) {
                ex_free(fs, &dn->st, old / fs->bpc);
                dn->st.size = old;
                return -1;
            }
            dn->valid = dn->st.size;
            if (ex_store(dn)) return -1;
        }
        if (!(p = ex_ent(fs, &dn->st, i))) return -1;
        run = p[0] & 0x80 ? 0 : run + 1;
    }
    i -= cnt;
    for (k = 0; k < cnt; k++) {
        if (!(p = ex_ent(fs, &dn->st, i + k))) return -1;
        memset(p, 0, 32);
        if (!k) {
            memcpy(p, fe, 32);
            p[0] = 0x85;
            p[1] = (uint8_t)(cnt - 1);
        } else if (k == 1) {
            p[0] = 0xC0;
            p[1] = (uint8_t)(1 | st->nofat << 1);
            p[3] = (uint8_t)len;
            wr(p + 4, ex_hash(fs, nm, len), 2);
            wr(p + 8, valid, 8);
            wr(p + 20, st->first, 4);
            wr(p + 24, st->size, 8);
        } else {
            p[0] = 0xC1;
            for (j = 0; j < 15 && (k - 2) * 15 + j < len; j++)
                wr(p + 2 + j * 2, nm[(k - 2) * 15 + j], 2);
        }
        fs->dirty = 1;
    }
    return ex_fix(fs, &dn->st, i);
}

static uint64_t ex_rw(ex_node_t *n, uint64_t off, uint64_t len, uint8_t *b,
                      int w) {
    ex_fs_t *fs = n->fs;
    uint64_t done = 0;
    uint64_t pos;
    uint64_t so;
    uint64_t take;
    uint64_t lba;
    uint32_t c;
    uint8_t *p;

    while (done < len) {
        pos = off + done;
        if (!(c = ex_clus(fs, &n->st, pos / fs->bpc))) break;
        lba = ex_lba(fs, c) + pos % fs->bpc / fs->bps;
        so = pos % fs->bps;
        take = fs->bpc - pos % fs->bpc;
        if (take > len - done) take = len - done;
        if (so || take < fs->bps || !b) {
            if (take > fs->bps - so) take = fs->bps - so;
            if (!(p = ex_get(fs, lba, so || take < fs->bps))) break;
            if (!w) {
                memcpy(b + done, p + so, take);
            } else {
                if (b) memcpy(p + so, b + done, take);
                else memset(p + so, 0, take);
                fs->dirty = 1;
            }
        } else {
            take -= take % fs->bps;
            if (ex_io(fs, lba, take / fs->bps, b + done, w)) break;
        }
        done += take;
    }
    return done;
}

static uint64_t ex_vfs_read(vfs_node_t *v, uint64_t off, uint64_t len,
                            uint8_t *b) {
    ex_node_t *n = (ex_node_t *)v;
    uint64_t done = 0;
    uint64_t z;

    mutex_lock(&n->fs->lock);
    if (!ex_load(n) && off < v->length) {
        if (len > v->length - off) len = v->length - off;
        z = off < n->valid ? n->valid - off : 0;
        if (z > len) z = len;
        done = ex_rw(n, off, z, b, 0);
        if (done == z) {
            memset(b + z, 0, len - z);
            done = len;
        }
    }
    ex_done(n->fs, 0);
    return done;
}

static uint64_t ex_vfs_write(vfs_node_t *v, uint64_t off, uint64_t len,
                             uint8_t *b) {
    ex_node_t *n = (ex_node_t *)v;
    ex_fs_t *fs = n->fs;
    uint64_t done = 0;

    mutex_lock(&fs->lock);
    if (!len || VFS_GET_TYPE(v->flags) != VFS_FILE || ex_load(n))
        goto out;
    if (off + len > n->st.size) {
        if (ex_grow(fs, &n->st, off + len)) goto out;
        n->st.size = off + len;
    }
    if (off <= n->valid ||
        ex_rw(n, n->valid, off - n->valid, NULL, 1) == off - n->valid) {
        done = ex_rw(n, off, len, b, 1);
        if (off + done > n->valid) n->valid = off + done;
    }
    v->length = n->st.size;
    v->mtime = ramfs_get_time();
    ex_store(n);
out:
    ex_done(fs, 0);
    return done;
}

static int ex_vfs_truncate(vfs_node_t *v, uint64_t len) {
    ex_node_t *n = (ex_node_t *)v;
    ex_fs_t *fs = n->fs;
    int r = -1;

    mutex_lock(&fs->lock);
    if (VFS_GET_TYPE(v->flags) == VFS_FILE && !ex_load(n)) {
        if (len > n->st.size) {
            r = ex_grow(fs, &n->st, len);
        } else {
            ex_free(fs, &n->st, (len + fs->bpc - 1) / fs->bpc);
            if (n->valid > len) n->valid = len;
            r = 0;
        }
        if (!r) {
            n->st.size = len;
            v->length = len;
            v->mtime = ramfs_get_time();
            r = ex_store(n);
        }
    }
    return ex_done(fs, r);
}

static void ex_vfs_close(vfs_node_t *v) {
    if (v->ref_count) return;
    vfs_node_release_name(v);
    kfree(v);
}

static vfs_node_t *ex_vfs_finddir(vfs_node_t *v, const char *name);
static int ex_vfs_readdir_next(vfs_node_t *v, uint64_t *cookie,
                               dirent_t *res);

static dirent_t *ex_vfs_readdir(vfs_node_t *v, uint64_t index) {
    static dirent_t dent;
    uint64_t cookie = 0;
    uint64_t i;

    for (i = 0; i <= index; i++)
        if (ex_vfs_readdir_next(v, &cookie, &dent) != 0) return NULL;
    return &dent;
}

static vfs_node_t *ex_mknode(ex_fs_t *fs, const char *name,
                             const ex_ent_t *e, const ex_stream_t *pd) {
    ex_node_t *n = (ex_node_t *)kmalloc(sizeof(ex_node_t));
    int dir = !e || (rd(e->fe + 4, 2) & EX_DIR);

    if (!n) return NULL;
    memset(n, 0, sizeof(ex_node_t));
    n->fs = fs;
    if (e) {
        n->st = e->st;
        n->valid = e->valid;
        n->pd = *pd;
        n->idx = e->idx;
        n->vfs.inode = (uint64_t)pd->first << 32 | e->idx;
        n->vfs.mtime = ex_unix((uint32_t)rd(e->fe + 12, 4));
        n->vfs.atime = ex_unix((uint32_t)rd(e->fe + 16, 4));
        n->vfs.ctime = n->vfs.mtime;
        if (!dir) n->vfs.length = e->st.size;
    } else {
        n->root = 1;
        n->st = fs->root;
        n->vfs.inode = fs->root.first;
        n->vfs.mtime = n->vfs.atime = n->vfs.ctime = ramfs_get_time();
    }
    n->vfs.mask = 0755;
    n->vfs.flags = (dir ? VFS_DIRECTORY : VFS_FILE) | VFS_DYNAMIC | VFS_EMBEDDED;
    if (vfs_node_set_name(&n->vfs, name) != 0) {
        kfree(n);
        return NULL;
    }
    n->vfs.read = ex_vfs_read;
    n->vfs.write = ex_vfs_write;
    n->vfs.close = ex_vfs_close;
    n->vfs.readdir = ex_vfs_readdir;
    n->vfs.finddir = ex_vfs_finddir;
    n->vfs.ops = &ex_ops;
    return &n->vfs;
}

static vfs_node_t *ex_vfs_finddir(vfs_node_t *v, const char *name) {
    ex_node_t *n = (ex_node_t *)v;
    ex_fs_t *fs = n->fs;
    vfs_node_t *r = NULL;
    ex_ent_t e;
    int len;

    if (!name || VFS_GET_TYPE(v->flags) != VFS_DIRECTORY) return NULL;
    mutex_lock(&fs->lock);
    if (!ex_load(n) && (len = fsname_to_utf16(name, fs->name)) > 0 &&
        !ex_find(fs, &n->st, fs->name, len, &e))
        r = ex_mknode(fs, name, &e, &n->st);
    ex_done(fs, 0);
    return r;
}

static int ex_vfs_readdir_next(vfs_node_t *v, uint64_t *cookie,
                               dirent_t *res) {
    ex_node_t *n = (ex_node_t *)v;
    ex_fs_t *fs = n->fs;
    uint64_t idx;
    ex_ent_t e;
    int r;

    if (!cookie || !res || VFS_GET_TYPE(v->flags) != VFS_DIRECTORY)
        return VFS_READDIR_IO;
    idx = *cookie;
    mutex_lock(&fs->lock);
    if (ex_load(n)) {
        r = -1;
    } else {
        while (!(r = ex_next(fs, &n->st, &idx, &e)) &&
               ex_name(fs, &n->st, &e, fs->name, NULL));
    }
    if (!r) {
        fsname_to_utf8(fs->name, e.len, fs->u8);
        if (vfs_dirent_set_name(res, fs->u8) != 0) {
            r = VFS_READDIR_NOMEM;
        } else {
            res->type = rd(e.fe + 4, 2) & EX_DIR ? VFS_DIRECTORY : VFS_FILE;
            res->inode = (uint64_t)n->st.first << 32 | e.idx;
            *cookie = idx;
        }
    } else if (r > 0) {
        r = VFS_READDIR_END;
    } else {
        r = VFS_READDIR_IO;
    }
    ex_done(fs, 0);
    return r;
}

static int ex_mk(vfs_node_t *v, const char *name, int dir, int excl) {
    ex_node_t *n = (ex_node_t *)v;
    ex_fs_t *fs = n->fs;
    ex_stream_t st = {0};
    uint8_t fe[32] = {0};
    uint32_t t = ex_ts(ramfs_get_time());
    ex_ent_t e;
    int len;
    int r = -1;

    if (!name) return -1;
    mutex_lock(&fs->lock);
    if (ex_load(n) || (len = fsname_to_utf16(name, fs->name)) <= 0) goto out;
    if ((r = ex_find(fs, &n->st, fs->name, len, &e)) <= 0) {
        r = r ? -1 : dir || excl ? -17 : 0;
        goto out;
    }
    r = -1;
    if (dir) {
        if (!(st.first = ex_alloc(fs, 0))) goto out;
        st.size = fs->bpc;
        if (ex_zero(fs, st.first)) goto undo;
    }
    wr(fe + 4, dir ? EX_DIR : EX_ARCH, 2);
    wr(fe + 8, t, 4);
    wr(fe + 12, t, 4);
    wr(fe + 16, t, 4);
    if (!(r = ex_add(n, fs->name, len, fe, &st, st.size))) {
        v->mtime = ramfs_get_time();
        goto out;
    }
undo:
    if (st.first) ex_bit(fs, st.first, 0);
out:
    return ex_done(fs, r);
}

static int ex_vfs_create(vfs_node_t *v, const char *name, uint64_t flags) {
    return ex_mk(v, name, 0, (flags & VFS_CREATE_EXCL) != 0);
}

static int ex_vfs_mkdir(vfs_node_t *v, const char *name, uint64_t perms) {
    (void)perms;
    return ex_mk(v, name, 1, 1);
}

static int ex_vfs_unlink(vfs_node_t *v, const char *name) {
    ex_node_t *n = (ex_node_t *)v;
    ex_fs_t *fs = n->fs;
    ex_ent_t e;
    ex_ent_t c;
    uint64_t i = 0;
    int len;
    int r = -1;

    if (!name) return -1;
    mutex_lock(&fs->lock);
    if (ex_load(n) || (len = fsname_to_utf16(name, fs->name)) <= 0 ||
        ex_find(fs, &n->st, fs->name, len, &e))
        goto out;
    if ((rd(e.fe + 4, 2) & EX_DIR) && (r = ex_next(fs, &e.st, &i, &c)) <= 0) {
        r = r ? -1 : -39;
        goto out;
    }
    if (!(r = ex_del(fs, &n->st, &e))) {
        ex_free(fs, &e.st, 0);
        v->mtime = ramfs_get_time();
    }
out:
    return ex_done(fs, r);
}

static int ex_vfs_rename(vfs_node_t *ov, const char *on, vfs_node_t *nv,
                         const char *nn) {
    ex_node_t *o = (ex_node_t *)ov;
    ex_node_t *d = (ex_node_t *)nv;
    ex_fs_t *fs = o->fs;
    ex_ent_t e;
    ex_ent_t t;
    ex_ent_t c;
    uint64_t i = 0;
    uint64_t dir;
    int len;
    int r = -1;

    if (!on || !nn || nv->ops != &ex_ops || d->fs != fs) return -1;
    mutex_lock(&fs->lock);
    if (ex_load(o) || ex_load(d) || (len = fsname_to_utf16(on, fs->name)) <= 0 ||
        ex_find(fs, &o->st, fs->name, len, &e) ||
        (len = fsname_to_utf16(nn, fs->name)) <= 0 ||
        (r = ex_find(fs, &d->st, fs->name, len, &t)) < 0)
        goto out;
    if (!r && (d->st.first != o->st.first || t.idx != e.idx)) {
        dir = rd(t.fe + 4, 2) & EX_DIR;
        if (dir != (rd(e.fe + 4, 2) & EX_DIR)) {
            r = dir ? -21 : -20;
            goto out;
        }
        if (dir && (r = ex_next(fs, &t.st, &i, &c)) <= 0) {
            r = r ? -1 : -39;
            goto out;
        }
        if ((r = ex_del(fs, &d->st, &t))) goto out;
        ex_free(fs, &t.st, 0);
    }
    if (!(r = ex_add(d, fs->name, len, e.fe, &e.st, e.valid)))
        r = ex_del(fs, &o->st, &e);
out:
    return ex_done(fs, r);
}

static const vfs_node_ops_t ex_ops = {
    .create = ex_vfs_create,
    .unlink = ex_vfs_unlink,
    .mkdir = ex_vfs_mkdir,
    .truncate = ex_vfs_truncate,
    .rename = ex_vfs_rename,
    .readdir_next = ex_vfs_readdir_next
};

static int ex_init(ex_fs_t *fs) {
    ex_stream_t up = {0};
    uint8_t *b;
    uint8_t *p;
    uint64_t i;
    uint32_t c;
    int act;

    fs->bps = BLKDEV_SECTOR_SIZE;
    fs->buf_lba = ~0ULL;
    fs->hint = 2;
    fs->nfree = UINT32_MAX;
    if (!(b = fs->buf = (uint8_t *)kmalloc(fs->bps)) || ex_dev(fs, 0, 1, b, 0))
        return -1;
    if (memcmp(b + 3, "EXFAT   ", 8) || rd(b + 510, 2) != 0xAA55 ||
        b[108] < 9 || b[108] > 12 || b[109] > 25 - b[108] || !b[110] ||
        b[110] > 2)
        return -1;
    act = b[110] == 2 && (rd(b + 106, 2) & 1);
    fs->vdirty = b[106] & 2 ? 2 : 0;
    fs->bps = 1u << b[108];
    fs->spc = 1u << b[109];
    fs->bpc = (uint64_t)fs->bps * fs->spc;
    fs->fat = rd(b + 80, 4) + (act ? rd(b + 84, 4) : 0);
    fs->heap = rd(b + 88, 4);
    fs->count = (uint32_t)rd(b + 92, 4);
    fs->root.first = (uint32_t)rd(b + 96, 4);
    if (!rd(b + 80, 4) || !fs->heap || !fs->count || fs->count > 0xFFFFFFF5 ||
        rd(b + 84, 4) * fs->bps / 4 < (uint64_t)fs->count + 2 ||
        !ex_ok(fs, fs->root.first))
        return -1;
    if (!(b = (uint8_t *)krealloc(fs->buf, fs->bps))) return -1;
    fs->buf = b;
    for (i = 1, c = fs->root.first; ex_ok(fs, c = ex_fat(fs, c)); i++)
        if (i >= fs->count) return -1;
    fs->root.size = i * fs->bpc;
    for (i = 0; (p = ex_ent(fs, &fs->root, i)) && p[0]; i++) {
        if (p[0] == 0x81 && (p[1] & 1) == act) {
            fs->bmp.first = (uint32_t)rd(p + 20, 4);
            fs->bmp.size = rd(p + 24, 8);
        } else if (p[0] == 0x82) {
            up.first = (uint32_t)rd(p + 20, 4);
            up.size = rd(p + 24, 8);
        }
    }
    if (!ex_ok(fs, fs->bmp.first) || fs->bmp.size * 8 < fs->count) return -1;
    if (ex_ok(fs, up.first)) ex_upcase(fs, &up);
    mutex_init(&fs->lock);
    return 0;
}

static void ex_fini(ex_fs_t *fs) {
    kfree(fs->buf);
    kfree(fs->up);
    kfree(fs);
}

static vfs_node_t *ex_do_mount(const char *device, const char *mountpoint) {
    extern uint64_t devfs_get_partition_start(vfs_node_t *node);
    vfs_node_t *dn;
    vfs_node_t *root;
    ex_fs_t *fs;

    (void)mountpoint;
    if (!device || !device[0] || !(dn = vfs_namei(device))) return NULL;
    if (VFS_GET_TYPE(dn->flags) != VFS_BLOCKDEVICE ||
        !(fs = (ex_fs_t *)kmalloc(sizeof(ex_fs_t)))) {
        vfs_release(dn);
        return NULL;
    }
    memset(fs, 0, sizeof(ex_fs_t));
    fs->dev = dn->inode;
    fs->part = devfs_get_partition_start(dn);
    vfs_release(dn);
    if (ex_init(fs) || !(root = ex_mknode(fs, "", NULL, NULL))) {
        ex_fini(fs);
        return NULL;
    }
    __atomic_add_fetch(&root->ref_count, 1, __ATOMIC_ACQ_REL);
    return root;
}

static int ex_do_sync(vfs_node_t *node, int data_only) {
    blkdev_t *d;

    (void)data_only;
    if (!node || !(d = blkdev_get(((ex_node_t *)node)->fs->dev))) return -1;
    return d->ops->flush ? d->ops->flush(d) : 0;
}

static int ex_do_statfs(vfs_node_t *root, vfs_statfs_t *st) {
    ex_fs_t *fs = ((ex_node_t *)root)->fs;
    uint64_t by;
    uint32_t n = 0;
    uint32_t c;
    uint8_t *p;
    uint8_t b;

    mutex_lock(&fs->lock);
    for (by = 0; fs->nfree == UINT32_MAX && by * 8 < fs->count; by++) {
        if (!(c = ex_clus(fs, &fs->bmp, by / fs->bpc)) ||
            !(p = ex_get(fs, ex_lba(fs, c) + by % fs->bpc / fs->bps, 1)))
            break;
        b = p[by % fs->bps];
        if (by * 8 + 8 > fs->count) b |= (uint8_t)(0xFF << (fs->count - by * 8));
        for (; b != 0xFF; b |= (uint8_t)(b + 1)) n++;
        if (by * 8 + 8 >= fs->count) fs->nfree = n;
    }
    st->type = 0x2011BAB0;
    st->bsize = fs->bpc;
    st->blocks = fs->count;
    st->bfree = fs->nfree;
    return ex_done(fs, fs->nfree == UINT32_MAX ? -1 : 0);
}

static int ex_do_unmount(vfs_node_t *root) {
    ex_fs_t *fs;

    if (!root) return -1;
    fs = ((ex_node_t *)root)->fs;
    ex_do_sync(root, 0);
    if (fs->vdirty == 1) ex_flag(fs, 0);
    __atomic_sub_fetch(&root->ref_count, 1, __ATOMIC_ACQ_REL);
    vfs_release(root);
    ex_fini(fs);
    return 0;
}

void exfat_vfs_register(void) {
    ex_fs_type.name = "exfat";
    ex_fs_type.mount = ex_do_mount;
    ex_fs_type.unmount = ex_do_unmount;
    ex_fs_type.sync = ex_do_sync;
    ex_fs_type.statfs = ex_do_statfs;
    ex_fs_type.next = NULL;
    vfs_register_fs(&ex_fs_type);
}
