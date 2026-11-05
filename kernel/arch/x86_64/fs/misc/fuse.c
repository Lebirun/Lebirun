#include <string.h>
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <lebirun/task.h>
#include <lebirun/spinlock.h>
#include <lebirun/uaccess.h>
#include <lebirun/vfs.h>
#include <lebirun/fs/fuse.h>

#define F_LOOKUP 1
#define F_FORGET 2
#define F_GETATTR 3
#define F_SETATTR 4
#define F_READLINK 5
#define F_SYMLINK 6
#define F_MKNOD 8
#define F_MKDIR 9
#define F_UNLINK 10
#define F_RMDIR 11
#define F_RENAME 12
#define F_OPEN 14
#define F_READ 15
#define F_WRITE 16
#define F_STATFS 17
#define F_RELEASE 18
#define F_FSYNC 20
#define F_INIT 26
#define F_OPENDIR 27
#define F_READDIR 28
#define F_RELEASEDIR 29
#define F_CREATE 35

#define F_BOUNCE 65536
#define F_DIRBUF (24 + 256 + 8)
#define F_MIN(a, b) ((a) < (b) ? (a) : (b))
#define ARG(r) ((uint8_t *)(&(r)->in + 1))

typedef struct {
    uint32_t len;
    uint32_t op;
    uint64_t unique;
    uint64_t nodeid;
    uint32_t uid;
    uint32_t gid;
    uint32_t pid;
    uint16_t extlen;
    uint16_t pad;
} f_in_t;

typedef struct {
    uint32_t len;
    int32_t error;
    uint64_t unique;
} f_out_t;

typedef struct {
    uint64_t ino, size, blocks, atime, mtime, ctime;
    uint32_t atimens, mtimens, ctimens, mode, nlink, uid, gid, rdev;
    uint32_t blksize, flags;
} f_attr_t;

typedef struct {
    uint64_t nodeid, gen, ev, av;
    uint32_t evn, avn;
    f_attr_t attr;
} f_entry_t;

typedef struct {
    uint64_t av;
    uint32_t avn, dummy;
    f_attr_t attr;
} f_attr_out_t;

typedef struct {
    uint32_t valid, pad;
    uint64_t fh, size, lock, atime, mtime, ctime;
    uint32_t atimens, mtimens, ctimens, mode, unused4, uid, gid, unused5;
} f_setattr_t;

typedef struct {
    uint64_t fh, off;
    uint32_t size, rwflags;
    uint64_t lock;
    uint32_t flags, pad;
} f_rw_t;

typedef struct {
    uint64_t ino, off;
    uint32_t namelen, type;
    char name[];
} f_dirent_t;

typedef struct f_req {
    struct f_req *next;
    void *out;
    const uint8_t *data;
    uint32_t dlen;
    uint32_t cap;
    uint32_t got;
    int32_t err;
    uint8_t wait;
    uint8_t done;
    uint8_t st;
    f_in_t in;
} f_req_t;

typedef struct f_wr {
    struct f_wr *next;
    task_t *task;
    f_req_t *r;
    f_out_t h;
    uint32_t hn;
    uint32_t left;
    uint32_t off;
} f_wr_t;

typedef struct {
    vfs_node_t dev;
    spinlock_t lock;
    wait_queue_t rq;
    wait_queue_t wq;
    f_req_t *head;
    f_req_t *tail;
    f_req_t *sent;
    f_wr_t *wr;
    uint64_t unique;
    uint32_t refs;
    uint32_t max_write;
    uint8_t dead;
    uint8_t ready;
    uint8_t mounted;
    uint8_t rattr;
} f_conn_t;

typedef struct {
    vfs_node_t vfs;
    f_conn_t *c;
    uint64_t id;
    uint64_t fh;
    uint64_t dfh;
    int32_t of;
    uint8_t fo;
    uint8_t dop;
} f_node_t;

static const vfs_node_ops_t f_ops;

static const uint8_t f_types[16] = {
    [1] = VFS_PIPE, [2] = VFS_CHARDEVICE, [4] = VFS_DIRECTORY,
    [6] = VFS_BLOCKDEVICE, [8] = VFS_FILE, [10] = VFS_SYMLINK,
    [12] = VFS_SOCKET
};

