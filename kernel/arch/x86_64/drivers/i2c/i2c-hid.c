#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <lebirun/acpi.h>
#include <lebirun/mouse.h>
#include <lebirun/pit.h>
#include <lebirun/framebuffer.h>
#include <lebirun/timekeeping.h>
#include <lebirun/drivers/i2c/dw-i2c.h>
#include <lebirun/drivers/i2c/i2c-hid.h>
#include <lebirun/lke.h>

#define TLC_POINTER       0x00010001u
#define TLC_MOUSE         0x00010002u
#define TLC_TOUCHPAD      0x000D0005u
#define HID_X             0x00010030u
#define HID_Y             0x00010031u
#define HID_WHEEL         0x00010038u
#define HID_BUTTON1       0x00090001u
#define DG_FINGER         0x000D0022u
#define DG_TIP            0x000D0042u
#define DG_CONTACT_ID     0x000D0051u
#define DG_INPUT_MODE     0x000D0052u
#define DG_CONTACT_COUNT  0x000D0054u
#define DG_SURFACE_SWITCH 0x000D0057u
#define DG_BUTTON_SWITCH  0x000D0058u

typedef struct {
    uint32_t off;
    uint8_t size;
    uint8_t sign;
} hid_field_t;

typedef struct {
    hid_field_t tip;
    hid_field_t id;
    hid_field_t x;
    hid_field_t y;
} hid_finger_t;

typedef struct {
    hid_field_t f;
    uint32_t bytes;
    uint8_t rid;
} hid_feature_t;

typedef struct {
    uint32_t id;
    int32_t x;
    int32_t y;
    uint8_t tip;
} hid_contact_t;

typedef struct i2c_hid {
    struct i2c_hid *next;
    dw_i2c_t *bus;
    uint8_t *buf;
    hid_finger_t *fg;
    hid_contact_t *ct;
    hid_field_t cc;
    hid_field_t tbtn;
    hid_field_t mbtn[3];
    hid_field_t mx;
    hid_field_t my;
    hid_field_t mw;
    hid_feature_t feat[3];
    uint64_t next_ns;
    uint64_t down_ns;
    int64_t ax;
    int64_t ay;
    int64_t aw;
    int32_t xmin;
    int32_t xmax;
    int32_t ymin;
    int32_t ymax;
    int32_t lx;
    int32_t ly;
    int32_t travel;
    uint32_t nf;
    uint32_t nct;
    uint32_t cct;
    uint32_t expect;
    uint32_t seen;
    uint32_t pid;
    uint32_t last_n;
    uint16_t addr;
    uint16_t cmd_reg;
    uint16_t data_reg;
    uint16_t in_len;
    uint8_t fast;
    uint8_t ids;
    uint8_t tp_rid;
    uint8_t m_rid;
    uint8_t pbtn;
    uint8_t btn;
    uint8_t prev;
    uint8_t fingers;
    uint8_t have;
    uint8_t tap;
} i2c_hid_t;

typedef struct {
    uint16_t *addr;
    uint8_t *fast;
    uint32_t n;
} i2c_hid_cands_t;

static i2c_hid_t *i2c_hid_devs;
static i2c_hid_t *i2c_hid_cur;

static int32_t hid_get(const uint8_t *r, uint32_t n, const hid_field_t *f) {
    uint32_t v;
    uint32_t b;
    uint32_t i;

    if (!f->size || f->off + f->size > n * 8) return 0;
    v = 0;
    for (i = 0; i < f->size; i++) {
        b = f->off + i;
        v |= (uint32_t)((r[b >> 3] >> (b & 7)) & 1) << i;
    }
    if (f->sign && f->size < 32 && ((v >> (f->size - 1)) & 1))
        v |= ~0u << f->size;
    return (int32_t)v;
}

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void i2c_hid_emit(uint8_t b, int32_t dx, int32_t dy, int32_t w) {
    int32_t cx;
    int32_t cy;
    int32_t cw;
    int has_z;

    has_z = mouse_get_packet_size() == 4;
    if (!has_z) w = 0;
    do {
        cx = clampi(dx, -127, 127);
        cy = clampi(dy, -127, 127);
        cw = clampi(w, -8, 7);
        dx -= cx;
        dy -= cy;
        w -= cw;
        mouse_inject_packet(b, (int8_t)cx, (int8_t)-cy, (int8_t)cw, has_z);
    } while (dx || dy || w);
}

static void i2c_hid_mouse(i2c_hid_t *d, const uint8_t *r, uint32_t n) {
    uint32_t i;
    int32_t dx;
    int32_t dy;
    int32_t w;
    uint8_t b;

    b = 0;
    for (i = 0; i < 3; i++)
        if (hid_get(r, n, &d->mbtn[i])) b |= (uint8_t)(1 << i);
    dx = hid_get(r, n, &d->mx);
    dy = hid_get(r, n, &d->my);
    w = -hid_get(r, n, &d->mw);
    if (b != d->btn || dx || dy || w) i2c_hid_emit(b, dx, dy, w);
    d->btn = b;
}

static void tp_contact(i2c_hid_t *d, uint32_t id, uint8_t tip, int32_t x,
                       int32_t y) {
    hid_contact_t *c;
    uint32_t i;

    for (i = 0; i < d->nct && d->ct[i].id != id; i++)
        ;
    if (i == d->nct) {
        if (!tip) return;
        if (d->nct == d->cct) {
            c = krealloc(d->ct, (d->cct + 1) * sizeof(*c));
            if (!c) return;
            d->ct = c;
            d->cct++;
        }
        d->nct++;
    }
    d->ct[i].id = id;
    d->ct[i].tip = tip;
    d->ct[i].x = x;
    d->ct[i].y = y;
}

static void tp_frame(i2c_hid_t *d) {
    hid_contact_t *p;
    framebuffer_t *fb;
    uint64_t now;
    uint32_t i;
    uint32_t k;
    uint32_t n;
    int32_t dx;
    int32_t dy;
    int32_t px;
    int32_t py;
    int32_t w;
    int32_t xr;
    int32_t yr;
    int32_t step;

    p = NULL;
    n = 0;
    for (i = 0; i < d->nct; i++) {
        if (!d->ct[i].tip) continue;
        n++;
        if (!p || d->ct[i].id == d->pid) p = &d->ct[i];
    }
    now = timekeeping_monotonic_ns();
    xr = d->xmax > d->xmin ? d->xmax - d->xmin : 1;
    yr = d->ymax > d->ymin ? d->ymax - d->ymin : 1;
    dx = dy = px = py = w = 0;
    if (n && !d->fingers) {
        d->down_ns = now;
        d->travel = 0;
        d->tap = 1;
    }
    if (n > d->fingers) d->fingers = (uint8_t)n;
    if (p && d->have && p->id == d->pid && n == d->last_n) {
        dx = p->x - d->lx;
        dy = p->y - d->ly;
        d->travel += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    }
    d->have = p != NULL;
    d->last_n = n;
    if (p) {
        d->pid = p->id;
        d->lx = p->x;
        d->ly = p->y;
    }
    if (!d->pbtn) {
        d->btn = 0;
    } else if (!d->btn) {
        d->btn = n >= 2 ? 2 : 1;
        d->tap = 0;
    }
    if (n >= 2) {
        step = yr / 16 ? yr / 16 : 1;
        d->aw += dy;
        w = (int32_t)(d->aw / step);
        d->aw -= (int64_t)w * step;
    } else if (n == 1) {
        fb = fb_get();
        d->ax += (int64_t)dx * (int64_t)(fb && fb->width ? fb->width : 1024);
        d->ay += (int64_t)dy * (int64_t)(fb && fb->height ? fb->height : 768);
        px = (int32_t)(d->ax / xr);
        py = (int32_t)(d->ay / yr);
        d->ax -= (int64_t)px * xr;
        d->ay -= (int64_t)py * yr;
    }
    if (!n && d->fingers) {
        if (d->tap && now - d->down_ns < 200000000ULL && d->travel < xr / 32) {
            i2c_hid_emit(d->fingers >= 2 ? 2 : 1, 0, 0, 0);
            i2c_hid_emit(0, 0, 0, 0);
        }
        d->fingers = 0;
        d->ax = d->ay = d->aw = 0;
    }
    for (i = k = 0; i < d->nct; i++)
        if (d->ct[i].tip) d->ct[k++] = d->ct[i];
    d->nct = k;
    if (px || py || w || d->btn != d->prev) i2c_hid_emit(d->btn, px, py, w);
    d->prev = d->btn;
}