static uint64_t f_lock(f_conn_t *c) {
    uint64_t fl;

    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) : : "memory");
    spin_lock(&c->lock);
    return fl;
}

static void f_unlock(f_conn_t *c, uint64_t fl) {
    spin_unlock(&c->lock);
    __asm__ volatile("push %0; popf" : : "r"(fl) : "memory", "cc");
}

static int f_wait(f_conn_t *c, wait_queue_t *q, uint64_t *fl) {
    if (task_has_pending_signals()) return -4;
    lock_scheduler();
    waitq_add(q, current_task);
    if (task_has_pending_signals()) wake_task(current_task);
    unlock_scheduler();
    f_unlock(c, *fl);
    schedule();
    *fl = f_lock(c);
    return 0;
}

static void f_put(f_conn_t *c) {
    if (__atomic_sub_fetch(&c->refs, 1, __ATOMIC_ACQ_REL)) return;
    vfs_node_release_name(&c->dev);
    kfree(c);
}

static void f_done(f_conn_t *c, f_req_t *r) {
    if (r->wait) {
        r->done = 1;
        waitq_wake_all(&c->wq);
        return;
    }
    if (r->in.op == F_INIT) {
        if (!r->err && r->got >= 24 && ((uint32_t *)r->out)[5])
            c->max_write = ((uint32_t *)r->out)[5];
        c->ready = 1;
        waitq_wake_all(&c->wq);
    }
    kfree(r);
}

static void f_abort(f_conn_t *c) {
    uint64_t fl = f_lock(c);
    f_req_t *l[2] = { c->head, c->sent };
    f_req_t *r;
    f_req_t *n;
    f_wr_t *w;
    int i;

    c->dead = 1;
    c->head = c->tail = c->sent = NULL;
    for (i = 0; i < 2; i++) {
        for (r = l[i]; r; r = n) {
            n = r->next;
            r->err = -107;
            r->got = 0;
            f_done(c, r);
        }
    }
    while ((w = c->wr)) {
        c->wr = w->next;
        if ((r = w->r)) {
            r->err = -107;
            r->got = 0;
            f_done(c, r);
        }
        kfree(w);
    }
    waitq_wake_all(&c->rq);
    waitq_wake_all(&c->wq);
    f_unlock(c, fl);
}

static f_req_t *f_new(uint32_t op, uint64_t id, uint32_t alen) {
    f_req_t *r = (f_req_t *)kcalloc(1, sizeof(f_req_t) + alen);

    if (!r) return NULL;
    r->in.len = (uint32_t)sizeof(f_in_t) + alen;
    r->in.op = op;
    r->in.nodeid = id;
    if (current_task) {
        r->in.uid = (uint32_t)current_task->fsuid;
        r->in.gid = (uint32_t)current_task->fsgid;
        r->in.pid = (uint32_t)current_task->pid;
    }
    return r;
}

static void f_queue(f_conn_t *c, f_req_t *r) {
    r->in.unique = ++c->unique;
    r->next = NULL;
    if (c->tail) c->tail->next = r;
    else c->head = r;
    c->tail = r;
    waitq_wake_all(&c->rq);
}

static void f_unqueue(f_conn_t *c, f_req_t *r) {
    f_req_t **pp = &c->head;
    f_req_t *prev = NULL;

    while (*pp != r) {
        prev = *pp;
        pp = &prev->next;
    }
    *pp = r->next;
    if (c->tail == r) c->tail = prev;
}

static void f_bg(f_conn_t *c, f_req_t *r) {
    uint64_t fl;

    if (!r) return;
    fl = f_lock(c);
    if (c->dead) kfree(r);
    else f_queue(c, r);
    f_unlock(c, fl);
}

static int f_call(f_conn_t *c, f_req_t *r, void *out, uint32_t cap) {
    uint64_t fl;
    int e = 0;

    if (!r) return -12;
    r->out = out;
    r->cap = cap;
    r->wait = 1;
    fl = f_lock(c);
    while (!c->ready && !c->dead && !(e = f_wait(c, &c->wq, &fl)));
    if (e || c->dead) {
        f_unlock(c, fl);
        kfree(r);
        return e ? e : -107;
    }
    f_queue(c, r);
    while (!r->done && !(e = f_wait(c, &c->wq, &fl)));
    while (!r->done && r->st == 1) {
        f_unlock(c, fl);
        schedule();
        fl = f_lock(c);
    }
    if (!r->done && !r->st) {
        f_unqueue(c, r);
        f_unlock(c, fl);
        kfree(r);
        return e;
    }
    if (!r->done) {
        r->wait = 0;
        r->out = NULL;
        f_unlock(c, fl);
        return e;
    }
    f_unlock(c, fl);
    e = r->err ? r->err : (int)r->got;
    kfree(r);
    return e;
}

static uint64_t f_dev_read(vfs_node_t *v, uint64_t off, uint64_t size,
                           uint8_t *ub) {
    f_conn_t *c = (f_conn_t *)v;
    f_req_t *r;
    uint64_t fl = f_lock(c);
    uint32_t n;
    uint32_t h;
    int e = 0;
    int dead;

    (void)off;
    while (!c->head && !c->dead && !(e = f_wait(c, &c->rq, &fl)));
    if (e || c->dead) {
        f_unlock(c, fl);
        return (uint64_t)(int64_t)(e ? e : -19);
    }
    r = c->head;
    if ((n = r->in.len) > size) {
        f_unlock(c, fl);
        return (uint64_t)-22;
    }
    if (!(c->head = r->next)) c->tail = NULL;
    r->st = 1;
    f_unlock(c, fl);
    h = n - r->dlen;
    e = copy_to_user(ub, &r->in, h) < 0 ||
        (r->dlen && copy_to_user(ub + h, r->data, r->dlen) < 0);
    fl = f_lock(c);
    r->st = 2;
    if (e && !c->dead) {
        r->st = 0;
        if (!(r->next = c->head)) c->tail = r;
        c->head = r;
    } else if (c->dead) {
        r->err = -107;
        f_done(c, r);
    } else if (r->in.op == F_FORGET) {
        kfree(r);
    } else {
        r->next = c->sent;
        c->sent = r;
    }
    dead = c->dead;
    f_unlock(c, fl);
    if (e) return (uint64_t)-14;
    return dead ? (uint64_t)-19 : n;
}

static uint64_t f_dev_write(vfs_node_t *v, uint64_t off, uint64_t len,
                            uint8_t *b) {
    f_conn_t *c = (f_conn_t *)v;
    f_wr_t *w;
    f_wr_t **wp;
    f_req_t *r;
    f_req_t **rp;
    uint64_t fl;
    uint64_t i = 0;
    uint64_t k;
    int bad = 0;
    int dead;

    (void)off;
    fl = f_lock(c);
    for (w = c->wr; w && w->task != current_task; w = w->next);
    if (!w && !c->dead) {
        f_unlock(c, fl);
        if (!(w = (f_wr_t *)kcalloc(1, sizeof(f_wr_t)))) return (uint64_t)-12;
        w->task = current_task;
        fl = f_lock(c);
        w->next = c->wr;
        c->wr = w;
    }
    while (w && i < len && !c->dead) {
        if (w->hn < sizeof(f_out_t)) {
            k = F_MIN(sizeof(f_out_t) - w->hn, len - i);
            memcpy((uint8_t *)&w->h + w->hn, b + i, k);
            w->hn += (uint32_t)k;
            i += k;
            if (w->hn < sizeof(f_out_t)) break;
            if (w->h.len < sizeof(f_out_t)) {
                bad = 1;
                break;
            }
            w->left = w->h.len - (uint32_t)sizeof(f_out_t);
            w->off = 0;
            for (rp = &c->sent; *rp && (*rp)->in.unique != w->h.unique;
                 rp = &(*rp)->next);
            if ((w->r = r = *rp)) {
                *rp = r->next;
                r->err = w->h.error > 0 || w->h.error < -4095 ? -5 : w->h.error;
            }
        } else {
            k = F_MIN(w->left, len - i);
            if ((r = w->r) && r->out && w->off < r->cap)
                memcpy((uint8_t *)r->out + w->off, b + i,
                       F_MIN(k, r->cap - w->off));
            w->off += (uint32_t)k;
            w->left -= (uint32_t)k;
            i += k;
        }
        if (w->hn == sizeof(f_out_t) && !w->left) {
            if ((r = w->r)) {
                r->got = F_MIN(w->off, r->cap);
                f_done(c, r);
            }
            w->hn = 0;
            w->r = NULL;
        }
    }
    if (w && (bad || !w->hn || c->dead)) {
        for (wp = &c->wr; *wp && *wp != w; wp = &(*wp)->next);
        if (*wp) *wp = w->next;
        kfree(w);
    }
    dead = c->dead;
    f_unlock(c, fl);
    if (bad) return (uint64_t)-22;
    return dead ? (uint64_t)-19 : len;
}