static void i2c_hid_touchpad(i2c_hid_t *d, const uint8_t *r, uint32_t n) {
    hid_finger_t *f;
    uint32_t cc;
    uint32_t i;
    uint32_t slots;

    cc = d->cc.size ? (uint32_t)hid_get(r, n, &d->cc) : d->nf;
    if (cc) {
        d->expect = cc;
        d->seen = 0;
    }
    slots = d->expect > d->seen ? d->expect - d->seen : 0;
    if (slots > d->nf) slots = d->nf;
    for (i = 0; i < slots; i++) {
        f = &d->fg[i];
        tp_contact(d, f->id.size ? (uint32_t)hid_get(r, n, &f->id) : i,
                   f->tip.size ? hid_get(r, n, &f->tip) != 0 : 1,
                   hid_get(r, n, &f->x), hid_get(r, n, &f->y));
    }
    d->seen += slots;
    d->pbtn = hid_get(r, n, &d->tbtn) != 0;
    if (d->seen >= d->expect) tp_frame(d);
}

static void i2c_hid_input(i2c_hid_t *d, const uint8_t *r, uint32_t n) {
    uint8_t id;

    id = 0;
    if (d->ids) {
        if (!n) return;
        id = *r++;
        n--;
    }
    if (d->nf && id == d->tp_rid)
        i2c_hid_touchpad(d, r, n);
    else if (d->mx.size && id == d->m_rid)
        i2c_hid_mouse(d, r, n);
}

static void i2c_hid_poll(uint64_t ticks) {
    i2c_hid_t *d;
    uint64_t now;
    uint32_t len;
    int r;

    (void)ticks;
    if ((d = i2c_hid_cur)) {
        r = dw_i2c_pump(d->bus);
        now = timekeeping_monotonic_ns();
        if (!r && now - d->bus->t < 50000000ULL) return;
        if (!r) dw_i2c_abort(d->bus);
        i2c_hid_cur = NULL;
        d->next_ns = now + 10000000ULL;
        len = d->buf[0] | ((uint32_t)d->buf[1] << 8);
        if (r > 0 && len > 2 && len <= d->in_len) {
            i2c_hid_input(d, d->buf + 2, len - 2);
            d->next_ns = now + 4000000ULL;
        }
    }
    now = timekeeping_monotonic_ns();
    for (d = i2c_hid_devs; d; d = d->next) {
        if (now < d->next_ns) continue;
        if (!dw_i2c_begin(d->bus, d->addr, d->fast, NULL, 0, d->buf,
                          d->in_len)) {
            i2c_hid_cur = d;
            return;
        }
    }
}

static uint32_t KERNEL_INIT hid_usage(const uint32_t *u, uint32_t nu,
                                      uint32_t i) {
    uint32_t k;
    uint32_t span;

    for (k = 0; k < nu; k++) {
        span = u[2 * k + 1] - u[2 * k] + 1;
        if (i < span) return u[2 * k] + i;
        i -= span;
    }
    return nu ? u[2 * nu - 1] : 0;
}

static void KERNEL_INIT hid_field(i2c_hid_t *d, uint32_t type, uint32_t rid,
                                  uint32_t tlc, uint32_t usage, int32_t finger,
                                  uint32_t flags, hid_field_t f, int32_t lmin,
                                  int32_t lmax) {
    hid_finger_t *fg;
    uint32_t k;

    if (type == 2) {
        k = usage == DG_INPUT_MODE ? 0 :
            usage == DG_SURFACE_SWITCH ? 1 :
            usage == DG_BUTTON_SWITCH ? 2 : 3;
        if (k < 3 && !d->feat[k].f.size) {
            d->feat[k].f = f;
            d->feat[k].rid = (uint8_t)rid;
        }
        return;
    }
    if (tlc == TLC_TOUCHPAD) {
        if (usage == DG_CONTACT_COUNT) d->cc = f;
        else if (usage == HID_BUTTON1) d->tbtn = f;
        if (finger < 0) return;
        if ((uint32_t)finger >= d->nf) {
            fg = krealloc(d->fg, ((uint32_t)finger + 1) * sizeof(*fg));
            if (!fg) return;
            memset(fg + d->nf, 0, ((uint32_t)finger + 1 - d->nf) * sizeof(*fg));
            d->fg = fg;
            d->nf = (uint32_t)finger + 1;
        }
        fg = &d->fg[finger];
        d->tp_rid = (uint8_t)rid;
        if (usage == DG_TIP) {
            fg->tip = f;
        } else if (usage == DG_CONTACT_ID) {
            fg->id = f;
        } else if (usage == HID_X) {
            fg->x = f;
            d->xmin = lmin;
            d->xmax = lmax;
        } else if (usage == HID_Y) {
            fg->y = f;
            d->ymin = lmin;
            d->ymax = lmax;
        }
        return;
    }
    if (tlc != TLC_MOUSE && tlc != TLC_POINTER) return;
    if (usage >= HID_BUTTON1 && usage < HID_BUTTON1 + 3) {
        if (!d->mbtn[usage - HID_BUTTON1].size) d->mbtn[usage - HID_BUTTON1] = f;
        return;
    }
    if (!(flags & 4)) return;
    if (usage == HID_X && !d->mx.size) {
        d->mx = f;
        d->m_rid = (uint8_t)rid;
    } else if (usage == HID_Y && !d->my.size) {
        d->my = f;
    } else if (usage == HID_WHEEL && !d->mw.size) {
        d->mw = f;
    }
}