static void f_dev_close(vfs_node_t *v) {
    if (v->ref_count) return;
    f_abort((f_conn_t *)v);
    f_put((f_conn_t *)v);
}

vfs_node_t *fuse_dev_node(void) {
    f_conn_t *c = (f_conn_t *)kcalloc(1, sizeof(f_conn_t));

    if (!c) return NULL;
    if (vfs_node_set_name(&c->dev, "fuse")) {
        kfree(c);
        return NULL;
    }
    c->dev.flags = VFS_CHARDEVICE | VFS_DYNAMIC | VFS_EMBEDDED | VFS_USER_READ;
    c->dev.mask = 0666;
    c->dev.inode = 229;
    c->dev.read = f_dev_read;
    c->dev.write = f_dev_write;
    c->dev.close = f_dev_close;
    c->refs = 1;
    c->max_write = 4096;
    return &c->dev;
}

static void f_forget(f_conn_t *c, uint64_t id) {
    f_req_t *q = f_new(F_FORGET, id, 8);

    if (q) *(uint64_t *)ARG(q) = 1;
    f_bg(c, q);
}

static void f_rel(f_conn_t *c, uint64_t id, uint32_t op, uint64_t fh,
                  uint32_t flags) {
    f_req_t *q = f_new(op, id, 24);

    if (q) {
        *(uint64_t *)ARG(q) = fh;
        *(uint32_t *)(ARG(q) + 8) = flags;
    }
    f_bg(c, q);
}

static int f_get(f_node_t *n, uint32_t op, uint32_t flags, uint64_t *fh) {
    struct { uint64_t fh; uint32_t flags, pad; } o;
    f_req_t *q = f_new(op, n->id, 8);
    int r;

    memset(&o, 0, sizeof(o));
    if (q) *(uint32_t *)ARG(q) = flags;
    r = f_call(n->c, q, &o, sizeof(o));
    *fh = o.fh;
    return r == -38 || r >= (int)sizeof(o) ? 0 : -1;
}

static int f_ncall(f_node_t *n, uint32_t op, const void *pre, uint32_t pl,
                   const char *s1, const char *s2, void *out, uint32_t cap) {
    uint32_t l1 = (uint32_t)strlen(s1) + 1;
    uint32_t l2 = s2 ? (uint32_t)strlen(s2) + 1 : 0;
    f_req_t *q = f_new(op, n->id, pl + l1 + l2);

    if (!q) return -12;
    if (pl) memcpy(ARG(q), pre, pl);
    memcpy(ARG(q) + pl, s1, l1);
    if (l2) memcpy(ARG(q) + pl + l1, s2, l2);
    if (out) memset(out, 0, cap);
    return f_call(n->c, q, out, cap);
}

static int f_made(f_conn_t *c, int r, const f_entry_t *e) {
    if (r >= 0 && e->nodeid) f_forget(c, e->nodeid);
    return r < 0 ? r : 0;
}

static void f_fill(f_node_t *n, const f_attr_t *a) {
    uint32_t t = f_types[a->mode >> 12 & 15];

    if (!t) t = VFS_FILE;
    n->vfs.flags = (n->vfs.flags & ~VFS_TYPE_MASK) | t;
    n->vfs.inode = a->ino;
    n->vfs.length = !a->size && t == VFS_SYMLINK ? 4095 : a->size;
    n->vfs.atime = a->atime;
    n->vfs.mtime = a->mtime;
    n->vfs.ctime = a->ctime;
    n->vfs.mask = a->mode & 07777;
    n->vfs.uid = a->uid;
    n->vfs.gid = a->gid;
}

static void f_getattr(f_node_t *n) {
    f_attr_out_t o;

    memset(&o, 0, sizeof(o));
    if (f_call(n->c, f_new(F_GETATTR, n->id, 16), &o, sizeof(o)) >=
        (int)sizeof(o))
        f_fill(n, &o.attr);
}