static void KERNEL_INIT hid_parse(i2c_hid_t *d, const uint8_t *p, uint32_t n) {
    hid_field_t f;
    uint32_t *offs;
    uint32_t *u;
    uint32_t *nu_buf;
    uint32_t nu;
    uint32_t cap;
    uint32_t page;
    uint32_t rsize;
    uint32_t rcount;
    uint32_t rid;
    uint32_t tlc;
    uint32_t depth;
    uint32_t fdepth;
    uint32_t umin;
    uint32_t usage;
    uint32_t lmax_u;
    uint32_t type;
    uint32_t tag;
    uint32_t sz;
    uint32_t v;
    uint32_t i;
    int32_t lmin;
    int32_t lmax_s;
    int32_t sv;
    int32_t finger;
    int32_t nfing;

    offs = kcalloc(3 * 256, sizeof(*offs));
    if (!offs) return;
    u = NULL;
    nu = cap = page = rsize = rcount = rid = tlc = depth = fdepth = umin = 0;
    lmax_u = 0;
    lmin = lmax_s = 0;
    finger = -1;
    nfing = 0;
    while (n) {
        if (*p == 0xFE) {
            sz = n >= 2 ? p[1] + 3u : n;
            if (sz > n) sz = n;
            p += sz;
            n -= sz;
            continue;
        }
        sz = (*p & 3) == 3 ? 4 : (*p & 3);
        if (n < sz + 1) break;
        v = 0;
        for (i = 0; i < sz; i++) v |= (uint32_t)p[1 + i] << (8 * i);
        sv = sz == 1 ? (int8_t)v : (sz == 2 ? (int16_t)v : (int32_t)v);
        tag = *p & 0xFC;
        p += sz + 1;
        n -= sz + 1;
        switch (tag) {
        case 0x04: page = v; break;
        case 0x14: lmin = sv; break;
        case 0x24: lmax_s = sv; lmax_u = v; break;
        case 0x74: rsize = v; break;
        case 0x84: rid = v & 0xFF; d->ids = 1; break;
        case 0x94: rcount = v; break;
        case 0x18: umin = sz == 4 ? v : (page << 16) | v; break;
        case 0x08:
        case 0x28:
            usage = sz == 4 ? v : (page << 16) | v;
            if (nu == cap) {
                nu_buf = krealloc(u, (cap ? cap * 2 : 8) * 2 * sizeof(*u));
                if (!nu_buf) break;
                u = nu_buf;
                cap = cap ? cap * 2 : 8;
            }
            u[2 * nu] = tag == 0x08 ? usage : umin;
            u[2 * nu + 1] = usage >= u[2 * nu] ? usage : u[2 * nu];
            nu++;
            break;
        case 0xA0:
            usage = nu ? u[0] : 0;
            if (++depth == 1) tlc = usage;
            if (usage == DG_FINGER && !fdepth) {
                finger = nfing++;
                fdepth = depth;
            }
            nu = 0;
            break;
        case 0xC0:
            if (depth && depth == fdepth) {
                finger = -1;
                fdepth = 0;
            }
            if (depth) depth--;
            nu = 0;
            break;
        case 0x80:
        case 0x90:
        case 0xB0:
            type = tag == 0x80 ? 0 : (tag == 0x90 ? 1 : 2);
            f.size = (uint8_t)(rsize <= 32 ? rsize : 0);
            f.sign = lmin < 0;
            if (type != 1 && !(v & 1) && (v & 2) && f.size) {
                for (i = 0; i < rcount; i++) {
                    f.off = offs[type * 256 + rid] + i * rsize;
                    hid_field(d, type, rid, tlc, hid_usage(u, nu, i), finger,
                              v, f, lmin, lmin < 0 ? lmax_s : (int32_t)lmax_u);
                }
            }
            offs[type * 256 + rid] += rsize * rcount;
            nu = 0;
            break;
        }
    }
    for (i = 0; i < 3; i++)
        if (d->feat[i].f.size)
            d->feat[i].bytes = (offs[512 + d->feat[i].rid] + 7) / 8;
    kfree(u);
    kfree(offs);
}

static int KERNEL_INIT hid_xfer(i2c_hid_t *d, const uint8_t *w, uint32_t wn,
                                uint8_t *r, uint32_t rn) {
    return dw_i2c_xfer(d->bus, d->addr, d->fast, w, wn, r, rn);
}

static void KERNEL_INIT hid_feature(i2c_hid_t *d, hid_feature_t *ft,
                                    uint32_t val) {
    uint8_t *w;
    uint8_t *r;
    uint8_t *pl;
    uint32_t len;
    uint32_t k;
    uint32_t i;
    uint32_t b;
    uint8_t id;

    if (!ft->f.size || !ft->bytes) return;
    id = ft->rid;
    len = ft->bytes + d->ids + 2;
    w = kmalloc(ft->bytes + 16);
    r = kcalloc(1, len);
    if (!w || !r) goto out;
    k = 0;
    w[k++] = (uint8_t)d->cmd_reg;
    w[k++] = (uint8_t)(d->cmd_reg >> 8);
    w[k++] = (uint8_t)(0x30 | (id < 15 ? id : 15));
    w[k++] = 0x02;
    if (id >= 15) w[k++] = id;
    w[k++] = (uint8_t)d->data_reg;
    w[k++] = (uint8_t)(d->data_reg >> 8);
    if (hid_xfer(d, w, k, r, len)) memset(r, 0, len);
    pl = r + 2 + d->ids;
    for (i = 0; i < ft->f.size; i++) {
        b = ft->f.off + i;
        if ((val >> i) & 1) pl[b >> 3] |= (uint8_t)(1 << (b & 7));
        else pl[b >> 3] &= (uint8_t)~(1 << (b & 7));
    }
    k = 0;
    w[k++] = (uint8_t)d->cmd_reg;
    w[k++] = (uint8_t)(d->cmd_reg >> 8);
    w[k++] = (uint8_t)(0x30 | (id < 15 ? id : 15));
    w[k++] = 0x03;
    if (id >= 15) w[k++] = id;
    w[k++] = (uint8_t)d->data_reg;
    w[k++] = (uint8_t)(d->data_reg >> 8);
    w[k++] = (uint8_t)len;
    w[k++] = (uint8_t)(len >> 8);
    if (d->ids) w[k++] = id;
    memcpy(w + k, pl, ft->bytes);
    hid_xfer(d, w, k + ft->bytes, NULL, 0);
out:
    kfree(w);
    kfree(r);
}

static void KERNEL_INIT i2c_hid_probe(dw_i2c_t *bus, uint16_t addr,
                                      uint8_t fast) {
    static const uint16_t regs[] KERNEL_INIT_RODATA = {
        0x0020, 0x0001, 0x0000, 0x0010, 0x0030
    };
    i2c_hid_t *d;
    uint8_t *rd;
    uint8_t h[30];
    uint8_t w[4];
    uint32_t rd_len;
    uint32_t i;
    int found;

    d = kcalloc(1, sizeof(*d));
    if (!d) return;
    d->bus = bus;
    d->addr = addr;
    found = 0;
    for (i = 0; i < sizeof(regs) / sizeof(regs[0]) && !found; i++) {
        w[0] = (uint8_t)regs[i];
        w[1] = (uint8_t)(regs[i] >> 8);
        if (hid_xfer(d, w, 2, h, sizeof(h))) {
            if (i) continue;
            dw_i2c_delay(1);
            if (hid_xfer(d, w, 2, h, sizeof(h))) break;
        }
        found = h[0] == 30 && !h[1] && !h[2] && h[3] == 1;
    }
    if (!found) goto fail;
    rd_len = h[4] | ((uint32_t)h[5] << 8);
    d->in_len = (uint16_t)(h[10] | (h[11] << 8));
    if (!rd_len || d->in_len < 3) goto fail;
    d->cmd_reg = (uint16_t)(h[16] | (h[17] << 8));
    d->data_reg = (uint16_t)(h[18] | (h[19] << 8));
    d->fast = fast;
    d->buf = kmalloc(d->in_len);
    rd = kmalloc(rd_len);
    if (!d->buf || !rd) {
        kfree(rd);
        goto fail;
    }
    w[0] = (uint8_t)d->cmd_reg;
    w[1] = (uint8_t)(d->cmd_reg >> 8);
    w[2] = 0x00;
    w[3] = 0x08;
    hid_xfer(d, w, 4, NULL, 0);
    dw_i2c_delay(1);
    w[3] = 0x01;
    hid_xfer(d, w, 4, NULL, 0);
    dw_i2c_delay(100);
    for (i = 0; i < 10; i++) {
        if (!hid_xfer(d, NULL, 0, d->buf, d->in_len) && !d->buf[0] &&
            !d->buf[1])
            break;
        dw_i2c_delay(10);
    }
    w[0] = h[6];
    w[1] = h[7];
    if (hid_xfer(d, w, 2, rd, rd_len)) {
        kfree(rd);
        goto fail;
    }
    hid_parse(d, rd, rd_len);
    kfree(rd);
    if (!d->nf && !(d->mx.size && d->my.size)) goto fail;
    if (d->nf) {
        hid_feature(d, &d->feat[0], 3);
        hid_feature(d, &d->feat[1], 1);
        hid_feature(d, &d->feat[2], 1);
    }
    KERNEL_INIT_LOG("I2C-HID: %04x:%04x at 0x%02x (%s)\n",
                    h[20] | (h[21] << 8), h[22] | (h[23] << 8), addr,
                    d->nf && d->feat[0].f.size ? KERNEL_INIT_STRING("touchpad")
                                               : KERNEL_INIT_STRING("mouse"));
    d->next = i2c_hid_devs;
    i2c_hid_devs = d;
    return;
fail:
    kfree(d->buf);
    kfree(d->fg);
    kfree(d);
}