static void f_root(f_node_t *n) {
    if (n->id != 1 || n->c->rattr) return;
    n->c->rattr = 1;
    f_getattr(n);
}

static int f_direct(const void *p) {
    uint64_t a = (uint64_t)(uintptr_t)p;

    return a >= KERNEL_VMA && (a < TEMP_MAP_BASE || a >= TEMP_MAP_END);
}

static int f_fopen(f_node_t *n, int w) {
    if (!n->fo && !f_get(n, F_OPEN, n->of < 0 ? (uint32_t)w : (uint32_t)n->of,
                         &n->fh))
        n->fo = 1;
    return !n->fo;
}

static uint64_t f_read(vfs_node_t *v, uint64_t off, uint64_t len,
                       uint8_t *b) {
    f_node_t *n = (f_node_t *)v;
    uint64_t done = 0;
    uint32_t k;
    uint8_t *bb = NULL;
    f_req_t *q;
    f_rw_t *a;
    int r;

    if (VFS_GET_TYPE(v->flags) == VFS_SYMLINK) {
        if (!(bb = (uint8_t *)kmalloc(4096))) return 0;
        r = f_call(n->c, f_new(F_READLINK, n->id, 0), bb, 4096);
        if (r > 0 && (uint64_t)r > off) {
            done = F_MIN(len, (uint64_t)r - off);
            memcpy(b, bb + off, done);
        }
        kfree(bb);
        return done;
    }
    if (VFS_GET_TYPE(v->flags) != VFS_FILE || f_fopen(n, 0)) return 0;
    if (!f_direct(b) && !(bb = (uint8_t *)kmalloc(F_MIN(len, F_BOUNCE))))
        return 0;
    while (done < len) {
        k = (uint32_t)F_MIN(len - done, bb ? F_BOUNCE : UINT32_MAX);
        if (!(q = f_new(F_READ, n->id, sizeof(f_rw_t)))) break;
        a = (f_rw_t *)ARG(q);
        a->fh = n->fh;
        a->off = off + done;
        a->size = k;
        a->flags = n->of < 0 ? 0 : (uint32_t)n->of;
        if ((r = f_call(n->c, q, bb ? bb : b + done, k)) <= 0) break;
        if (bb) memcpy(b + done, bb, (uint32_t)r);
        done += (uint32_t)r;
        if ((uint32_t)r < k) break;
    }
    kfree(bb);
    return done;
}

static uint64_t f_write(vfs_node_t *v, uint64_t off, uint64_t len,
                        uint8_t *b) {
    f_node_t *n = (f_node_t *)v;
    uint64_t done = 0;
    uint32_t o[2];
    uint32_t k;
    f_req_t *q;
    f_rw_t *a;
    int d = f_direct(b);

    if (VFS_GET_TYPE(v->flags) != VFS_FILE || f_fopen(n, 1)) return 0;
    while (done < len) {
        k = (uint32_t)F_MIN(len - done, n->c->max_write);
        if (!(q = f_new(F_WRITE, n->id, sizeof(f_rw_t) + (d ? 0 : k)))) break;
        a = (f_rw_t *)ARG(q);
        a->fh = n->fh;
        a->off = off + done;
        a->size = k;
        a->flags = n->of < 0 ? 0 : (uint32_t)n->of;
        if (d) {
            q->data = b + done;
            q->dlen = k;
            q->in.len += k;
        } else {
            memcpy(a + 1, b + done, k);
        }
        o[0] = 0;
        if (f_call(n->c, q, o, sizeof(o)) < (int)sizeof(o) || !o[0]) break;
        done += F_MIN(o[0], k);
        if (o[0] < k) break;
    }
    if (off + done > v->length) v->length = off + done;
    return done;
}

static void f_vopen(vfs_node_t *v, uint64_t flags) {
    f_node_t *n = (f_node_t *)v;

    if (n->of < 0 && VFS_GET_TYPE(v->flags) == VFS_FILE)
        n->of = (int32_t)(flags & ~(uint64_t)(VFS_O_CREAT | VFS_O_EXCL |
                          VFS_O_NOCTTY | VFS_O_TRUNC | VFS_O_CLOEXEC) &
                          0x7FFFFFFF);
}