static void KERNEL_INIT i2c_hid_add(i2c_hid_cands_t *l, uint16_t addr,
                                    uint8_t fast) {
    uint16_t *a;
    uint8_t *f;
    uint32_t i;

    for (i = 0; i < l->n; i++) {
        if (l->addr[i] != addr) continue;
        l->fast[i] &= fast;
        return;
    }
    a = krealloc(l->addr, (l->n + 1) * sizeof(*a));
    if (a) l->addr = a;
    f = a ? krealloc(l->fast, l->n + 1) : NULL;
    if (!f) return;
    l->fast = f;
    l->addr[l->n] = addr;
    l->fast[l->n++] = fast;
}

static void KERNEL_INIT i2c_hid_acpi_res(const uint8_t *res, uint32_t len,
                                         void *ctx) {
    uint32_t speed;
    uint16_t addr;

    if (res[0] != 0x8E || len < 18 || res[5] != 1 || (res[7] & 1)) return;
    memcpy(&speed, res + 12, 4);
    addr = (uint16_t)(res[16] | (res[17] << 8));
    if (addr && addr < 0x80)
        i2c_hid_add((i2c_hid_cands_t *)ctx, addr, speed >= 400000);
}

static void KERNEL_INIT i2c_hid_scan(i2c_hid_cands_t *l) {
    i2c_hid_t *d;
    uint32_t b;
    uint32_t i;

    for (b = 0; b < dw_i2c_count; b++) {
        for (i = 0; i < l->n; i++) {
            for (d = i2c_hid_devs; d; d = d->next)
                if (d->bus == &dw_i2c_buses[b] && d->addr == l->addr[i]) break;
            if (!d) i2c_hid_probe(&dw_i2c_buses[b], l->addr[i], l->fast[i]);
        }
    }
}

int KERNEL_INIT i2c_hid_init(void) {
    static const uint8_t fallback[] KERNEL_INIT_RODATA = {
        0x2C, 0x15, 0x10, 0x2A, 0x20, 0x38
    };
    const char *ids[] = {
        KERNEL_INIT_STRING("PNP0C50"), KERNEL_INIT_STRING("ACPI0C50"), NULL
    };
    i2c_hid_cands_t l;
    uint32_t i;

    if (!dw_i2c_init()) return -1;
    memset(&l, 0, sizeof(l));
    acpi_scan_devices(ids, i2c_hid_acpi_res, &l);
    i2c_hid_scan(&l);
    if (!i2c_hid_devs) {
        l.n = 0;
        for (i = 0; i < sizeof(fallback); i++) i2c_hid_add(&l, fallback[i], 0);
        i2c_hid_scan(&l);
    }
    kfree(l.addr);
    kfree(l.fast);
    if (!i2c_hid_devs) {
        dw_i2c_release();
        return -1;
    }
    pit_register_callback(i2c_hid_poll, 1, false);
    return 0;
}

#ifndef __is_kernel
module_init(i2c_hid_init);
LKE_NAME("i2c");
LKE_DESC("DesignWare I2C and I2C HID drivers");
LKE_LICENSE("GPLv2");
LKE_VERSION_STR("1.0.0");
#endif