static void f_close(vfs_node_t *v) {
    f_node_t *n = (f_node_t *)v;

    if (v->ref_count) return;
    if (n->fo)
        f_rel(n->c, n->id, F_RELEASE, n->fh, n->of < 0 ? 0 : (uint32_t)n->of);
    if (n->dop) f_rel(n->c, n->id, F_RELEASEDIR, n->dfh, 0);
    if (n->id != 1) f_forget(n->c, n->id);
    f_put(n->c);
    vfs_node_release_name(v);
    kfree(n);
}

static vfs_node_t *f_finddir(vfs_node_t *v, const char *name);
static int f_readdir_next(vfs_node_t *v, uint64_t *cookie, dirent_t *e);

static dirent_t *f_readdir(vfs_node_t *v, uint64_t index) {
    static dirent_t dent;
    uint64_t cookie = 0;
    uint64_t i;

    for (i = 0; i <= index; i++)
        if (f_readdir_next(v, &cookie, &dent) != 0) return NULL;
    return &dent;
}

static vfs_node_t *f_mknode(f_conn_t *c, uint64_t id, const f_attr_t *a,
                            const char *name) {
    f_node_t *n = (f_node_t *)kcalloc(1, sizeof(f_node_t));

    if (!n) return NULL;
    if (vfs_node_set_name(&n->vfs, name)) {
        kfree(n);
        return NULL;
    }
    n->c = c;
    n->id = id;
    n->of = -1;
    n->vfs.flags = VFS_DYNAMIC | VFS_EMBEDDED;
    n->vfs.read = f_read;
    n->vfs.write = f_write;
    n->vfs.open = f_vopen;
    n->vfs.close = f_close;
    n->vfs.readdir = f_readdir;
    n->vfs.finddir = f_finddir;
    n->vfs.ops = &f_ops;
    if (a) f_fill(n, a);
    __atomic_add_fetch(&c->refs, 1, __ATOMIC_ACQ_REL);
    return &n->vfs;
}

static vfs_node_t *f_finddir(vfs_node_t *v, const char *name) {
    f_node_t *n = (f_node_t *)v;
    vfs_node_t *r;
    f_entry_t e;

    if (!name || VFS_GET_TYPE(v->flags) != VFS_DIRECTORY) return NULL;
    f_root(n);
    if (f_ncall(n, F_LOOKUP, NULL, 0, name, NULL, &e, sizeof(e)) <
        (int)sizeof(e) || !e.nodeid)
        return NULL;
    if (!(r = f_mknode(n->c, e.nodeid, &e.attr, name))) f_forget(n->c, e.nodeid);
    return r;
}

static int f_readdir_next(vfs_node_t *v, uint64_t *cookie, dirent_t *e) {
    f_node_t *n = (f_node_t *)v;
    uint8_t buf[F_DIRBUF];
    f_dirent_t *d = (f_dirent_t *)buf;
    f_req_t *q;
    f_rw_t *a;
    int r;

    if (!cookie || !e || VFS_GET_TYPE(v->flags) != VFS_DIRECTORY)
        return VFS_READDIR_IO;
    f_root(n);
    if (!*cookie || !n->dop) {
        if (n->dop) f_rel(n->c, n->id, F_RELEASEDIR, n->dfh, 0);
        if (!(n->dop = !f_get(n, F_OPENDIR, 0, &n->dfh))) return VFS_READDIR_IO;
    }
    if (!(q = f_new(F_READDIR, n->id, sizeof(f_rw_t)))) return VFS_READDIR_NOMEM;
    a = (f_rw_t *)ARG(q);
    a->fh = n->dfh;
    a->off = *cookie;
    a->size = sizeof(buf);
    memset(buf, 0, sizeof(buf));
    if ((r = f_call(n->c, q, buf, sizeof(buf))) < 0) return VFS_READDIR_IO;
    if (r < (int)sizeof(f_dirent_t) || d->namelen > r - sizeof(f_dirent_t))
        return VFS_READDIR_END;
    if (vfs_dirent_set_name_n(e, d->name, d->namelen)) return VFS_READDIR_NOMEM;
    e->inode = d->ino;
    e->type = f_types[d->type & 15];
    *cookie = d->off;
    return 0;
}

static int f_create(vfs_node_t *v, const char *name, uint64_t flags) {
    f_node_t *n = (f_node_t *)v;
    struct { f_entry_t e; uint64_t fh; uint32_t flags, pad; } o;
    int excl = (flags & VFS_CREATE_EXCL) != 0;
    uint32_t a[4] = {
        VFS_O_CREAT | VFS_O_RDWR | (excl ? VFS_O_EXCL : 0),
        (flags & VFS_CREATE_SOCKET ? 0140000 : 0100000) | (flags & 07777), 0, 0
    };
    int r = -38;

    if (!(flags & VFS_CREATE_SOCKET)) {
        r = f_ncall(n, F_CREATE, a, 16, name, NULL, &o, sizeof(o));
        if (r >= (int)sizeof(o))
            f_rel(n->c, o.e.nodeid, F_RELEASE, o.fh, a[0]);
        if (r != -38) return f_made(n->c, r, &o.e);
    }
    a[0] = a[1];
    a[1] = 0;
    r = f_ncall(n, F_MKNOD, a, 16, name, NULL, &o.e, sizeof(o.e));
    return r == -17 && !excl ? 0 : f_made(n->c, r, &o.e);
}

static int f_mkdir(vfs_node_t *v, const char *name, uint64_t perms) {
    f_node_t *n = (f_node_t *)v;
    uint32_t a[2] = { (uint32_t)(perms & 07777), 0 };
    f_entry_t e;

    return f_made(n->c, f_ncall(n, F_MKDIR, a, 8, name, NULL, &e, sizeof(e)), &e);
}

static int f_symlink(vfs_node_t *v, const char *name, const char *target) {
    f_node_t *n = (f_node_t *)v;
    f_entry_t e;

    return f_made(n->c, f_ncall(n, F_SYMLINK, NULL, 0, name, target, &e,
                                sizeof(e)), &e);
}

static int f_unlink(vfs_node_t *v, const char *name) {
    f_node_t *n = (f_node_t *)v;
    f_entry_t e;
    int r = f_ncall(n, F_LOOKUP, NULL, 0, name, NULL, &e, sizeof(e));

    if (r < 0) return r;
    f_made(n->c, r, &e);
    r = f_ncall(n, (e.attr.mode & 0170000) == 040000 ? F_RMDIR : F_UNLINK,
                NULL, 0, name, NULL, NULL, 0);
    return r < 0 ? r : 0;
}

static int f_rename(vfs_node_t *ov, const char *on, vfs_node_t *nv,
                    const char *nn) {
    f_node_t *o = (f_node_t *)ov;
    uint64_t dir;
    int r;

    if (nv->ops != &f_ops || ((f_node_t *)nv)->c != o->c) return -18;
    dir = ((f_node_t *)nv)->id;
    r = f_ncall(o, F_RENAME, &dir, 8, on, nn, NULL, 0);
    return r < 0 ? r : 0;
}

static int f_setattr(vfs_node_t *v, const f_setattr_t *s) {
    f_node_t *n = (f_node_t *)v;
    f_req_t *q = f_new(F_SETATTR, n->id, sizeof(f_setattr_t));
    f_attr_out_t o;
    f_setattr_t *a;
    int r;

    if (!q) return -12;
    a = (f_setattr_t *)ARG(q);
    *a = *s;
    if (n->fo) {
        a->valid |= 64;
        a->fh = n->fh;
    }
    memset(&o, 0, sizeof(o));
    if ((r = f_call(n->c, q, &o, sizeof(o))) >= (int)sizeof(o)) f_fill(n, &o.attr);
    return r < 0 ? r : 0;
}

static int f_truncate(vfs_node_t *v, uint64_t len) {
    return f_setattr(v, &(f_setattr_t){ .valid = 8, .size = len });
}

static int f_chmod(vfs_node_t *v, uint64_t mode) {
    return f_setattr(v, &(f_setattr_t){ .valid = 1,
                                        .mode = (uint32_t)(mode & 07777) });
}

static int f_chown(vfs_node_t *v, uint64_t uid, uint64_t gid) {
    return f_setattr(v, &(f_setattr_t){
        .valid = (uid != (uint64_t)-1 ? 2u : 0) | (gid != (uint64_t)-1 ? 4u : 0),
        .uid = (uint32_t)uid, .gid = (uint32_t)gid });
}

static int f_set_times(vfs_node_t *v, uint64_t atime, uint64_t mtime,
                       uint64_t ctime) {
    (void)ctime;
    return f_setattr(v, &(f_setattr_t){ .valid = 16 | 32, .atime = atime,
                                        .mtime = mtime });
}

static const vfs_node_ops_t f_ops = {
    .create = f_create,
    .unlink = f_unlink,
    .mkdir = f_mkdir,
    .symlink = f_symlink,
    .truncate = f_truncate,
    .rename = f_rename,
    .chmod = f_chmod,
    .chown = f_chown,
    .readdir_next = f_readdir_next,
    .set_times = f_set_times
};

static uint64_t f_opt(const char *s, const char *k, uint64_t base,
                      uint64_t v) {
    size_t n = strlen(k);

    while (s && *s) {
        if (!strncmp(s, k, n) && s[n] == '=') {
            for (s += n + 1, v = 0; *s >= '0' && *s < (char)('0' + base); s++)
                v = v * base + (uint64_t)(*s - '0');
            return v;
        }
        if ((s = strchr(s, ','))) s++;
    }
    return v;
}

static vfs_node_t *f_mount(const char *device, const char *mountpoint,
                           const char *data) {
    uint64_t fd = f_opt(data, "fd", 10, UINT64_MAX);
    uint64_t mode = f_opt(data, "rootmode", 8, 040755) & 07777;
    vfs_node_t *root;
    task_fd_t *t;
    f_conn_t *c;
    f_req_t *q;
    uint32_t *a;

    (void)device;
    (void)mountpoint;
    if (!current_task || fd >= (uint64_t)current_task->fds_capacity) return NULL;
    t = &current_task->fds[fd];
    if (!t->in_use || t->type != FD_TYPE_FILE || !t->node ||
        ((vfs_node_t *)t->node)->read != f_dev_read)
        return NULL;
    c = (f_conn_t *)t->node;
    if (c->mounted || c->dead || !(q = f_new(F_INIT, 0, 16 + 24))) return NULL;
    if (!(root = f_mknode(c, 1, NULL, ""))) {
        kfree(q);
        return NULL;
    }
    root->flags |= VFS_DIRECTORY;
    root->inode = 1;
    root->mask = (uint32_t)(mode ? mode : 0755);
    root->uid = (uint32_t)f_opt(data, "user_id", 10, 0);
    root->gid = (uint32_t)f_opt(data, "group_id", 10, 0);
    __atomic_add_fetch(&root->ref_count, 1, __ATOMIC_ACQ_REL);
    a = (uint32_t *)ARG(q);
    a[0] = 7;
    a[1] = 31;
    a[3] = 1 | 32;
    q->in.len = (uint32_t)sizeof(f_in_t) + 16;
    q->out = a + 4;
    q->cap = 24;
    c->mounted = 1;
    f_bg(c, q);
    return root;
}

static int f_umount(vfs_node_t *root) {
    if (!root) return -1;
    f_abort(((f_node_t *)root)->c);
    __atomic_sub_fetch(&root->ref_count, 1, __ATOMIC_ACQ_REL);
    vfs_release(root);
    return 0;
}

static int f_sync(vfs_node_t *v, int data_only) {
    f_node_t *n = (f_node_t *)v;
    f_req_t *q;
    int r;

    if (!n || n->vfs.ops != &f_ops || !n->fo) return 0;
    if (!(q = f_new(F_FSYNC, n->id, 16))) return -1;
    *(uint64_t *)ARG(q) = n->fh;
    *(uint32_t *)(ARG(q) + 8) = data_only ? 1 : 0;
    r = f_call(n->c, q, NULL, 0);
    return r < 0 && r != -38 ? -1 : 0;
}

static int f_statfs(vfs_node_t *root, vfs_statfs_t *st) {
    f_node_t *n = (f_node_t *)root;
    uint64_t o[10];

    memset(o, 0, sizeof(o));
    if (f_call(n->c, f_new(F_STATFS, n->id, 0), o, sizeof(o)) < 48) return -1;
    st->type = 0x65735546;
    st->bsize = (uint32_t)o[5];
    st->blocks = o[0];
    st->bfree = o[1];
    return 0;
}

static vfs_fs_type_t f_fs_type = {
    .name = "fuse",
    .mount_data = f_mount,
    .unmount = f_umount,
    .sync = f_sync,
    .statfs = f_statfs
};

void KERNEL_INIT fuse_vfs_register(void) {
    vfs_register_fs(&f_fs_type);
}
