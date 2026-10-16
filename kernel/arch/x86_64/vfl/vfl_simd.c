#include "vfl_priv.h"
#include "../syscalls/syscall_defs.h"
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <string.h>
#include <stdint.h>

static int host_rdrand_ok = -1;

int vfl_host_rdrand(void) {
    uint32_t a = 1;
    uint32_t b;
    uint32_t c;
    uint32_t d;

    if (host_rdrand_ok >= 0) return host_rdrand_ok;
    __asm__ volatile ("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    host_rdrand_ok = (c >> 30) & 1;
    return host_rdrand_ok;
}

static int host_rdrand64(uint64_t *v) {
    uint8_t ok = 0;
    int i;

    for (i = 0; i < 10; i++) {
        __asm__ volatile (".byte 0x48, 0x0f, 0xc7, 0xf0; setc %1"
                          : "=a"(*v), "=qm"(ok) : : "cc");
        if (ok) return 1;
    }
    return 0;
}

static __attribute__((always_inline)) inline long double f80_to_ld(uint64_t m, uint16_t e) {
    long double x = 0;

    memcpy(&x, &m, 8);
    memcpy((uint8_t *)&x + 8, &e, 2);
    return x;
}

static void ld_to_f80(long double x, uint64_t *m, uint16_t *e) {
    memcpy(m, &x, 8);
    memcpy(e, (uint8_t *)&x + 8, 2);
}

static int fpu_top(vfl_vcpu_t *v) {
    return (v->fpu.fsw >> 11) & 7;
}

static void fpu_set_top(vfl_vcpu_t *v, int t) {
    v->fpu.fsw = (uint16_t)((v->fpu.fsw & ~(7 << 11)) | ((t & 7) << 11));
}

static int fpu_tag(vfl_vcpu_t *v, int r) {
    return (v->fpu.ftw >> (r * 2)) & 3;
}

static void fpu_set_tag(vfl_vcpu_t *v, int r, int tag) {
    v->fpu.ftw = (uint16_t)((v->fpu.ftw & ~(3 << (r * 2))) | ((tag & 3) << (r * 2)));
}

static __attribute__((always_inline)) inline long double fpu_get(vfl_vcpu_t *v, int i) {
    int r = (fpu_top(v) + i) & 7;

    return f80_to_ld(v->fpu.fpr_m[r], v->fpu.fpr_e[r]);
}

static void fpu_set(vfl_vcpu_t *v, int i, long double x) {
    int r = (fpu_top(v) + i) & 7;
    uint64_t m;
    uint16_t e;

    ld_to_f80(x, &m, &e);
    v->fpu.fpr_m[r] = m;
    v->fpu.fpr_e[r] = e;
    if ((e & 0x7FFF) == 0 && (m == 0)) fpu_set_tag(v, r, 1);
    else fpu_set_tag(v, r, 0);
}

static int fpu_push(vfl_vcpu_t *v, long double x) {
    int i;

    for (i = 0; i < 8; i++) {
        if (fpu_tag(v, i) == 3) break;
    }
    if (i == 8) {
        v->fpu.fsw |= 0x41;
        return -1;
    }
    fpu_set_top(v, (fpu_top(v) - 1) & 7);
    fpu_set(v, 0, x);
    return 0;
}

static int fpu_pop(vfl_vcpu_t *v, long double *x) {
    int r = fpu_top(v);

    if (fpu_tag(v, r) == 3) {
        v->fpu.fsw |= 0x41;
        if (x) *x = f80_to_ld(0xC000000000000000ULL, 0xFFFF);
        return -1;
    }
    if (x) *x = fpu_get(v, 0);
    fpu_set_tag(v, r, 3);
    fpu_set_top(v, (r + 1) & 7);
    return 0;
}

static void fpu_init(vfl_vcpu_t *v) {
    int i;

    v->fpu.fcw = 0x37F;
    v->fpu.fsw = 0;
    v->fpu.ftw = 0xFFFF;
    v->fpu.fop = 0;
    v->fpu.fip = 0;
    v->fpu.fcs = 0;
    v->fpu.fdp = 0;
    v->fpu.fds = 0;
    for (i = 0; i < 8; i++) {
        v->fpu.fpr_m[i] = 0;
        v->fpu.fpr_e[i] = 0;
    }
}



static __attribute__((always_inline)) inline long double f80_sqrt(long double x) {
    long double r = x;

    __asm__ volatile ("fsqrt" : "+t"(r));
    return r;
}

static __attribute__((always_inline)) inline long double f80_sin(long double x) {
    long double r = x;

    __asm__ volatile ("fsin" : "+t"(r));
    return r;
}

static __attribute__((always_inline)) inline long double f80_cos(long double x) {
    long double r = x;

    __asm__ volatile ("fcos" : "+t"(r));
    return r;
}

static __attribute__((always_inline)) inline long double f80_atan2(long double y, long double x) {
    long double r = x;

    __asm__ volatile ("fpatan" : "+t"(r) : "u"(y) : "st(1)");
    return r;
}

static __attribute__((always_inline)) inline long double f80_2xm1(long double x) {
    long double r = x;

    __asm__ volatile ("f2xm1" : "+t"(r));
    return r;
}

static __attribute__((always_inline)) inline long double f80_scale(long double x, long double n) {
    __asm__ volatile ("fscale" : "+t"(x) : "u"(n) : "st(1)");
    return x;
}

static __attribute__((always_inline)) inline long double f80_prem(long double x, long double y, int want1, uint16_t *sw) {
    uint16_t s = 0;
    int i;

    for (i = 0; i < 64; i++) {
        if (want1) __asm__ volatile ("fprem1" : "+t"(x) : "u"(y) : "st(1)");
        else __asm__ volatile ("fprem" : "+t"(x) : "u"(y) : "st(1)");
        __asm__ volatile ("fnstsw %0" : "=m"(s));
        if (!(s & 0x400)) break;
    }
    if (sw) *sw = s;
    return x;
}

static __attribute__((always_inline)) inline long double f80_y12x(long double y, long double x) {
    long double r = x;

    __asm__ volatile ("fyl2x" : "+t"(r) : "u"(y) : "st(1)");
    return r;
}

static __attribute__((always_inline)) inline long double f80_round(long double x) {
    long double r = x;

    __asm__ volatile ("frndint" : "+t"(r));
    return r;
}

void vec_read(vfl_vcpu_t *v, int r, int lanes, uint32_t *d) {
    uint8_t lo[16];
    uint8_t hi[16];
    int i;

    memcpy(lo, &v->fpu.xmm[r & 15], 16);
    memcpy(hi, &v->fpu.ymm_hi[r & 15], 16);
    for (i = 0; i < 4 && i < lanes; i++)
        memcpy(&d[i], lo + i * 4, 4);
    for (i = 4; i < lanes; i++)
        memcpy(&d[i], hi + (i - 4) * 4, 4);
}

void vec_write(vfl_vcpu_t *v, int r, int lanes, uint32_t *s, int zero_hi) {
    uint8_t lo[16];
    uint8_t hi[16];
    int i;

    memcpy(lo, &v->fpu.xmm[r & 15], 16);
    if (!zero_hi) memcpy(hi, &v->fpu.ymm_hi[r & 15], 16);
    else memset(hi, 0, 16);
    for (i = 0; i < 4 && i < lanes; i++)
        memcpy(lo + i * 4, &s[i], 4);
    for (i = 4; i < lanes; i++)
        memcpy(hi + (i - 4) * 4, &s[i], 4);
    memcpy(&v->fpu.xmm[r & 15], lo, 16);
    memcpy(&v->fpu.ymm_hi[r & 15], hi, 16);
}

#define vfl_u32f(x) (__extension__ ({ float _v = 0; memcpy(&_v, &(x), 4); _v; }))
#define vfl_u64d(x) (__extension__ ({ double _v = 0; memcpy(&_v, &(x), 8); _v; }))
#define vfl_f32_add(a, b) ((a) + (b))
#define vfl_f32_sub(a, b) ((a) - (b))
#define vfl_f32_mul(a, b) ((a) * (b))
#define vfl_f32_div(a, b) ((a) / (b))
#define vfl_f64_add(a, b) ((a) + (b))
#define vfl_f64_sub(a, b) ((a) - (b))
#define vfl_f64_mul(a, b) ((a) * (b))
#define vfl_f64_div(a, b) ((a) / (b))
#define vfl_f32_sqrt(x) (__extension__ ({ float _v = (x); __asm__ volatile ("fsqrt" : "+t"(_v)); _v; }))
#define vfl_f64_sqrt(x) (__extension__ ({ double _v = (x); __asm__ volatile ("fsqrt" : "+t"(_v)); _v; }))


static uint32_t fu32(float f) {
    uint32_t x;

    memcpy(&x, &f, 4);
    return x;
}



static uint64_t du64(double f) {
    uint64_t x;

    memcpy(&x, &f, 8);
    return x;
}

static uint64_t f32_to_f64(uint32_t x) {
    uint32_t sign = x >> 31;
    uint32_t exp = (x >> 23) & 255;
    uint32_t mant = x & 0x7FFFFFu;
    uint64_t e;
    uint64_t m;

    if (exp == 0) {
        if (mant == 0) return (uint64_t)sign << 63;
        e = 1023 - 126;
        while (!(mant & 0x800000u)) {
            mant <<= 1;
            e--;
        }
        m = (uint64_t)(mant & 0x7FFFFFu) << 29;
        return ((uint64_t)sign << 63) | (e << 52) | m;
    }
    if (exp == 255) {
        e = 2047;
        if (mant == 0) m = 0;
        else m = ((uint64_t)mant << 29) | 0x8000000000000ULL;
        return ((uint64_t)sign << 63) | (e << 52) | m;
    }
    e = (uint64_t)exp - 127 + 1023;
    m = (uint64_t)mant << 29;
    return ((uint64_t)sign << 63) | (e << 52) | m;
}

static int64_t f32_rint_i64(uint32_t x, int *ov) {
    uint32_t exp = (x >> 23) & 255;
    uint32_t mant = x & 0x7FFFFFu;
    int neg = (x >> 31) & 1;
    int64_t e2;
    uint64_t m;
    uint64_t base;
    uint64_t rest;
    uint64_t halfbit;
    int shift;

    *ov = 0;
    if (exp == 255 || exp == 0) {
        if (exp == 0) return 0;
        *ov = 1;
        return 0;
    }
    e2 = (int64_t)exp - 127;
    m = (1ULL << 23) | mant;
    if (e2 < 0) {
        shift = (int)(23 - e2);
        if (shift >= 64) return 0;
        base = m >> shift;
        rest = m & ((1ULL << shift) - 1);
        halfbit = 1ULL << (shift - 1);
        if (rest > halfbit || (rest == halfbit && (base & 1))) base++;
        if (neg) return -(int64_t)base;
        return (int64_t)base;
    }
    if (e2 >= 63) {
        *ov = 1;
        return 0;
    }
    if (e2 >= 23) {
        uint64_t r = m << (e2 - 23);

        if (!neg) {
            if (r >> 63) {
                *ov = 1;
                return 0;
            }
            return (int64_t)r;
        }
        if (r > 0x8000000000000000ULL) {
            *ov = 1;
            return 0;
        }
        if (r == 0x8000000000000000ULL) return (int64_t)0x8000000000000000ULL;
        return -(int64_t)r;
    }
    shift = (int)(23 - e2);
    base = m >> shift;
    rest = m & ((1ULL << shift) - 1);
    halfbit = 1ULL << (shift - 1);
    if (rest > halfbit || (rest == halfbit && (base & 1))) base++;
    if (!neg) return (int64_t)base;
    return -(int64_t)base;
}

static uint32_t f64_to_f32(uint64_t x) {    uint32_t sign = (uint32_t)(x >> 63);
    uint32_t exp = (uint32_t)((x >> 52) & 2047);
    uint64_t mant = x & 0xFFFFFFFFFFFFFULL;
    uint64_t full;
    int64_t e2;

    if (exp == 0) return sign << 31;
    if (exp == 2047) {
        if (mant == 0) return (sign << 31) | 0x7F800000u;
        return (sign << 31) | 0x7FC00000u | (uint32_t)((mant >> 29) & 0x3FFFFFu);
    }
    e2 = (int64_t)exp - 1023 + 127;
    full = (1ULL << 52) | mant;
    if (e2 >= 255) return (sign << 31) | 0x7F800000u;
    if (e2 <= 0) {
        uint64_t shift = (uint64_t)(1 - e2) + 29;
        uint64_t base;
        uint64_t rest;
        uint64_t halfbit;

        if (shift >= 64) return sign << 31;
        base = full >> shift;
        rest = full & ((1ULL << shift) - 1);
        halfbit = 1ULL << (shift - 1);
        if (rest > halfbit || (rest == halfbit && (base & 1))) base++;
        if (base >= 0x800000ULL) return (sign << 31) | (1u << 23);
        return (sign << 31) | (uint32_t)base;
    }
    {
        uint64_t low = full & 0x1FFFFFFFULL;
        uint32_t m = (uint32_t)(full >> 29);

        if (low > 0x10000000ULL || (low == 0x10000000ULL && (m & 1))) {
            m++;
            if (m & 0x1000000u) {
                m = 0;
                e2++;
                if (e2 >= 255) return (sign << 31) | 0x7F800000u;
            }
        }
        return (sign << 31) | ((uint32_t)e2 << 23) | (m & 0x7FFFFFu);
    }
}



static uint32_t f32_cmp(float a, float b, int pred) {
    int eq = a == b;
    int lt = a < b;
    int un = a != a || b != b;

    switch (pred) {
        case 0: return eq && !un ? 0xFFFFFFFFu : 0;
        case 1: return lt && !un ? 0xFFFFFFFFu : 0;
        case 2: return (lt || eq) && !un ? 0xFFFFFFFFu : 0;
        case 3: return un ? 0xFFFFFFFFu : 0;
        case 4: return (!eq || un) ? 0xFFFFFFFFu : 0;
        case 5: return (!lt || un) ? 0xFFFFFFFFu : 0;
        case 6: return ((!lt && !eq) || un) ? 0xFFFFFFFFu : 0;
        default: return !un ? 0xFFFFFFFFu : 0;
    }
}

static uint32_t f32_min(float a, float b) {
    uint32_t abits;
    uint32_t bbits;

    memcpy(&abits, &a, 4);
    memcpy(&bbits, &b, 4);
    if ((abits & 0x7FFFFFFFu) == 0 && (bbits & 0x7FFFFFFFu) == 0)
        return abits & bbits & 0x80000000u ? 0x80000000u : 0;
    if (a != a) return bbits;
    if (b != b) return bbits;
    return a < b ? abits : bbits;
}

static uint32_t f32_max(float a, float b) {
    uint32_t abits;
    uint32_t bbits;

    memcpy(&abits, &a, 4);
    memcpy(&bbits, &b, 4);
    if ((abits & 0x7FFFFFFFu) == 0 && (bbits & 0x7FFFFFFFu) == 0)
        return abits & bbits & 0x80000000u ? 0 : 0x80000000u;
    if (a != a) return bbits;
    if (b != b) return bbits;
    return a > b ? abits : bbits;
}

static uint64_t f64_min(double a, double b) {
    uint64_t abits;
    uint64_t bbits;

    memcpy(&abits, &a, 8);
    memcpy(&bbits, &b, 8);
    if ((abits & 0x7FFFFFFFFFFFFFFFULL) == 0 && (bbits & 0x7FFFFFFFFFFFFFFFULL) == 0)
        return abits & bbits & 0x8000000000000000ULL ? 0x8000000000000000ULL : 0;
    if (a != a) return bbits;
    if (b != b) return bbits;
    return a < b ? abits : bbits;
}

static uint64_t f64_max(double a, double b) {
    uint64_t abits;
    uint64_t bbits;

    memcpy(&abits, &a, 8);
    memcpy(&bbits, &b, 8);
    if ((abits & 0x7FFFFFFFFFFFFFFFULL) == 0 && (bbits & 0x7FFFFFFFFFFFFFFFULL) == 0)
        return abits & bbits & 0x8000000000000000ULL ? 0 : 0x8000000000000000ULL;
    if (a != a) return bbits;
    if (b != b) return bbits;
    return a > b ? abits : bbits;
}

static int sse_load_vec(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                        int lanes, uint32_t *d) {
    int bytes = lanes * 4;
    uint8_t tmp[32];
    int rr;
    int i;

    if (ea->is_reg) {
        vec_read(v, ea->reg, lanes, d);
        return 0;
    }
    rr = vfl_mem_read(vm, v, ea->addr, (uint64_t)bytes, tmp);
    if (rr == -2) {
        vfl_mem_fault(v, run);
        return -1;
    }
    if (rr == 1) return vfl_mmio_fill_exit(vm, v, run, ea->addr, bytes);
    if (rr < 0) {
        vfl_exit_exception(v, run, 13, 0);
        return -1;
    }
    for (i = 0; i < lanes; i++)
        memcpy(&d[i], tmp + i * 4, 4);
    return 0;
}

static int sse_mem_fill(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint64_t addr,
                        uint8_t *tmp, int bytes) {
    int rr = vfl_mem_read(vm, v, addr, (uint64_t)bytes, tmp);

    if (rr == -2) {
        vfl_mem_fault(v, run);
        return 1;
    }
    if (rr == 1) return vfl_mmio_fill_exit(vm, v, run, addr, bytes);
    if (rr < 0) {
        vfl_exit_exception(v, run, 13, 0);
        return 1;
    }
    return 0;
}

static int sse_store_vec(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                         int lanes, uint32_t *s, int n, uint64_t ripmask, int align) {
    uint8_t tmp[32];
    int bytes = lanes * 4;
    int i;
    int rr;

    if (ea->is_reg) {
        vec_write(v, ea->reg, lanes, s, 0);
        vfl_adv(v, n, ripmask);
        return 1;
    }
    if (align && (ea->addr & (uint64_t)(align - 1)))
        return vfl_exit_exception(v, run, 13, 0);
    for (i = 0; i < lanes; i++)
        memcpy(tmp + i * 4, &s[i], 4);
    rr = vfl_mem_write(vm, v, ea->addr, (uint64_t)bytes, tmp);
    if (rr == -2) return vfl_mem_fault(v, run);
    if (rr == 1) {
        uint64_t phys;

        if (bytes != 16 && bytes != 32)
            return vfl_exit_exception(v, run, 14, 0);
        if (vfl_translate(vm, v, ea->addr, VFL_MMU_WRITE, &phys))
            return vfl_mem_fault(v, run);
        if (bytes == 32) {
            uint64_t phys2;

            if (vfl_translate(vm, v, ea->addr + 16, VFL_MMU_WRITE, &phys2))
                return vfl_mem_fault(v, run);
            if (phys2 != phys + 16)
                return vfl_exit_exception(v, run, 14, 0);
        }
        return vfl_exit_mmio_write64(v, run, phys, tmp, bytes,
                                     VFL_P_NONE, 0, n, ripmask);
    }
    if (rr < 0) return vfl_exit_exception(v, run, 13, 0);
    vfl_adv(v, n, ripmask);
    return 1;
}

static int sse_move_load(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                         int reg, int lanes, int n, uint64_t ripmask, int align,
                         int zero_hi) {
    uint32_t d[8];
    uint8_t tmp[32];
    int bytes = lanes * 4;
    int rr;
    int i;

    if (ea->is_reg) {
        vec_read(v, ea->reg, lanes, d);
        vec_write(v, reg, lanes, d, zero_hi);
        vfl_adv(v, n, ripmask);
        return 0;
    }
    if (align && (ea->addr & (uint64_t)(align - 1)))
        return vfl_exit_exception(v, run, 13, 0);
    rr = vfl_mem_read(vm, v, ea->addr, (uint64_t)bytes, tmp);
    if (rr == -2) return vfl_mem_fault(v, run);
    if (rr == 1) {
        uint64_t phys;

        if (bytes != 16 && bytes != 32)
            return vfl_exit_exception(v, run, 14, 0);
        if (vfl_translate(vm, v, ea->addr, VFL_MMU_READ, &phys))
            return vfl_mem_fault(v, run);
        if (bytes == 32) {
            uint64_t phys2;

            if (vfl_translate(vm, v, ea->addr + 16, VFL_MMU_READ, &phys2))
                return vfl_mem_fault(v, run);
            if (phys2 != phys + 16)
                return vfl_exit_exception(v, run, 14, 0);
        }
        return vfl_exit_mmio_read(v, run, phys, bytes, VFL_P_MMIO_XMM,
                                  reg, zero_hi, 0, 0, n, ripmask);
    }
    if (rr < 0) return vfl_exit_exception(v, run, 13, 0);
    for (i = 0; i < lanes; i++)
        memcpy(&d[i], tmp + i * 4, 4);
    vec_write(v, reg, lanes, d, zero_hi);
    vfl_adv(v, n, ripmask);
    return 0;
}

static int sse_mov_load(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                        int reg, int lanes, int n, uint64_t ripmask, int align) {
    return sse_move_load(vm, v, run, ea, reg, lanes, n, ripmask, align, 0);
}

static int sse_mov_store(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                         int reg, int lanes, int n, uint64_t ripmask, int align) {
    uint32_t s[8];

    vec_read(v, reg, lanes, s);
    return sse_store_vec(vm, v, run, ea, lanes, s, n, ripmask, align);
}

static int sse_bitop(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                     int reg, int lanes, int which, int n, uint64_t ripmask) {
    uint32_t a[8];
    uint32_t b[8];
    int i;
    int rr;

    vec_read(v, reg, lanes, a);
    if (ea->is_reg) {
        vec_read(v, ea->reg, lanes, b);
    } else {
        rr = sse_load_vec(vm, v, run, ea, lanes, b);
        if (rr) return 1;
    }
    for (i = 0; i < lanes; i++) {
        if (which == 0) a[i] ^= b[i];
        else if (which == 1) a[i] &= b[i];
        else if (which == 2) a[i] |= b[i];
        else a[i] = (~a[i]) & b[i];
    }
    vec_write(v, reg, lanes, a, 0);
    vfl_adv(v, n, ripmask);
    return 0;
}

static int sse_arith_f32(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                         int reg, int lanes, int which, int n, uint64_t ripmask) {
    uint32_t a[8];
    uint32_t b[8];
    int i;
    int rr;

    vec_read(v, reg, lanes, a);
    if (ea->is_reg) {
        vec_read(v, ea->reg, lanes, b);
    } else {
        rr = sse_load_vec(vm, v, run, ea, lanes, b);
        if (rr) return 1;
    }
    for (i = 0; i < lanes; i++) {
        float x = vfl_u32f(a[i]);
        float y = vfl_u32f(b[i]);

        if (which == 0) a[i] = fu32(vfl_f32_add(x, y));
        else if (which == 1) a[i] = fu32(vfl_f32_sub(x, y));
        else if (which == 2) a[i] = fu32(vfl_f32_mul(x, y));
        else if (which == 3) a[i] = fu32(vfl_f32_div(x, y));
        else if (which == 4) a[i] = fu32(vfl_f32_sqrt(x));
        else if (which == 5) a[i] = f32_min(x, y);
        else if (which == 6) a[i] = f32_max(x, y);
        else if (which == 7) a[i] = fu32(1.0f / y);
        else a[i] = fu32(1.0f / vfl_f32_sqrt(y));
    }
    vec_write(v, reg, lanes, a, 0);
    vfl_adv(v, n, ripmask);
    return 0;
}

static int sse_arith_f64(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                         int reg, int lanes, int which, int n, uint64_t ripmask) {
    uint64_t a[4];
    uint64_t b[4];
    uint32_t t[8];
    int i;
    int rr;
    int q = lanes / 2;

    vec_read(v, reg, lanes, t);
    for (i = 0; i < q; i++)
        a[i] = (uint64_t)t[i * 2] | ((uint64_t)t[i * 2 + 1] << 32);
    if (ea->is_reg) {
        vec_read(v, ea->reg, lanes, t);
        for (i = 0; i < q; i++)
            b[i] = (uint64_t)t[i * 2] | ((uint64_t)t[i * 2 + 1] << 32);
    } else {
        rr = sse_load_vec(vm, v, run, ea, lanes, t);
        if (rr) return 1;
        for (i = 0; i < q; i++)
            b[i] = (uint64_t)t[i * 2] | ((uint64_t)t[i * 2 + 1] << 32);
    }
    for (i = 0; i < q; i++) {
        double x = vfl_u64d(a[i]);
        double y = vfl_u64d(b[i]);
        uint64_t r;

        if (which == 0) r = du64(vfl_f64_add(x, y));
        else if (which == 1) r = du64(vfl_f64_sub(x, y));
        else if (which == 2) r = du64(vfl_f64_mul(x, y));
        else if (which == 3) r = du64(vfl_f64_div(x, y));
        else if (which == 4) r = du64(vfl_f64_sqrt(x));
        else if (which == 5) r = f64_min(x, y);
        else r = f64_max(x, y);
        t[i * 2] = (uint32_t)r;
        t[i * 2 + 1] = (uint32_t)(r >> 32);
    }
    vec_write(v, reg, lanes, t, 0);
    vfl_adv(v, n, ripmask);
    return 0;
}

static int sse_cmp_f32(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                       int reg, int lanes, int pred, int n, uint64_t ripmask) {
    uint32_t a[8];
    uint32_t b[8];
    int i;
    int rr;

    vec_read(v, reg, lanes, a);
    if (ea->is_reg) {
        vec_read(v, ea->reg, lanes, b);
    } else {
        rr = sse_load_vec(vm, v, run, ea, lanes, b);
        if (rr) return 1;
    }
    for (i = 0; i < lanes; i++)
        a[i] = f32_cmp(vfl_u32f(a[i]), vfl_u32f(b[i]), pred);
    vec_write(v, reg, lanes, a, 0);
    vfl_adv(v, n, ripmask);
    return 0;
}

static int sse_intop(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                     int reg, int lanes, int el, int which, int n, uint64_t ripmask) {
    uint32_t a[8];
    uint32_t b[8];
    int rr;

    vec_read(v, reg, lanes, a);
    if (ea->is_reg) {
        vec_read(v, ea->reg, lanes, b);
    } else {
        rr = sse_load_vec(vm, v, run, ea, lanes, b);
        if (rr) return 1;
    }
    if (el == 0) {
        uint8_t *pa = (uint8_t *)a;
        uint8_t *pb = (uint8_t *)b;
        int nb = lanes * 4;
        int i;

        for (i = 0; i < nb; i++) {
            if (which == 0) pa[i] += pb[i];
            else if (which == 1) pa[i] -= pb[i];
            else if (which == 2) pa[i] = pa[i] == pb[i] ? 0xFF : 0;
            else if (which == 3) pa[i] = (int8_t)pa[i] > (int8_t)pb[i] ? 0xFF : 0;
            else if (which == 4) pa[i] = pa[i] > pb[i] ? 0xFF : 0;
            else if (which == 5) pa[i] = pa[i] < pb[i] ? 0xFF : 0;
            else if (which == 6) pa[i] = (int8_t)pa[i] < (int8_t)pb[i] ? 0xFF : 0;
            else if (which == 7) pa[i] = pa[i] < pb[i] ? pa[i] : pb[i];
            else if (which == 8) pa[i] = pa[i] > pb[i] ? pa[i] : pb[i];
            else if (which == 9) pa[i] = (int8_t)pa[i] < (int8_t)pb[i] ? pa[i] : pb[i];
            else if (which == 10) pa[i] = (int8_t)pa[i] > (int8_t)pb[i] ? pa[i] : pb[i];
        }
    } else if (el == 1) {
        uint16_t *pa = (uint16_t *)a;
        uint16_t *pb = (uint16_t *)b;
        int nw = lanes * 2;
        int i;

        for (i = 0; i < nw; i++) {
            if (which == 0) pa[i] += pb[i];
            else if (which == 1) pa[i] -= pb[i];
            else if (which == 2) pa[i] = pa[i] == pb[i] ? 0xFFFF : 0;
            else if (which == 3) pa[i] = (int16_t)pa[i] > (int16_t)pb[i] ? 0xFFFF : 0;
            else if (which == 4) pa[i] = pa[i] > pb[i] ? 0xFFFF : 0;
            else if (which == 5) pa[i] = pa[i] < pb[i] ? 0xFFFF : 0;
            else if (which == 6) pa[i] = (int16_t)pa[i] < (int16_t)pb[i] ? 0xFFFF : 0;
            else if (which == 7) pa[i] = pa[i] < pb[i] ? pa[i] : pb[i];
            else if (which == 8) pa[i] = pa[i] > pb[i] ? pa[i] : pb[i];
            else if (which == 9) pa[i] = (int16_t)pa[i] < (int16_t)pb[i] ? pa[i] : pb[i];
            else if (which == 10) pa[i] = (int16_t)pa[i] > (int16_t)pb[i] ? pa[i] : pb[i];
        }
    } else if (el == 2) {
        int i;

        for (i = 0; i < lanes; i++) {
            if (which == 0) a[i] += b[i];
            else if (which == 1) a[i] -= b[i];
            else if (which == 2) a[i] = a[i] == b[i] ? 0xFFFFFFFFu : 0;
            else if (which == 3) a[i] = (int32_t)a[i] > (int32_t)b[i] ? 0xFFFFFFFFu : 0;
            else if (which == 4) a[i] = a[i] > b[i] ? 0xFFFFFFFFu : 0;
            else if (which == 5) a[i] = a[i] < b[i] ? 0xFFFFFFFFu : 0;
            else if (which == 6) a[i] = (int32_t)a[i] < (int32_t)b[i] ? 0xFFFFFFFFu : 0;
            else if (which == 7) a[i] = a[i] < b[i] ? a[i] : b[i];
            else if (which == 8) a[i] = a[i] > b[i] ? a[i] : b[i];
            else if (which == 9) a[i] = (int32_t)a[i] < (int32_t)b[i] ? a[i] : b[i];
            else if (which == 10) a[i] = (int32_t)a[i] > (int32_t)b[i] ? a[i] : b[i];
            else if (which == 11) {
                uint64_t p = (uint64_t)a[i] * b[i];

                a[i] = (uint32_t)(p >> 32);
            } else if (which == 12) {
                int64_t p = (int64_t)(int32_t)a[i] * (int64_t)(int32_t)b[i];

                a[i] = (uint32_t)(p >> 32);
            }
        }
    } else {
        int q = lanes / 2;
        int i;

        for (i = 0; i < q; i++) {
            uint64_t x = (uint64_t)a[i * 2] | ((uint64_t)a[i * 2 + 1] << 32);
            uint64_t y = (uint64_t)b[i * 2] | ((uint64_t)b[i * 2 + 1] << 32);
            uint64_t r;

            if (which == 0) r = x + y;
            else if (which == 1) r = x - y;
            else if (which == 2) r = x == y ? ~0ULL : 0;
            else if (which == 3) r = (int64_t)x > (int64_t)y ? ~0ULL : 0;
            else r = x;
            a[i * 2] = (uint32_t)r;
            a[i * 2 + 1] = (uint32_t)(r >> 32);
        }
    }
    vec_write(v, reg, lanes, a, 0);
    vfl_adv(v, n, ripmask);
    return 0;
}

static int sse_shift_cnt(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                         int reg, int lanes, int el, int which, int n, uint64_t ripmask) {
    uint32_t d[8];
    uint64_t cnt = 0;
    int rr;

    vec_read(v, reg, lanes, d);
    if (ea->is_reg) {
        uint32_t t[8];

        vec_read(v, ea->reg, lanes, t);
        cnt = (uint64_t)t[0] | ((uint64_t)t[1] << 32);
    } else {
        uint8_t tmp[8];

        rr = vfl_mem_read(vm, v, ea->addr, 8, tmp);
        if (rr == -2) {
            vfl_mem_fault(v, run);
            return 1;
        }
        if (rr == 1) return vfl_mmio_fill_exit(vm, v, run, ea->addr, 8);
        if (rr < 0) {
            vfl_exit_exception(v, run, 13, 0);
            return 1;
        }
        memcpy(&cnt, tmp, 8);
    }
    if (el == 0) {
        uint16_t *pa = (uint16_t *)d;
        int k;

        if (cnt > 15) cnt = 16;
        for (k = 0; k < lanes * 2; k++) {
            if (which == 1) pa[k] >>= cnt;
            else pa[k] = (uint16_t)((int16_t)pa[k] >> cnt);
        }
    } else if (el == 1) {
        int k;

        if (cnt > 31) cnt = 32;
        for (k = 0; k < lanes; k++) {
            if (which == 1) d[k] >>= cnt;
            else d[k] = (uint32_t)((int32_t)d[k] >> cnt);
        }
    } else {
        int q = lanes / 2;
        int k;

        if (cnt > 63) cnt = 64;
        for (k = 0; k < q; k++) {
            uint64_t x = (uint64_t)d[k * 2] | ((uint64_t)d[k * 2 + 1] << 32);
            uint64_t r;

            if (which == 1) r = cnt >= 64 ? 0 : x >> cnt;
            else r = cnt >= 64 ? ((int64_t)x < 0 ? ~0ULL : 0) : (uint64_t)((int64_t)x >> cnt);
            d[k * 2] = (uint32_t)r;
            d[k * 2 + 1] = (uint32_t)(r >> 32);
        }
    }
    vec_write(v, reg, lanes, d, 0);
    vfl_adv(v, n, ripmask);
    return 0;
}

static int sse_cvt_si2f(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                        int reg, int rex, int todouble, int n, uint64_t ripmask) {
    uint32_t d[8];
    int64_t sv = 0;
    int rr;

    vec_read(v, reg, 4, d);
    if (ea->is_reg) {
        sv = (int64_t)vfl_read_reg(v, ea->reg, rex, rex ? 8 : 4);
        if (!rex) sv = (int32_t)(sv & 0xFFFFFFFFULL);
    } else {
        uint8_t tmp[8];

        rr = vfl_mem_read(vm, v, ea->addr, rex ? 8 : 4, tmp);
        if (rr == -2) {
            vfl_mem_fault(v, run);
            return 1;
        }
        if (rr == 1) return vfl_mmio_fill_exit(vm, v, run, ea->addr, rex ? 8 : 4);
        if (rr < 0) {
            vfl_exit_exception(v, run, 13, 0);
            return 1;
        }
        if (rex) {
            uint64_t u = 0;

            memcpy(&u, tmp, 8);
            sv = (int64_t)u;
        } else {
            uint32_t u = 0;

            memcpy(&u, tmp, 4);
            sv = (int32_t)u;
        }
    }
    if (todouble) {
        double f = (double)sv;
        uint64_t u;

        memcpy(&u, &f, 8);
        d[0] = (uint32_t)u;
        d[1] = (uint32_t)(u >> 32);
    } else {
        float f = (float)sv;

        memcpy(&d[0], &f, 4);
    }
    vec_write(v, reg, 4, d, 0);
    vfl_adv(v, n, ripmask);
    return 0;
}

static int sse_cvt_f2si(vfl_vcpu_t *v, int reg, int rex, int fromdouble,
                        int trunc, int n, uint64_t ripmask) {
    uint32_t d[8];
    int64_t r = 0;

    vec_read(v, reg, 4, d);
    if (fromdouble) {
        double x;

        memcpy(&x, d, 8);
        if (trunc) {
            if (!(x > -9.223372036854776e18) || !(x < 9.223372036854776e18)) {
                r = rex ? (int64_t)0x8000000000000000ULL : 0x80000000;
            } else {
                r = (int64_t)x;
            }
        } else {
            long double t = f80_round((long double)x);

            if (!(t > -9.223372036854776e18L) || !(t < 9.223372036854776e18L)) {
                r = rex ? (int64_t)0x8000000000000000ULL : 0x80000000;
            } else {
                r = (int64_t)t;
            }
        }
    } else {
        float x;

        memcpy(&x, d, 4);
        if (trunc) {
            if (!rex && (!(x > -2.1474836e9f) || !(x < 2.1474836e9f))) {
                r = 0x80000000;
            } else if (!(x > -9.223372036854776e18) || !(x < 9.223372036854776e18)) {
                r = rex ? (int64_t)0x8000000000000000ULL : 0x80000000;
            } else if (!rex) {
                r = (int32_t)x;
            } else {
                r = (int64_t)x;
            }
        } else {
            int ov = 0;
            int64_t q = f32_rint_i64(d[0], &ov);

            if (!rex && (ov || q != (int64_t)(int32_t)q)) {
                r = 0x80000000;
            } else if (ov) {
                r = (int64_t)0x8000000000000000ULL;
            } else {
                r = q;
            }
        }
    }
    if (rex) *vfl_reg64(v, reg) = (uint64_t)r;
    else vfl_write_reg(v, reg, 0, 4, (uint64_t)(uint32_t)r);
    vfl_adv(v, n, ripmask);
    return 0;
}

static int sse_shufps(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                      int reg, int lanes, int imm, int pd, int n, uint64_t ripmask) {
    uint32_t a[8];
    uint32_t b[8];
    uint32_t r[8];
    int rr;
    int i;
    int half = lanes / 2;

    vec_read(v, reg, lanes, a);
    if (ea->is_reg) {
        vec_read(v, ea->reg, lanes, b);
    } else {
        rr = sse_load_vec(vm, v, run, ea, lanes, b);
        if (rr) return 1;
    }
    if (!pd) {
        for (i = 0; i < half; i++)
            r[i] = a[(imm >> (i * 2)) & 3];
        for (i = 0; i < half; i++)
            r[half + i] = b[(imm >> (4 + i * 2)) & 3];
    } else {
        for (i = 0; i < half / 2; i++) {
            uint64_t x = (uint64_t)a[i * 2] | ((uint64_t)a[i * 2 + 1] << 32);
            uint64_t y = (uint64_t)b[i * 2] | ((uint64_t)b[i * 2 + 1] << 32);
            uint64_t o = ((imm >> (i * 4)) & 1) ? y : x;

            r[i * 2] = (uint32_t)o;
            r[i * 2 + 1] = (uint32_t)(o >> 32);
        }
        for (i = 0; i < half / 2; i++) {
            uint64_t x = (uint64_t)a[half + i * 2] | ((uint64_t)a[half + i * 2 + 1] << 32);
            uint64_t y = (uint64_t)b[half + i * 2] | ((uint64_t)b[half + i * 2 + 1] << 32);
            uint64_t o = ((imm >> (4 + i * 4)) & 1) ? y : x;

            r[half + i * 2] = (uint32_t)o;
            r[half + i * 2 + 1] = (uint32_t)(o >> 32);
        }
    }
    vec_write(v, reg, lanes, r, 0);
    vfl_adv(v, n, ripmask);
    return 0;
}

static int sse_movmsk(vfl_vcpu_t *v, int dst, int src, int rex, int el, int n, uint64_t ripmask) {
    uint32_t d[8];
    uint64_t mask = 0;
    int i;

    vec_read(v, src, 4, d);
    if (el == 0) {
        for (i = 0; i < 16; i++)
            if ((d[i >> 2] >> ((i & 3) * 8 + 7)) & 1) mask |= 1ULL << i;
    } else if (el == 1) {
        for (i = 0; i < 4; i++)
            if ((d[i] >> 31) & 1) mask |= 1ULL << i;
    } else {
        for (i = 0; i < 2; i++) {
            uint64_t q = (uint64_t)d[i * 2] | ((uint64_t)d[i * 2 + 1] << 32);

            if ((q >> 63) & 1) mask |= 1ULL << i;
        }
    }
    if (rex) *vfl_reg64(v, dst) = mask;
    else vfl_write_reg(v, dst, 0, 4, mask);
    vfl_adv(v, n, ripmask);
    return 0;
}

static int simd_syscall(vfl_vcpu_t *v, int n, uint64_t ripmask, int is64) {
    uint64_t star = v->msr_star;

    if (!(v->sregs.efer & 1ULL)) return -1;
    if (is64) {
        v->regs.rcx = (v->regs.rip + (uint64_t)n) & ripmask;
        v->regs.r11 = v->regs.rflags;
        v->sregs.cs.selector = (uint16_t)((star >> 32) & 0xFFFCULL);
        v->sregs.ss.selector = (uint16_t)(((star >> 32) + 8) & 0xFFFCULL);
        v->regs.rip = v->msr_lstar;
    } else {
        v->regs.rcx = (v->regs.rcx & ~0xFFFFFFFFULL) | ((v->regs.rip + (uint64_t)n) & 0xFFFFFFFFULL);
        v->regs.r11 = (v->regs.r11 & ~0xFFFFFFFFULL) | (v->regs.rflags & 0xFFFFFFFFULL);
        v->sregs.cs.selector = (uint16_t)((star >> 32) & 0xFFFCULL);
        v->sregs.ss.selector = (uint16_t)(((star >> 32) + 8) & 0xFFFCULL);
        v->regs.rip = (v->regs.rip & ~0xFFFFFFFFULL) | (star & 0xFFFFFFFFULL);
    }
    v->regs.rflags &= ~(v->msr_sfmask | 0x20000ULL);
    v->regs.rflags &= ~(1ULL << 17);
    return 0;
}

static int simd_sysret(vfl_vcpu_t *v, int n, uint64_t ripmask, int is64, vfl_run_t *run) {
    uint64_t star = v->msr_star;

    (void)n;
    (void)ripmask;

    (void)n;
    if (!(v->sregs.efer & 1ULL)) return -1;
    if (is64) {
        if ((v->regs.rcx >> 48) != 0 && (v->regs.rcx >> 48) != 0xFFFFULL)
            return vfl_exit_exception(v, run, 13, 0);
        v->sregs.cs.selector = (uint16_t)(((star >> 48) + 16) | 3);
        v->sregs.ss.selector = (uint16_t)(((star >> 48) + 8) | 3);
        v->regs.rip = v->regs.rcx;
        v->regs.rflags = (v->regs.rflags & 0x3C0000ULL) | (v->regs.r11 & ~0x3C0000ULL) | 0x2ULL;
    } else {
        v->sregs.cs.selector = (uint16_t)(((star >> 48)) | 3);
        v->sregs.ss.selector = (uint16_t)(((star >> 48) + 8) | 3);
        v->regs.rip = (v->regs.rip & ~0xFFFFFFFFULL) | (v->regs.rcx & 0xFFFFFFFFULL);
        v->regs.rflags = (v->regs.rflags & 0x3C0000ULL) | (v->regs.r11 & ~0x3C0000ULL) | 0x2ULL;
    }
    return 0;
}

static int simd_sysenter(vfl_vcpu_t *v, vfl_run_t *run) {
    uint64_t cs = v->msr_sysenter_cs & 0xFFFCULL;

    if (!(v->sregs.cr0 & 1ULL)) return vfl_exit_exception(v, run, 13, 0);
    if (cs == 0) return vfl_exit_exception(v, run, 13, 0);
    v->sregs.cs.selector = (uint16_t)cs;
    v->sregs.ss.selector = (uint16_t)(cs + 8);
    v->regs.rsp = (v->regs.rsp & ~vfl_stack_mask(v)) | (v->msr_sysenter_esp & vfl_stack_mask(v));
    v->regs.rip = (v->regs.rip & ~vfl_stack_mask(v)) | (v->msr_sysenter_eip & vfl_stack_mask(v));
    v->regs.rflags &= ~(1ULL << 9);
    return 0;
}

static int simd_sysexit(vfl_vcpu_t *v, vfl_run_t *run, int is64) {
    uint64_t cs = v->msr_sysenter_cs;

    if (!(v->sregs.cr0 & 1ULL)) return vfl_exit_exception(v, run, 13, 0);
    if (cs == 0) return vfl_exit_exception(v, run, 13, 0);
    if (is64) {
        v->sregs.cs.selector = (uint16_t)((cs + 16) | 3);
        v->sregs.ss.selector = (uint16_t)((cs + 24) | 3);
        v->regs.rip = v->regs.rdx;
        v->regs.rsp = v->regs.rcx;
    } else {
        v->sregs.cs.selector = (uint16_t)((cs + 16) | 3);
        v->sregs.ss.selector = (uint16_t)((cs + 24) | 3);
        v->regs.rip = (v->regs.rip & ~0xFFFFFFFFULL) | (v->regs.rdx & 0xFFFFFFFFULL);
        vfl_set_sp(v, v->regs.rcx & 0xFFFFFFFFULL);
    }
    return 0;
}

static int simd_fxsave(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint64_t addr,
                       int n, uint64_t ripmask, int save) {
    uint8_t area[512];
    int i;
    int rr;

    if (save) {
        memset(area, 0, sizeof(area));
        memcpy(area, &v->fpu.fcw, 2);
        memcpy(area + 2, &v->fpu.fsw, 2);
        {
            uint8_t ab = 0;

            for (i = 0; i < 8; i++)
                if (((v->fpu.ftw >> (i * 2)) & 3) != 3) ab |= (uint8_t)(1 << i);
            area[4] = ab;
        }
        memcpy(area + 6, &v->fpu.fop, 2);
        memcpy(area + 8, &v->fpu.fip, 4);
        memcpy(area + 12, &v->fpu.fcs, 2);
        memcpy(area + 16, &v->fpu.fdp, 4);
        memcpy(area + 20, &v->fpu.fds, 2);
        memcpy(area + 24, &v->fpu.mxcsr, 4);
        for (i = 0; i < 8; i++) {
            memcpy(area + 32 + i * 16, &v->fpu.fpr_m[i], 8);
            memcpy(area + 32 + i * 16 + 8, &v->fpu.fpr_e[i], 2);
        }
        for (i = 0; i < 16; i++)
            memcpy(area + 160 + i * 16, &v->fpu.xmm[i], 16);
        rr = vfl_mem_write(vm, v, addr, 512, area);
        if (rr) {
            vfl_memerr(v, run, rr);
            return 1;
        }
    } else {
        rr = vfl_mem_read(vm, v, addr, 512, area);
        if (rr) {
            vfl_memerr(v, run, rr);
            return 1;
        }
        memcpy(&v->fpu.fcw, area, 2);
        memcpy(&v->fpu.fsw, area + 2, 2);
        v->fpu.ftw = 0;
        for (i = 0; i < 8; i++)
            if (area[4] & (1 << i)) v->fpu.ftw |= (uint16_t)(0 << (i * 2));
            else v->fpu.ftw |= (uint16_t)(3 << (i * 2));
        memcpy(&v->fpu.fop, area + 6, 2);
        memcpy(&v->fpu.fip, area + 8, 4);
        memcpy(&v->fpu.fcs, area + 12, 2);
        memcpy(&v->fpu.fdp, area + 16, 4);
        memcpy(&v->fpu.fds, area + 20, 2);
        memcpy(&v->fpu.mxcsr, area + 24, 4);
        v->fpu.mxcsr &= 0xFFFFu;
        for (i = 0; i < 8; i++) {
            memcpy(&v->fpu.fpr_m[i], area + 32 + i * 16, 8);
            memcpy(&v->fpu.fpr_e[i], area + 32 + i * 16 + 8, 2);
            if (area[4] & (1 << i)) {
                uint64_t m = v->fpu.fpr_m[i];
                uint16_t e = v->fpu.fpr_e[i];

                if ((e & 0x7FFF) == 0 && m == 0) v->fpu.ftw |= (uint16_t)(1 << (i * 2));
            }
        }
        for (i = 0; i < 16; i++)
            memcpy(&v->fpu.xmm[i], area + 160 + i * 16, 16);
    }
    vfl_adv(v, n, ripmask);
    return 0;
}

static int simd_xsave(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint64_t addr,
                      int n, uint64_t ripmask, int save) {
    uint8_t hdr[64];
    int i;
    int rr;

    if (save) {
        uint8_t area[512];

        memset(area, 0, sizeof(area));
        memcpy(area, &v->fpu.fcw, 2);
        memcpy(area + 2, &v->fpu.fsw, 2);
        {
            uint8_t ab = 0;

            for (i = 0; i < 8; i++)
                if (((v->fpu.ftw >> (i * 2)) & 3) != 3) ab |= (uint8_t)(1 << i);
            area[4] = ab;
        }
        memcpy(area + 24, &v->fpu.mxcsr, 4);
        for (i = 0; i < 8; i++) {
            memcpy(area + 32 + i * 16, &v->fpu.fpr_m[i], 8);
            memcpy(area + 32 + i * 16 + 8, &v->fpu.fpr_e[i], 2);
        }
        for (i = 0; i < 16; i++)
            memcpy(area + 160 + i * 16, &v->fpu.xmm[i], 16);
        rr = vfl_mem_write(vm, v, addr, 512, area);
        if (rr) {
            vfl_memerr(v, run, rr);
            return 1;
        }
        memset(hdr, 0, sizeof(hdr));
        hdr[0] = 7;
        rr = vfl_mem_write(vm, v, addr + 512, 64, hdr);
        if (rr) {
            vfl_memerr(v, run, rr);
            return 1;
        }
        for (i = 0; i < 16; i++) {
            rr = vfl_mem_write(vm, v, addr + 576 + (uint64_t)i * 16,
                               16, (uint8_t *)&v->fpu.ymm_hi[i]);
            if (rr) {
                vfl_memerr(v, run, rr);
                return 1;
            }
        }
    } else {
        uint8_t area[512];

        rr = vfl_mem_read(vm, v, addr, 512, area);
        if (rr) {
            vfl_memerr(v, run, rr);
            return 1;
        }
        rr = vfl_mem_read(vm, v, addr + 512, 64, hdr);
        if (rr) {
            vfl_memerr(v, run, rr);
            return 1;
        }
        if (!(hdr[0] & 3)) return vfl_exit_exception(v, run, 13, 0);
        memcpy(&v->fpu.fcw, area, 2);
        memcpy(&v->fpu.fsw, area + 2, 2);
        v->fpu.ftw = 0;
        for (i = 0; i < 8; i++)
            if (area[4] & (1 << i)) v->fpu.ftw |= (uint16_t)(0 << (i * 2));
            else v->fpu.ftw |= (uint16_t)(3 << (i * 2));
        memcpy(&v->fpu.mxcsr, area + 24, 4);
        v->fpu.mxcsr &= 0xFFFFu;
        for (i = 0; i < 8; i++) {
            memcpy(&v->fpu.fpr_m[i], area + 32 + i * 16, 8);
            memcpy(&v->fpu.fpr_e[i], area + 32 + i * 16 + 8, 2);
        }
        for (i = 0; i < 16; i++)
            memcpy(&v->fpu.xmm[i], area + 160 + i * 16, 16);
        if (hdr[0] & 4) {
            for (i = 0; i < 16; i++) {
                rr = vfl_mem_read(vm, v, addr + 576 + (uint64_t)i * 16,
                                  16, (uint8_t *)&v->fpu.ymm_hi[i]);
                if (rr) {
                    vfl_memerr(v, run, rr);
                    return 1;
                }
            }
        }
    }
    vfl_adv(v, n, ripmask);
    return 0;
}

static int escape_modrm(vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf, int *pos,
                        int rex_r, int rex_b, int *mod, int *reg, int *rm) {
    uint8_t m;

    (void)v;
    (void)run;
    if (*pos >= 15) return -1;
    m = buf[*pos];
    *pos += 1;
    *mod = (m >> 6) & 3;
    *reg = ((m >> 3) & 7) | (rex_r << 3);
    *rm = (m & 7) | (rex_b << 3);
    return 0;
}

static int escape_ea(vfl_vcpu_t *v, uint8_t *buf, int *pos, int mod,
                     int rm, int asz, int seg, int rex_b, int rex_x, vfl_ea_t *ea,
                     uint64_t rip, uint64_t ripmask, int tailsz) {
    int n;

    if (vfl_decode_ea(v, buf, pos, mod, rm, asz, seg, rex_b, rex_x, ea) < 0)
        return -1;
    n = *pos + tailsz;
    if (!ea->is_reg && ea->rip_rel)
        ea->addr = (rip + (uint64_t)n + (uint64_t)(int64_t)ea->disp) & ripmask;
    return n;
}

static int vfl_simd_media(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                          int *pos, uint8_t op2, int pp, int osz, int asz, int seg,
                          int rep, int rex, int rex_w, int rex_r, int rex_b,
                          int rex_x, int lm, uint64_t ripmask);

static int media_mov(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                     int *pos, uint8_t op2, int pp, int asz, int seg,
                     int rex, int rex_w, int rex_r, int rex_b, int rex_x,
                     int lm, uint64_t ripmask);

static int media_arith(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                       int *pos, uint8_t op2, int pp, int asz, int seg,
                       int rex, int rex_r, int rex_b, int rex_x,
                       int lm, uint64_t ripmask);

static int media_int(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                     int *pos, uint8_t op2, int pp, int asz, int seg,
                     int rex, int rex_r, int rex_b, int rex_x,
                     int lm, uint64_t ripmask);

static int media_conv(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                      int *pos, uint8_t op2, int pp, int asz, int seg,
                      int rex, int rex_r, int rex_b, int rex_x,
                      int lm, uint64_t ripmask);

static int vfl_vex_more(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                        int *pos, int op, int pp, int W, int L, int lanes, int s1,
                        int asz, int seg, int reg, int rm, int mod,
                        vfl_ea_t *ea, int n, uint64_t ripmask);

static int vfl_vex_more2(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                         int *pos, int op, int pp, int W, int L, int lanes, int s1,
                         int reg, vfl_ea_t *ea, int n, uint64_t ripmask);

static int vfl_vex_more3(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                         int *pos, int op, int pp, int W, int L, int lanes, int s1,
                         int reg, vfl_ea_t *ea, int n, uint64_t ripmask);

static int vfl_vex38_3a(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                        int *pos, int mmmmm, uint8_t op3, int vvvv, int L, int pp,
                        int W, int asz, int seg, int rxb, int rxr, int rxx,
                        uint64_t ripmask);

static int simd_38(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                   int *pos, int pp, int asz, int seg, int rex, int rex_r,
                   int rex_b, int rex_x, int lm, uint64_t ripmask);

static int simd_3a(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                   int *pos, int pp, int asz, int seg, int rex, int rex_r,
                   int rex_b, int rex_x, int lm, uint64_t ripmask);

int vfl_simd_escape(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf, int *pos,
                    int osz, int asz, int seg, int rep, int p66, int rex, int rex_w,
                    int rex_r, int rex_b, int rex_x, int lm, uint64_t ripmask) {
    uint8_t op2;
    int pp;
    int mod;
    int reg;
    int rm;
    vfl_ea_t ea;
    int n;
    int rr;

    (void)rex;
    (void)rex_w;
    if (*pos >= 15) return vfl_exit_internal(v, run);
    op2 = buf[*pos];
    pp = p66 ? 1 : (rep == 3 ? 3 : (rep == 2 ? 2 : 0));
    switch (op2) {
        case 0x05:
            *pos += 1;
            n = *pos;
            if (simd_syscall(v, n, ripmask, lm) < 0)
                return vfl_exit_exception(v, run, 6, 0);
            return 0;

        case 0x07:
            *pos += 1;
            n = *pos;
            if (simd_sysret(v, n, ripmask, lm, run) < 0)
                return vfl_exit_exception(v, run, 6, 0);
            return 0;

        case 0x34:
            *pos += 1;
            n = *pos;
            rr = simd_sysenter(v, run);
            if (rr) return rr;
            return 0;

        case 0x35:
            *pos += 1;
            n = *pos;
            rr = simd_sysexit(v, run, lm && rex_w);
            if (rr) return rr;
            return 0;

        case 0x0D:
        case 0x0E:
        case 0x1F:
            *pos += 1;
            if (escape_modrm(v, run, buf, pos, rex_r, rex_b, &mod, &reg, &rm) < 0)
                return vfl_exit_internal(v, run);
            if (mod != 3) {
                if (vfl_decode_ea(v, buf, pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                    return vfl_exit_internal(v, run);
            }
            n = *pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0x01: {
            int save = *pos;

            *pos += 1;
            if (escape_modrm(v, run, buf, pos, rex_r, rex_b, &mod, &reg, &rm) < 0)
                return vfl_exit_internal(v, run);
            n = *pos;
            if (mod != 3) {
                *pos = save;
                return VFL_SIMD_NOTME;
            }
            if (reg == 7 && rm == 0) {
                uint64_t t;

                t = v->msr_gs_base;
                v->msr_gs_base = v->msr_kernel_gs_base;
                v->msr_kernel_gs_base = t;
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (reg == 7 && rm == 1) {
                v->tsc += 1000;
                v->regs.rax = (v->regs.rax & ~0xFFFFFFFFULL) |
                              ((v->tsc + v->tsc_offset) & 0xFFFFFFFFULL);
                v->regs.rdx = (v->regs.rdx & ~0xFFFFFFFFULL) |
                              (((v->tsc + v->tsc_offset) >> 32) & 0xFFFFFFFFULL);
                v->regs.rcx = (v->regs.rcx & ~0xFFFFFFFFULL) | (v->msr_tsc_aux & 0xFFFFFFFFULL);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (reg == 0 && rm == 0) {
                uint64_t lo = vfl_read_reg(v, 0, rex, 4);

                if (lo != 0) return vfl_exit_exception(v, run, 13, 0);
                v->regs.rax = (v->regs.rax & ~0xFFFFFFFFULL) | 7;
                v->regs.rdx = 0;
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (reg == 4 && (rm == 0 || rm == 1)) {
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (reg == 4 && rm == 2) {
                v->regs.rflags &= ~(1ULL << 18);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (reg == 4 && rm == 3) {
                v->regs.rflags |= (1ULL << 18);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            *pos = save;
            return VFL_SIMD_NOTME;
        }

        case 0x18:
            *pos += 1;
            if (escape_modrm(v, run, buf, pos, rex_r, rex_b, &mod, &reg, &rm) < 0)
                return vfl_exit_internal(v, run);
            if (vfl_decode_ea(v, buf, pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                return vfl_exit_internal(v, run);
            n = *pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0xC7: {
            int save = *pos;

            *pos += 1;
            if (escape_modrm(v, run, buf, pos, rex_r, rex_b, &mod, &reg, &rm) < 0)
                return vfl_exit_internal(v, run);
            if (reg == 6 || reg == 7) {
                uint64_t val = 0;

                if (mod != 3) return vfl_exit_exception(v, run, 6, 0);
                n = *pos;
                if (!vfl_host_rdrand()) return vfl_exit_exception(v, run, 6, 0);
                if (!host_rdrand64(&val)) {
                    v->regs.rflags &= ~((1ULL << 0) | (1ULL << 2) | (1ULL << 4) |
                                        (1ULL << 6) | (1ULL << 7) | (1ULL << 11));
                } else {
                    v->regs.rflags &= ~((1ULL << 2) | (1ULL << 4) | (1ULL << 6) |
                                        (1ULL << 7) | (1ULL << 11));
                    v->regs.rflags |= (1ULL << 0);
                }
                vfl_write_reg(v, rm, rex, osz, val);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (reg != 1) {
                *pos = save;
                return VFL_SIMD_NOTME;
            }
            n = escape_ea(v, buf, pos, mod, rm, asz, seg, rex_b, rex_x, &ea,
                          v->regs.rip, ripmask, 0);
            if (n < 0) return vfl_exit_internal(v, run);
            if (ea.is_reg) return vfl_exit_exception(v, run, 6, 0);
            if (rex_w) {
                uint64_t lo = vfl_read_reg(v, 0, rex, 8);
                uint64_t hi = vfl_read_reg(v, 2, rex, 8);
                uint64_t mlo = 0;
                uint64_t mhi = 0;
                uint8_t tb[16];

                rr = vfl_mem_read(vm, v, ea.addr, 16, tb);
                if (rr) {
                    vfl_memerr(v, run, rr);
                    return 1;
                }
                memcpy(&mlo, tb, 8);
                memcpy(&mhi, tb + 8, 8);
                if (mlo == lo && mhi == hi) {
                    uint8_t wb[16];

                    memcpy(wb, &v->regs.rcx, 8);
                    memcpy(wb + 8, &v->regs.rbx, 8);
                    rr = vfl_mem_write(vm, v, ea.addr, 16, wb);
                    if (rr) {
                        vfl_memerr(v, run, rr);
                        return 1;
                    }
                    v->regs.rflags |= (1ULL << 6);
                } else {
                    v->regs.rax = mlo;
                    v->regs.rdx = mhi;
                    v->regs.rflags &= ~(1ULL << 6);
                }
            } else {
                uint32_t lo = (uint32_t)v->regs.rax;
                uint32_t hi = (uint32_t)v->regs.rdx;
                uint32_t mlo = 0;
                uint32_t mhi = 0;
                uint8_t tb[8];

                rr = vfl_mem_read(vm, v, ea.addr, 8, tb);
                if (rr) {
                    vfl_memerr(v, run, rr);
                    return 1;
                }
                memcpy(&mlo, tb, 4);
                memcpy(&mhi, tb + 4, 4);
                if (mlo == lo && mhi == hi) {
                    uint8_t wb[8];

                    memcpy(wb, &v->regs.rcx, 4);
                    memcpy(wb + 4, &v->regs.rbx, 4);
                    rr = vfl_mem_write(vm, v, ea.addr, 8, wb);
                    if (rr) {
                        vfl_memerr(v, run, rr);
                        return 1;
                    }
                    v->regs.rflags |= (1ULL << 6);
                } else {
                    v->regs.rax = (v->regs.rax & ~0xFFFFFFFFULL) | mlo;
                    v->regs.rdx = (v->regs.rdx & ~0xFFFFFFFFULL) | mhi;
                    v->regs.rflags &= ~(1ULL << 6);
                }
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xAE: {
            *pos += 1;
            if (escape_modrm(v, run, buf, pos, rex_r, rex_b, &mod, &reg, &rm) < 0)
                return vfl_exit_internal(v, run);
            if (reg == 5 || reg == 6) {
                if (mod != 3) return vfl_exit_exception(v, run, 6, 0);
                n = *pos;
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (reg == 7) {
                if (mod == 3) return vfl_exit_exception(v, run, 6, 0);
                n = escape_ea(v, buf, pos, mod, rm, asz, seg, rex_b, rex_x, &ea,
                              v->regs.rip, ripmask, 0);
                if (n < 0) return vfl_exit_internal(v, run);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            n = escape_ea(v, buf, pos, mod, rm, asz, seg, rex_b, rex_x, &ea,
                          v->regs.rip, ripmask, 0);
            if (n < 0) return vfl_exit_internal(v, run);
            if (ea.is_reg) return vfl_exit_exception(v, run, 6, 0);
            if (reg == 0) return simd_fxsave(vm, v, run, ea.addr, n, ripmask, 1);
            if (reg == 1) return simd_fxsave(vm, v, run, ea.addr, n, ripmask, 0);
            if (reg == 2 || reg == 3) {
                uint32_t val = 0;

                if (reg == 2) {
                    rr = vfl_mem_read(vm, v, ea.addr, 4, (uint8_t *)&val);
                    if (rr == -2) {
                        vfl_mem_fault(v, run);
                        return 1;
                    }
                    if (rr == 1) return vfl_mmio_fill_exit(vm, v, run, ea.addr, 4);
                    if (rr < 0) {
                        vfl_exit_exception(v, run, 13, 0);
                        return 1;
                    }
                    v->fpu.mxcsr = val & 0xFFFFu;
                } else {
                    vfl_put64((uint8_t *)&val, v->fpu.mxcsr, 4);
                    rr = vfl_mem_write(vm, v, ea.addr, 4, (uint8_t *)&val);
                    if (rr) {
                        vfl_memerr(v, run, rr);
                        return 1;
                    }
                }
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (reg == 4) return simd_xsave(vm, v, run, ea.addr, n, ripmask, 1);
            if (reg == 5) return simd_xsave(vm, v, run, ea.addr, n, ripmask, 0);
            return vfl_exit_exception(v, run, 6, 0);
        }

        case 0x00:
        case 0x06:
        case 0x08:
        case 0x09:
        case 0x0B:
        case 0x20:
        case 0x21:
        case 0x22:
        case 0x23:
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x77:
        case 0xA2:
        case 0xA0:
        case 0xA1:
        case 0xA8:
        case 0xA9:
        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83:
        case 0x84:
        case 0x85:
        case 0x86:
        case 0x87:
        case 0x88:
        case 0x89:
        case 0x8A:
        case 0x8B:
        case 0x8C:
        case 0x8D:
        case 0x8E:
        case 0x8F:
        case 0x90:
        case 0x91:
        case 0x92:
        case 0x93:
        case 0x94:
        case 0x95:
        case 0x96:
        case 0x97:
        case 0x98:
        case 0x99:
        case 0x9A:
        case 0x9B:
        case 0x9C:
        case 0x9D:
        case 0x9E:
        case 0x9F:
        case 0x40:
        case 0x41:
        case 0x42:
        case 0x43:
        case 0x44:
        case 0x45:
        case 0x46:
        case 0x47:
        case 0x48:
        case 0x49:
        case 0x4A:
        case 0x4B:
        case 0x4C:
        case 0x4D:
        case 0x4E:
        case 0x4F:
        case 0xA4:
        case 0xA5:
        case 0xAC:
        case 0xAD:
        case 0xA3:
        case 0xAB:
        case 0xB3:
        case 0xBB:
        case 0xBA:
        case 0xAF:
        case 0xB6:
        case 0xB7:
        case 0xBE:
        case 0xBF:
        case 0xB0:
        case 0xB1:
        case 0xBC:
        case 0xBD:
        case 0xC0:
        case 0xC1:
        case 0xC8:
        case 0xC9:
        case 0xCA:
        case 0xCB:
        case 0xCC:
        case 0xCD:
        case 0xCE:
        case 0xCF:
        case 0xF1:
            return VFL_SIMD_NOTME;

        default:
            break;
    }
    return vfl_simd_media(vm, v, run, buf, pos, op2, pp, osz, asz, seg, rep,
                          rex, rex_w, rex_r, rex_b, rex_x, lm, ripmask);
}

static int sse_unpck(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                     int reg, int lanes, int el, int hi, int n, uint64_t ripmask) {
    uint32_t a[8];
    uint32_t b[8];
    uint32_t r[8];
    int rr;
    int i;

    vec_read(v, reg, lanes, a);
    if (ea->is_reg) {
        vec_read(v, ea->reg, lanes, b);
    } else {
        rr = sse_load_vec(vm, v, run, ea, lanes, b);
        if (rr) return 1;
    }
    if (el == 0) {
        uint8_t *pa = (uint8_t *)a;
        uint8_t *pb = (uint8_t *)b;
        uint8_t *pr = (uint8_t *)r;
        int base = hi ? 8 * (lanes / 4) : 0;

        for (i = 0; i < 8 * (lanes / 4); i++) {
            pr[i * 2] = pa[base + i];
            pr[i * 2 + 1] = pb[base + i];
        }
    } else if (el == 1) {
        uint16_t *pa = (uint16_t *)a;
        uint16_t *pb = (uint16_t *)b;
        uint16_t *pr = (uint16_t *)r;
        int base = hi ? 4 * (lanes / 4) : 0;

        for (i = 0; i < 4 * (lanes / 4); i++) {
            pr[i * 2] = pa[base + i];
            pr[i * 2 + 1] = pb[base + i];
        }
    } else {
        int base = hi ? 2 * (lanes / 4) : 0;

        for (i = 0; i < 2 * (lanes / 4); i++) {
            r[i * 2] = a[base + i];
            r[i * 2 + 1] = b[base + i];
        }
    }
    vec_write(v, reg, lanes, r, 0);
    vfl_adv(v, n, ripmask);
    return 0;
}

static int sse_pack(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                    int reg, int lanes, int which, int n, uint64_t ripmask) {
    uint32_t a[8];
    uint32_t b[8];
    int rr;

    vec_read(v, reg, lanes, a);
    if (ea->is_reg) {
        vec_read(v, ea->reg, lanes, b);
    } else {
        rr = sse_load_vec(vm, v, run, ea, lanes, b);
        if (rr) return 1;
    }
    if (which == 0) {
        int16_t *pa = (int16_t *)a;
        int16_t *pb = (int16_t *)b;
        uint8_t r[32];
        int nb = lanes * 4;
        int i;

        for (i = 0; i < nb; i++) {
            int v16 = pa[i];

            r[i] = (uint8_t)(v16 < -128 ? -128 : (v16 > 127 ? 127 : v16));
        }
        for (i = 0; i < nb; i++) {
            int v16 = pb[i];

            r[nb + i] = (uint8_t)(v16 < -128 ? -128 : (v16 > 127 ? 127 : v16));
        }
        for (i = 0; i < lanes; i++)
            memcpy(&a[i], r + i * 4, 4);
    } else if (which == 1) {
        int32_t *pa = (int32_t *)a;
        int32_t *pb = (int32_t *)b;
        uint16_t r[16];
        int nw = lanes * 2;
        int i;

        for (i = 0; i < nw; i++) {
            int v32 = pa[i];

            r[i] = (uint16_t)(v32 < -32768 ? -32768 : (v32 > 32767 ? 32767 : v32));
        }
        for (i = 0; i < nw; i++) {
            int v32 = pb[i];

            r[nw + i] = (uint16_t)(v32 < -32768 ? -32768 : (v32 > 32767 ? 32767 : v32));
        }
        for (i = 0; i < lanes; i++)
            memcpy(&a[i], r + i * 2, 4);
    } else if (which == 2) {
        int16_t *pa = (int16_t *)a;
        int16_t *pb = (int16_t *)b;
        uint8_t r[32];
        int nb = lanes * 4;
        int i;

        for (i = 0; i < nb; i++) {
            int v16 = pa[i];

            r[i] = (uint8_t)(v16 < 0 ? 0 : (v16 > 255 ? 255 : v16));
        }
        for (i = 0; i < nb; i++) {
            int v16 = pb[i];

            r[nb + i] = (uint8_t)(v16 < 0 ? 0 : (v16 > 255 ? 255 : v16));
        }
        for (i = 0; i < lanes; i++)
            memcpy(&a[i], r + i * 4, 4);
    } else {
        int32_t *pa = (int32_t *)a;
        int32_t *pb = (int32_t *)b;
        uint16_t r[16];
        int nw = lanes * 2;
        int i;

        for (i = 0; i < nw; i++) {
            int64_t v32 = pa[i];

            r[i] = (uint16_t)(v32 < 0 ? 0 : (v32 > 65535 ? 65535 : v32));
        }
        for (i = 0; i < nw; i++) {
            int64_t v32 = pb[i];

            r[nw + i] = (uint16_t)(v32 < 0 ? 0 : (v32 > 65535 ? 65535 : v32));
        }
        for (i = 0; i < lanes; i++)
            memcpy(&a[i], r + i * 2, 4);
    }
    vec_write(v, reg, lanes, a, 0);
    vfl_adv(v, n, ripmask);
    return 0;
}

static int sse_hadd(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                    int reg, int lanes, int which, int isd, int n, uint64_t ripmask) {
    uint32_t a[8];
    uint32_t b[8];
    uint32_t r[8];
    int rr;
    int i;

    vec_read(v, reg, lanes, a);
    if (ea->is_reg) {
        vec_read(v, ea->reg, lanes, b);
    } else {
        rr = sse_load_vec(vm, v, run, ea, lanes, b);
        if (rr) return 1;
    }
    if (!isd) {
        int16_t *pa = (int16_t *)a;
        int16_t *pb = (int16_t *)b;
        int16_t *pr = (int16_t *)r;
        int nw = lanes * 2;

        for (i = 0; i < nw / 2; i++) {
            int x = pa[i * 2] + pa[i * 2 + 1];
            int y = pb[i * 2] + pb[i * 2 + 1];

            if (which == 0) { pr[i] = (int16_t)x; pr[i + nw / 2] = (int16_t)y; }
            else { pr[i] = (int16_t)(pa[i * 2] - pa[i * 2 + 1]); pr[i + nw / 2] = (int16_t)(pb[i * 2] - pb[i * 2 + 1]); }
            (void)y;
        }
    } else {
        int32_t *pa = (int32_t *)a;
        int32_t *pb = (int32_t *)b;
        int32_t *pr = (int32_t *)r;

        for (i = 0; i < lanes / 2; i++) {
            if (which == 0) { pr[i] = pa[i * 2] + pa[i * 2 + 1]; pr[i + lanes / 2] = pb[i * 2] + pb[i * 2 + 1]; }
            else { pr[i] = pa[i * 2] - pa[i * 2 + 1]; pr[i + lanes / 2] = pb[i * 2] - pb[i * 2 + 1]; }
        }
    }
    for (i = 0; i < lanes; i++)
        memcpy(&a[i], &r[i], 4);
    vec_write(v, reg, lanes, a, 0);
    vfl_adv(v, n, ripmask);
    return 0;
}

static int media_pre(vfl_vcpu_t *v, uint8_t *buf, int *pos, int asz, int seg,
                     int rex_r, int rex_b, int rex_x, uint64_t ripmask, int tailsz,
                     int *mod, int *reg, vfl_ea_t *ea) {
    uint8_t m;
    int rm;
    int n;

    if (*pos + 1 >= 15) return -1;
    (*pos)++;
    m = buf[*pos];
    (*pos)++;
    *mod = (m >> 6) & 3;
    *reg = ((m >> 3) & 7) | (rex_r << 3);
    rm = (m & 7) | (rex_b << 3);
    if (vfl_decode_ea(v, buf, pos, *mod, rm, asz, seg, rex_b, rex_x, ea) < 0)
        return -1;
    n = *pos + tailsz;
    if (!ea->is_reg && ea->rip_rel)
        ea->addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea->disp) & ripmask;
    return n;
}

static int sse_scalar_f32(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                          int reg, int which, int n, uint64_t ripmask) {
    uint32_t d[4];
    float y = 0;

    vec_read(v, reg, 4, d);
    if (ea->is_reg) {
        uint32_t t[4];

        vec_read(v, ea->reg, 4, t);
        y = vfl_u32f(t[0]);
    } else {
        uint8_t tmp[4];

        if (sse_mem_fill(vm, v, run, ea->addr, tmp, 4)) return 1;
        memcpy(&y, tmp, 4);
    }
    {
        float x = vfl_u32f(d[0]);

        if (which == 0) d[0] = fu32(vfl_f32_add(x, y));
        else if (which == 1) d[0] = fu32(vfl_f32_sub(x, y));
        else if (which == 2) d[0] = fu32(vfl_f32_mul(x, y));
        else if (which == 3) d[0] = fu32(vfl_f32_div(x, y));
        else if (which == 4) d[0] = fu32(vfl_f32_sqrt(x));
        else if (which == 5) d[0] = f32_min(x, y);
        else d[0] = f32_max(x, y);
    }
    vec_write(v, reg, 4, d, 0);
    vfl_adv(v, n, ripmask);
    return 0;
}

static int sse_scalar_f64(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                          int reg, int which, int n, uint64_t ripmask) {
    uint32_t d[4];
    double y = 0;

    vec_read(v, reg, 4, d);
    if (ea->is_reg) {
        uint32_t t[4];

        vec_read(v, ea->reg, 4, t);
        memcpy(&y, t, 8);
    } else {
        uint8_t tmp[8];

        if (sse_mem_fill(vm, v, run, ea->addr, tmp, 8)) return 1;
        memcpy(&y, tmp, 8);
    }
    {
        double x;

        memcpy(&x, d, 8);
        if (which == 0) x = vfl_f64_add(x, y);
        else if (which == 1) x = vfl_f64_sub(x, y);
        else if (which == 2) x = vfl_f64_mul(x, y);
        else if (which == 3) x = vfl_f64_div(x, y);
        else if (which == 4) x = vfl_f64_sqrt(x);
        else if (which == 5) {
            uint64_t r = f64_min(x, y);

            memcpy(d, &r, 8);
            vec_write(v, reg, 4, d, 0);
            vfl_adv(v, n, ripmask);
            return 0;
        } else {
            uint64_t r = f64_max(x, y);

            memcpy(d, &r, 8);
            vec_write(v, reg, 4, d, 0);
            vfl_adv(v, n, ripmask);
            return 0;
        }
        memcpy(d, &x, 8);
    }
    vec_write(v, reg, 4, d, 0);
    vfl_adv(v, n, ripmask);
    return 0;
}

static int sse_comi(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                    int reg, int isd, int ucom, int n, uint64_t ripmask) {
    (void)ucom;
    if (isd) {
        uint32_t d[4];
        double x;
        double y = 0;

        vec_read(v, reg, 4, d);
        memcpy(&x, d, 8);
        if (ea->is_reg) {
            vec_read(v, ea->reg, 4, d);
            memcpy(&y, d, 8);
        } else {
            uint8_t tmp[8];

            if (sse_mem_fill(vm, v, run, ea->addr, tmp, 8)) return 1;
            memcpy(&y, tmp, 8);
        }
        v->regs.rflags &= ~((1ULL << 0) | (1ULL << 2) | (1ULL << 6) |
                            (1ULL << 7) | (1ULL << 11) | (1ULL << 4));
        if (x != x || y != y) {
            v->regs.rflags |= (1ULL << 0) | (1ULL << 2) | (1ULL << 6);
        } else {
            if (x == y) v->regs.rflags |= (1ULL << 6);
            if (x < y) v->regs.rflags |= (1ULL << 0);
        }
    } else {
        uint32_t d[4];
        float x;
        float y = 0;

        vec_read(v, reg, 4, d);
        x = vfl_u32f(d[0]);
        if (ea->is_reg) {
            vec_read(v, ea->reg, 4, d);
            y = vfl_u32f(d[0]);
        } else {
            uint8_t tmp[4];

            if (sse_mem_fill(vm, v, run, ea->addr, tmp, 4)) return 1;
            memcpy(&y, tmp, 4);
        }
        v->regs.rflags &= ~((1ULL << 0) | (1ULL << 2) | (1ULL << 6) |
                            (1ULL << 7) | (1ULL << 11) | (1ULL << 4));
        if (x != x || y != y) {
            v->regs.rflags |= (1ULL << 0) | (1ULL << 2) | (1ULL << 6);
        } else {
            if (x == y) v->regs.rflags |= (1ULL << 6);
            if (x < y) v->regs.rflags |= (1ULL << 0);
        }
    }
    vfl_adv(v, n, ripmask);
    return 0;
}


static int media_arith(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                       int *pos, uint8_t op2, int pp, int asz, int seg,
                       int rex, int rex_r, int rex_b, int rex_x,
                       int lm, uint64_t ripmask) {
    int mod;
    int reg;
    vfl_ea_t ea;
    int n;
    int rr;

    (void)lm;
    (void)rex;
    switch (op2) {
        case 0x54:
        case 0x55:
        case 0x56:
        case 0x57:
            if (pp != 0 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            return sse_bitop(vm, v, run, &ea, reg, 4, op2 - 0x54 + 1, n, ripmask);

        case 0x58:
        case 0x59:
        case 0x5C:
        case 0x5E:
        case 0x51:
        case 0x5D:
        case 0x5F:
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (pp == 0)
                return sse_arith_f32(vm, v, run, &ea, reg, 4,
                                     op2 == 0x58 ? 0 : (op2 == 0x5C ? 1 : (op2 == 0x59 ? 2 :
                                     (op2 == 0x5E ? 3 : (op2 == 0x51 ? 4 : (op2 == 0x5D ? 5 : 6))))),
                                     n, ripmask);
            if (pp == 1)
                return sse_arith_f64(vm, v, run, &ea, reg, 4,
                                     op2 == 0x58 ? 0 : (op2 == 0x5C ? 1 : (op2 == 0x59 ? 2 :
                                     (op2 == 0x5E ? 3 : (op2 == 0x51 ? 4 : (op2 == 0x5D ? 5 : 6))))),
                                     n, ripmask);
            if (pp == 3)
                return sse_scalar_f32(vm, v, run, &ea, reg,
                                      op2 == 0x58 ? 0 : (op2 == 0x5C ? 1 : (op2 == 0x59 ? 2 :
                                      (op2 == 0x5E ? 3 : (op2 == 0x51 ? 4 : (op2 == 0x5D ? 5 : 6))))),
                                      n, ripmask);
            if (pp == 2)
                return sse_scalar_f64(vm, v, run, &ea, reg,
                                      op2 == 0x58 ? 0 : (op2 == 0x5C ? 1 : (op2 == 0x59 ? 2 :
                                      (op2 == 0x5E ? 3 : (op2 == 0x51 ? 4 : (op2 == 0x5D ? 5 : 6))))),
                                      n, ripmask);
            return vfl_exit_exception(v, run, 6, 0);

        case 0x52:
            if (pp != 0 && pp != 3) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (pp == 0)
                return sse_arith_f32(vm, v, run, &ea, reg, 4, 8, n, ripmask);
            return sse_scalar_f32(vm, v, run, &ea, reg, 8, n, ripmask);

        case 0x53:
            if (pp != 0 && pp != 3) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (pp == 0)
                return sse_arith_f32(vm, v, run, &ea, reg, 4, 7, n, ripmask);
            return sse_scalar_f32(vm, v, run, &ea, reg, 7, n, ripmask);

        case 0x5A:
        case 0x5B: {
            uint32_t d[8];
            uint32_t t[8];

            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (ea.is_reg) {
                vec_read(v, ea.reg, 4, t);
            } else {
                rr = sse_load_vec(vm, v, run, &ea, 4, t);
                if (rr) return 1;
            }
            if (op2 == 0x5A && pp == 0) {
                uint64_t u0 = f32_to_f64(t[0]);
                uint64_t u1 = f32_to_f64(t[1]);

                d[0] = (uint32_t)u0;
                d[1] = (uint32_t)(u0 >> 32);
                d[2] = (uint32_t)u1;
                d[3] = (uint32_t)(u1 >> 32);
            } else if (op2 == 0x5A && pp == 1) {
                uint64_t u0 = (uint64_t)t[0] | ((uint64_t)t[1] << 32);
                uint64_t u1 = (uint64_t)t[2] | ((uint64_t)t[3] << 32);

                d[0] = f64_to_f32(u0);
                d[1] = f64_to_f32(u1);
                d[2] = 0;
                d[3] = 0;
            } else if (op2 == 0x5A && pp == 3) {
                uint64_t u = f32_to_f64(t[0]);

                vec_read(v, reg, 4, d);
                d[0] = (uint32_t)u;
                d[1] = (uint32_t)(u >> 32);
            } else if (op2 == 0x5A && pp == 2) {
                uint64_t u = (uint64_t)t[0] | ((uint64_t)t[1] << 32);

                vec_read(v, reg, 4, d);
                d[0] = f64_to_f32(u);
            } else if (op2 == 0x5B && pp == 0) {
                int i;

                for (i = 0; i < 4; i++) {
                    float f = (float)(int32_t)t[i];

                    memcpy(&d[i], &f, 4);
                }
            } else if (op2 == 0x5B && pp == 1) {
                int i;

                vec_read(v, reg, 4, d);
                for (i = 0; i < 4; i++) {
                    int ov = 0;
                    int64_t q = f32_rint_i64(t[i], &ov);

                    if (ov || q != (int64_t)(int32_t)q) d[i] = 0x80000000u;
                    else d[i] = (uint32_t)(int32_t)q;
                }
            } else if (op2 == 0x5B && pp == 3) {
                int i;

                vec_read(v, reg, 4, d);
                for (i = 0; i < 4; i++) {
                    float x = vfl_u32f(t[i]);

                    if (!(x > -2.1474836e9f) || !(x < 2.1474836e9f)) d[i] = 0x80000000u;
                    else d[i] = (uint32_t)(int32_t)x;
                }
            } else {
                return vfl_exit_exception(v, run, 6, 0);
            }
            vec_write(v, reg, 4, d, 0);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xC2: {
            int imm;

            if (*pos + 2 >= 15) return vfl_exit_internal(v, run);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 1,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            imm = buf[n - 1] & 7;
            if (pp == 0)
                return sse_cmp_f32(vm, v, run, &ea, reg, 4, imm, n, ripmask);
            if (pp == 1) {
                uint64_t a[2];
                uint64_t b[2];
                uint32_t t[8];
                uint32_t r[8];
                int i;

                vec_read(v, reg, 4, t);
                for (i = 0; i < 2; i++)
                    a[i] = (uint64_t)t[i * 2] | ((uint64_t)t[i * 2 + 1] << 32);
                if (ea.is_reg) {
                    vec_read(v, ea.reg, 4, t);
                    for (i = 0; i < 2; i++)
                        b[i] = (uint64_t)t[i * 2] | ((uint64_t)t[i * 2 + 1] << 32);
                } else {
                    rr = sse_load_vec(vm, v, run, &ea, 4, t);
                    if (rr) return 1;
                    for (i = 0; i < 2; i++)
                        b[i] = (uint64_t)t[i * 2] | ((uint64_t)t[i * 2 + 1] << 32);
                }
                for (i = 0; i < 2; i++) {
                    double x = vfl_u64d(a[i]);
                    double y = vfl_u64d(b[i]);
                    int eq = x == y;
                    int lt = x < y;
                    int un = x != x || y != y;
                    uint64_t m = 0;

                    if (imm == 0) m = eq && !un ? ~0ULL : 0;
                    else if (imm == 1) m = lt && !un ? ~0ULL : 0;
                    else if (imm == 2) m = (lt || eq) && !un ? ~0ULL : 0;
                    else if (imm == 3) m = un ? ~0ULL : 0;
                    else if (imm == 4) m = (!eq || un) ? ~0ULL : 0;
                    else if (imm == 5) m = (!lt || un) ? ~0ULL : 0;
                    else if (imm == 6) m = ((!lt && !eq) || un) ? ~0ULL : 0;
                    else m = !un ? ~0ULL : 0;
                    r[i * 2] = (uint32_t)m;
                    r[i * 2 + 1] = (uint32_t)(m >> 32);
                }
                vec_write(v, reg, 4, r, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (pp == 3) {
                uint32_t d[4];
                float y;

                vec_read(v, reg, 4, d);
                if (ea.is_reg) {
                    uint32_t t[4];

                    vec_read(v, ea.reg, 4, t);
                    y = vfl_u32f(t[0]);
                } else {
                    uint8_t tmp[4];

                    if (sse_mem_fill(vm, v, run, ea.addr, tmp, 4)) return 1;
                    memcpy(&y, tmp, 4);
                }
                d[0] = f32_cmp(vfl_u32f(d[0]), y, imm);
                vec_write(v, reg, 4, d, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (pp == 2) {
                uint32_t d[4];
                double y;
                double x;

                vec_read(v, reg, 4, d);
                memcpy(&x, d, 8);
                if (ea.is_reg) {
                    uint32_t t[4];

                    vec_read(v, ea.reg, 4, t);
                    memcpy(&y, t, 8);
                } else {
                    uint8_t tmp[8];

                    if (sse_mem_fill(vm, v, run, ea.addr, tmp, 8)) return 1;
                    memcpy(&y, tmp, 8);
                }
                {
                    int eq = x == y;
                    int lt = x < y;
                    int un = x != x || y != y;
                    uint64_t m = 0;

                    if (imm == 0) m = eq && !un ? ~0ULL : 0;
                    else if (imm == 1) m = lt && !un ? ~0ULL : 0;
                    else if (imm == 2) m = (lt || eq) && !un ? ~0ULL : 0;
                    else if (imm == 3) m = un ? ~0ULL : 0;
                    else if (imm == 4) m = (!eq || un) ? ~0ULL : 0;
                    else if (imm == 5) m = (!lt || un) ? ~0ULL : 0;
                    else if (imm == 6) m = ((!lt && !eq) || un) ? ~0ULL : 0;
                    else m = !un ? ~0ULL : 0;
                    d[0] = (uint32_t)m;
                    d[1] = (uint32_t)(m >> 32);
                }
                vec_write(v, reg, 4, d, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            return vfl_exit_exception(v, run, 6, 0);
        }

        case 0xC6: {
            int imm;

            if (pp != 0) return vfl_exit_exception(v, run, 6, 0);
            if (*pos + 2 >= 15) return vfl_exit_internal(v, run);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 1,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            imm = buf[n - 1];
            return sse_shufps(vm, v, run, &ea, reg, 4, imm, 0, n, ripmask);
        }

        case 0xD0:
        case 0xD1:
            if (pp != 2) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            {
                uint32_t a[8];
                uint32_t b[8];
                uint32_t r[8];
                int i;

                vec_read(v, reg, 4, a);
                if (ea.is_reg) vec_read(v, ea.reg, 4, b);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, 4, b);
                    if (rr) return 1;
                }
                if (op2 == 0xD0) {
                    for (i = 0; i < 4; i++) {
                        float x = vfl_u32f(a[i]);
                        float y = vfl_u32f(b[i]);

                        if (i & 1) r[i] = fu32(vfl_f32_add(x, y));
                        else r[i] = fu32(vfl_f32_sub(x, y));
                    }
                } else {
                    for (i = 0; i < 2; i++) {
                        uint64_t au = (uint64_t)a[i * 2] | ((uint64_t)a[i * 2 + 1] << 32);
                        uint64_t bu = (uint64_t)b[i * 2] | ((uint64_t)b[i * 2 + 1] << 32);
                        double x = vfl_u64d(au);
                        double y = vfl_u64d(bu);
                        uint64_t o = i & 1 ? du64(vfl_f64_add(x, y)) : du64(vfl_f64_sub(x, y));

                        r[i * 2] = (uint32_t)o;
                        r[i * 2 + 1] = (uint32_t)(o >> 32);
                    }
                }
                vec_write(v, reg, 4, r, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }

        case 0x7C:
        case 0x7D: {
            int ispd = pp == 1;

            if (pp != 2 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            {
                uint32_t a[8];
                uint32_t b[8];
                uint32_t r[8];

                vec_read(v, reg, 4, a);
                if (ea.is_reg) vec_read(v, ea.reg, 4, b);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, 4, b);
                    if (rr) return 1;
                }
                if (!ispd) {
                    float x0 = vfl_u32f(a[0]);
                    float x1 = vfl_u32f(a[1]);
                    float x2 = vfl_u32f(a[2]);
                    float x3 = vfl_u32f(a[3]);
                    float y0 = vfl_u32f(b[0]);
                    float y1 = vfl_u32f(b[1]);
                    float y2 = vfl_u32f(b[2]);
                    float y3 = vfl_u32f(b[3]);

                    if (op2 == 0x7C) {
                        r[0] = fu32(vfl_f32_add(x0, x1));
                        r[1] = fu32(vfl_f32_add(x2, x3));
                        r[2] = fu32(vfl_f32_add(y0, y1));
                        r[3] = fu32(vfl_f32_add(y2, y3));
                    } else {
                        r[0] = fu32(vfl_f32_sub(x0, x1));
                        r[1] = fu32(vfl_f32_sub(x2, x3));
                        r[2] = fu32(vfl_f32_sub(y0, y1));
                        r[3] = fu32(vfl_f32_sub(y2, y3));
                    }
                } else {
                    uint64_t au0 = (uint64_t)a[0] | ((uint64_t)a[1] << 32);
                    uint64_t au1 = (uint64_t)a[2] | ((uint64_t)a[3] << 32);
                    uint64_t bu0 = (uint64_t)b[0] | ((uint64_t)b[1] << 32);
                    uint64_t bu1 = (uint64_t)b[2] | ((uint64_t)b[3] << 32);
                    double x0 = vfl_u64d(au0);
                    double x1 = vfl_u64d(au1);
                    double y0 = vfl_u64d(bu0);
                    double y1 = vfl_u64d(bu1);
                    uint64_t o0;
                    uint64_t o1;

                    if (op2 == 0x7C) {
                        o0 = du64(vfl_f64_add(x0, x1));
                        o1 = du64(vfl_f64_add(y0, y1));
                    } else {
                        o0 = du64(vfl_f64_sub(x0, x1));
                        o1 = du64(vfl_f64_sub(y0, y1));
                    }
                    r[0] = (uint32_t)o0;
                    r[1] = (uint32_t)(o0 >> 32);
                    r[2] = (uint32_t)o1;
                    r[3] = (uint32_t)(o1 >> 32);
                }
                vec_write(v, reg, 4, r, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }
        }

        default:
            break;
    }
    return vfl_exit_exception(v, run, 6, 0);
}

static int media_int(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                     int *pos, uint8_t op2, int pp, int asz, int seg,
                     int rex, int rex_r, int rex_b, int rex_x,
                     int lm, uint64_t ripmask) {
    int mod;
    int reg;
    vfl_ea_t ea;
    int n;
    int rr;
    int i;

    (void)lm;
    (void)rex;
    switch (op2) {
        case 0x60:
        case 0x61:
        case 0x62:
        case 0x68:
        case 0x69:
        case 0x6A:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            {
                int el = op2 == 0x60 || op2 == 0x68 ? 0 : (op2 == 0x61 || op2 == 0x69 ? 1 : 2);
                int hi = op2 >= 0x68;

                return sse_unpck(vm, v, run, &ea, reg, 4, el, hi, n, ripmask);
            }

        case 0x63:
        case 0x67:
        case 0x6B:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            return sse_pack(vm, v, run, &ea, reg, 4, op2 == 0x63 ? 0 : (op2 == 0x67 ? 1 : 2),
                            n, ripmask);

        case 0x64:
        case 0x65:
        case 0x66:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            return sse_intop(vm, v, run, &ea, reg, 4, op2 - 0x64, 3, n, ripmask);

        case 0x74:
        case 0x75:
        case 0x76:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            return sse_intop(vm, v, run, &ea, reg, 4, op2 - 0x74, 2, n, ripmask);

        case 0x77:
            (*pos)++;
            n = *pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0x70: {
            int imm;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (*pos + 2 >= 15) return vfl_exit_internal(v, run);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 1,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            imm = buf[n - 1];
            {
                uint32_t t[4];
                uint32_t r[4];

                if (ea.is_reg) vec_read(v, ea.reg, 4, t);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, 4, t);
                    if (rr) return 1;
                }
                for (i = 0; i < 4; i++)
                    r[i] = t[(imm >> (i * 2)) & 3];
                vec_write(v, reg, 4, r, 0);
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x71:
        case 0x72:
        case 0x73: {
            int grp;
            int imm;
            int el = op2 == 0x71 ? 0 : (op2 == 0x72 ? 1 : 2);

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (*pos + 2 >= 15) return vfl_exit_internal(v, run);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 1,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            grp = reg;
            imm = buf[n - 1];
            if (el == 2 && (grp == 3 || grp == 7)) {
                uint32_t t[4];

                if (ea.is_reg) vec_read(v, ea.reg, 4, t);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, 4, t);
                    if (rr) return 1;
                }
                {
                    uint8_t *b = (uint8_t *)t;
                    uint8_t r[16];
                    int k;

                    if (grp == 3) {
                        for (k = 0; k < 16; k++)
                            r[k] = k + imm < 16 ? b[k + imm] : 0;
                    } else {
                        for (k = 15; k >= 0; k--)
                            r[k] = k - imm >= 0 ? b[k - imm] : 0;
                    }
                    for (k = 0; k < 4; k++)
                        memcpy(&t[k], r + k * 4, 4);
                }
                if (ea.is_reg) {
                    vec_write(v, ea.reg, 4, t, 0);
                } else {
                    return sse_store_vec(vm, v, run, &ea, 4, t, n, ripmask, 0);
                }
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (grp != 2 && grp != 4 && grp != 6) return vfl_exit_exception(v, run, 6, 0);
            {
                uint32_t t[4];
                int which = grp == 2 ? 1 : (grp == 4 ? 2 : 0);

                if (ea.is_reg) vec_read(v, ea.reg, 4, t);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, 4, t);
                    if (rr) return 1;
                }
                if (el == 0) {
                    uint16_t *pa = (uint16_t *)t;
                    int k;

                    if ((uint64_t)imm > 15) imm = 16;
                    for (k = 0; k < 8; k++) {
                        if (which == 1) pa[k] >>= imm;
                        else if (which == 2) pa[k] = (uint16_t)((int16_t)pa[k] >> imm);
                        else pa[k] <<= imm;
                    }
                } else if (el == 1) {
                    if ((uint64_t)imm > 31) imm = 32;
                    {
                        int k2;

                        for (k2 = 0; k2 < 4; k2++) {
                            if (which == 1) t[k2] >>= imm;
                            else if (which == 2) t[k2] = (uint32_t)((int32_t)t[k2] >> imm);
                            else t[k2] <<= imm;
                        }
                    }
                } else {
                    int q;

                    if ((uint64_t)imm > 63) imm = 64;
                    for (q = 0; q < 2; q++) {
                        uint64_t x = (uint64_t)t[q * 2] | ((uint64_t)t[q * 2 + 1] << 32);
                        uint64_t r;

                        if (which == 1) r = imm >= 64 ? 0 : x >> imm;
                        else if (which == 2) r = imm >= 64 ? ((int64_t)x < 0 ? ~0ULL : 0) : (uint64_t)((int64_t)x >> imm);
                        else r = imm >= 64 ? 0 : x << imm;
                        t[q * 2] = (uint32_t)r;
                        t[q * 2 + 1] = (uint32_t)(r >> 32);
                    }
                }
                if (ea.is_reg) {
                    vec_write(v, ea.reg, 4, t, 0);
                } else {
                    return sse_store_vec(vm, v, run, &ea, 4, t, n, ripmask, 0);
                }
                vfl_adv(v, n, ripmask);
                return 0;
            }
        }

        case 0xC4:
        case 0xC5: {
            int imm;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (*pos + 2 >= 15) return vfl_exit_internal(v, run);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 1,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            imm = buf[n - 1];
            if (op2 == 0xC4) {
                uint64_t gv;

                if (ea.is_reg) gv = vfl_read_reg(v, ea.reg, rex, 4);
                else {
                    uint8_t tmp[4];

                    if (sse_mem_fill(vm, v, run, ea.addr, tmp, 4)) return 1;
                    memcpy(&gv, tmp, 4);
                }
                {
                    uint32_t d[4];

                    vec_read(v, reg, 4, d);
                    {
                        uint16_t *w = (uint16_t *)d;

                        w[imm & 7] = (uint16_t)gv;
                    }
                    vec_write(v, reg, 4, d, 0);
                }
            } else {
                uint32_t src[4];
                uint16_t val;

                if (mod != 3) return vfl_exit_exception(v, run, 6, 0);
                vec_read(v, ea.reg, 4, src);
                val = (uint16_t)(((uint16_t *)src)[imm & 7]);
                vfl_write_reg(v, reg, rex, 4, val);
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xD1:
        case 0xD2:
        case 0xD3:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            return sse_shift_cnt(vm, v, run, &ea, reg, 4, op2 - 0xD1, 1, n, ripmask);

        case 0xD0:
            if (pp != 2) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            {
                uint32_t a[8];
                uint32_t b[8];
                uint32_t r[8];
                int k;

                vec_read(v, reg, 4, a);
                if (ea.is_reg) vec_read(v, ea.reg, 4, b);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, 4, b);
                    if (rr) return 1;
                }
                for (k = 0; k < 2; k++) {
                    uint64_t au = (uint64_t)a[k * 2] | ((uint64_t)a[k * 2 + 1] << 32);
                    uint64_t bu = (uint64_t)b[k * 2] | ((uint64_t)b[k * 2 + 1] << 32);
                    double x = vfl_u64d(au);
                    double y = vfl_u64d(bu);
                    uint64_t o = du64(vfl_f64_add(x, y));

                    r[k * 2] = (uint32_t)o;
                    r[k * 2 + 1] = (uint32_t)(o >> 32);
                }
                vec_write(v, reg, 4, r, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }

        case 0xD4:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            return sse_intop(vm, v, run, &ea, reg, 4, 3, 0, n, ripmask);

        case 0xD5:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            {
                uint32_t a[8];
                uint32_t b[8];
                uint32_t r[8];
                int k;

                vec_read(v, reg, 4, a);
                if (ea.is_reg) vec_read(v, ea.reg, 4, b);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, 4, b);
                    if (rr) return 1;
                }
                for (k = 0; k < 8; k++) {
                    uint16_t x = (uint16_t)(a[k] & 0xFFFFu);
                    uint16_t y = (uint16_t)(b[k] & 0xFFFFu);
                    uint32_t p = (uint32_t)x * y;

                    r[k] = p >> 16;
                }
                vec_write(v, reg, 4, r, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }

        case 0xD8:
        case 0xD9:
        case 0xDC:
        case 0xDD:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            {
                uint32_t a[8];
                uint32_t b[8];
                uint32_t r[8];
                int nb = 16;
                int k;

                vec_read(v, reg, 4, a);
                if (ea.is_reg) vec_read(v, ea.reg, 4, b);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, 4, b);
                    if (rr) return 1;
                }
                if (op2 == 0xD8 || op2 == 0xD9) {
                    uint8_t *pa = (uint8_t *)a;
                    uint8_t *pb = (uint8_t *)b;
                    uint8_t *pr = (uint8_t *)r;

                    for (k = 0; k < nb; k++) {
                        int x = pa[k];
                        int y = pb[k];

                        if (op2 == 0xD8) {
                            int t = x - y;

                            pr[k] = (uint8_t)(t < 0 ? 0 : (t > 255 ? 255 : t));
                        } else {
                            int t = x - y;

                            pr[k] = (uint8_t)(t < -128 ? -128 : (t > 127 ? 127 : t));
                        }
                    }
                } else {
                    uint16_t *pa = (uint16_t *)a;
                    uint16_t *pb = (uint16_t *)b;
                    uint16_t *pr = (uint16_t *)r;

                    for (k = 0; k < 8; k++) {
                        unsigned x = pa[k];
                        unsigned y = pb[k];

                        if (op2 == 0xDC) {
                            unsigned t = x + y;

                            pr[k] = (uint16_t)(t > 65535 ? 65535 : t);
                        } else {
                            int t = (int)x + (int)y;

                            pr[k] = (uint16_t)(t < 0 ? 0 : (t > 65535 ? 65535 : t));
                        }
                    }
                }
                vec_write(v, reg, 4, r, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }

        case 0xDB:
        case 0xDF:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            return sse_bitop(vm, v, run, &ea, reg, 4, op2 == 0xDB ? 1 : 3, n, ripmask);

        case 0xDE:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            return sse_intop(vm, v, run, &ea, reg, 4, 1, 8, n, ripmask);

        case 0xE0:
        case 0xE3:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            {
                uint32_t a[8];
                uint32_t b[8];
                uint32_t r[8];
                uint8_t *pa;
                uint8_t *pb;
                uint8_t *pr;
                int k;

                vec_read(v, reg, 4, a);
                if (ea.is_reg) vec_read(v, ea.reg, 4, b);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, 4, b);
                    if (rr) return 1;
                }
                pa = (uint8_t *)a;
                pb = (uint8_t *)b;
                pr = (uint8_t *)r;
                for (k = 0; k < 16; k++)
                    pr[k] = (uint8_t)(((uint16_t)pa[k] + pb[k] + 1) >> 1);
                vec_write(v, reg, 4, r, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }

        case 0xE1:
        case 0xE2:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            return sse_shift_cnt(vm, v, run, &ea, reg, 4, op2 == 0xE1 ? 0 : 1,
                                 2, n, ripmask);

        case 0xE4:
        case 0xE5:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            {
                uint32_t a[8];
                uint32_t b[8];
                uint32_t r[8];
                uint16_t *pa;
                uint16_t *pb;
                int k;

                vec_read(v, reg, 4, a);
                if (ea.is_reg) vec_read(v, ea.reg, 4, b);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, 4, b);
                    if (rr) return 1;
                }
                pa = (uint16_t *)a;
                pb = (uint16_t *)b;
                for (k = 0; k < 8; k++) {
                    uint32_t p = (uint32_t)pa[k] * pb[k];

                    if (op2 == 0xE4) r[k] = p >> 16;
                    else r[k] = (uint32_t)(((int32_t)(p) ) >> 16);
                }
                for (k = 0; k < 4; k++)
                    a[k] = r[k * 2] | (r[k * 2 + 1] << 16);
                vec_write(v, reg, 4, a, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }

        case 0xE8:
        case 0xE9:
        case 0xEA:
        case 0xEC:
        case 0xED:
        case 0xEE:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (op2 == 0xEA)
                return sse_intop(vm, v, run, &ea, reg, 4, 1, 9, n, ripmask);
            if (op2 == 0xEE)
                return sse_intop(vm, v, run, &ea, reg, 4, 1, 10, n, ripmask);
            {
                uint32_t a[8];
                uint32_t b[8];
                uint32_t r[8];

                vec_read(v, reg, 4, a);
                if (ea.is_reg) vec_read(v, ea.reg, 4, b);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, 4, b);
                    if (rr) return 1;
                }
                if (op2 == 0xE8 || op2 == 0xEC) {
                    uint8_t *pa = (uint8_t *)a;
                    uint8_t *pb = (uint8_t *)b;
                    uint8_t *pr = (uint8_t *)r;
                    int k;

                    for (k = 0; k < 16; k++) {
                        if (op2 == 0xE8) {
                            int t = (int)(int8_t)pa[k] - (int)(int8_t)pb[k];

                            pr[k] = (uint8_t)(t < -128 ? -128 : (t > 127 ? 127 : t));
                        } else {
                            unsigned t = (unsigned)pa[k] + pb[k];

                            pr[k] = (uint8_t)(t > 255 ? 255 : t);
                        }
                    }
                } else {
                    uint16_t *pa = (uint16_t *)a;
                    uint16_t *pb = (uint16_t *)b;
                    uint16_t *pr = (uint16_t *)r;
                    int k;

                    for (k = 0; k < 8; k++) {
                        if (op2 == 0xE9) {
                            int t = (int)(int16_t)pa[k] - (int)(int16_t)pb[k];

                            pr[k] = (uint16_t)(t < -32768 ? -32768 : (t > 32767 ? 32767 : t));
                        } else {
                            unsigned t = (unsigned)pa[k] + pb[k];

                            pr[k] = (uint16_t)(t > 65535 ? 65535 : t);
                        }
                    }
                }
                vec_write(v, reg, 4, r, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }

        case 0xEB:
        case 0xEF:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            return sse_bitop(vm, v, run, &ea, reg, 4, op2 == 0xEB ? 2 : 0, n, ripmask);

        case 0xE6:
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (pp == 2 || pp == 1) {
                uint32_t t[8];
                uint32_t d[8];
                int k;

                if (ea.is_reg) vec_read(v, ea.reg, 4, t);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, 4, t);
                    if (rr) return 1;
                }
                for (k = 0; k < 2; k++) {
                    uint64_t u = (uint64_t)t[k * 2] | ((uint64_t)t[k * 2 + 1] << 32);

                    if (pp == 2) {
                        double f = (double)(int64_t)u;

                        memcpy(&d[k], &f, 4);
                    } else {
                        long double q = f80_round((long double)vfl_u64d(u));

                        if (!(q > -2.1474836e9L) || !(q < 2.1474836e9L)) d[k] = 0x80000000u;
                        else d[k] = (uint32_t)(int32_t)q;
                    }
                }
                d[2] = 0;
                d[3] = 0;
                vec_write(v, reg, 4, d, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (pp == 3) {
                uint32_t t[8];
                uint32_t d[8];
                int k;

                if (ea.is_reg) vec_read(v, ea.reg, 4, t);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, 2, t);
                    if (rr) return 1;
                }
                for (k = 0; k < 2; k++) {
                    double f = (double)(int32_t)t[k];
                    uint64_t u;

                    memcpy(&u, &f, 8);
                    d[k * 2] = (uint32_t)u;
                    d[k * 2 + 1] = (uint32_t)(u >> 32);
                }
                vec_write(v, reg, 4, d, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            return vfl_exit_exception(v, run, 6, 0);

        case 0xF1:
        case 0xF2:
        case 0xF3: {
            int imm;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (*pos + 2 >= 15) return vfl_exit_internal(v, run);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 1,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            imm = buf[n - 1];
            {
                uint32_t t[4];

                if (ea.is_reg) vec_read(v, ea.reg, 4, t);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, 4, t);
                    if (rr) return 1;
                }
                if (op2 == 0xF1) {
                    uint16_t *pa = (uint16_t *)t;
                    int k;

                    if (imm > 15) imm = 16;
                    for (k = 0; k < 8; k++)
                        pa[k] <<= imm;
                } else if (op2 == 0xF2) {
                    int k;

                    if (imm > 31) imm = 32;
                    for (k = 0; k < 4; k++)
                        t[k] <<= imm;
                } else {
                    int k;

                    if (imm > 63) imm = 64;
                    for (k = 0; k < 2; k++) {
                        uint64_t x = (uint64_t)t[k * 2] | ((uint64_t)t[k * 2 + 1] << 32);
                        uint64_t r = imm >= 64 ? 0 : x << imm;

                        t[k * 2] = (uint32_t)r;
                        t[k * 2 + 1] = (uint32_t)(r >> 32);
                    }
                }
                if (ea.is_reg) {
                    vec_write(v, ea.reg, 4, t, 0);
                } else {
                    return sse_store_vec(vm, v, run, &ea, 4, t, n, ripmask, 0);
                }
                vfl_adv(v, n, ripmask);
                return 0;
            }
        }

        case 0xF4:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            {
                uint32_t a[8];
                uint32_t b[8];
                uint32_t r[8];
                int k;

                vec_read(v, reg, 4, a);
                if (ea.is_reg) vec_read(v, ea.reg, 4, b);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, 4, b);
                    if (rr) return 1;
                }
                for (k = 0; k < 2; k++) {
                    uint64_t p = (uint64_t)a[k * 2] * b[k * 2];

                    r[k * 2] = (uint32_t)p;
                    r[k * 2 + 1] = (uint32_t)(p >> 32);
                }
                vec_write(v, reg, 4, r, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }

        case 0xF5:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            {
                uint32_t a[8];
                uint32_t b[8];
                uint32_t r[8];
                int16_t *pa;
                int16_t *pb;
                int k;

                vec_read(v, reg, 4, a);
                if (ea.is_reg) vec_read(v, ea.reg, 4, b);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, 4, b);
                    if (rr) return 1;
                }
                pa = (int16_t *)a;
                pb = (int16_t *)b;
                for (k = 0; k < 4; k++)
                    r[k] = (uint32_t)((int32_t)pa[k * 2] * pb[k * 2] +
                                      (int32_t)pa[k * 2 + 1] * pb[k * 2 + 1]);
                vec_write(v, reg, 4, r, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }

        case 0xF6:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            {
                uint32_t a[8];
                uint32_t b[8];
                uint32_t r[8];
                uint8_t *pa;
                uint8_t *pb;
                int k;

                vec_read(v, reg, 4, a);
                if (ea.is_reg) vec_read(v, ea.reg, 4, b);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, 4, b);
                    if (rr) return 1;
                }
                pa = (uint8_t *)a;
                pb = (uint8_t *)b;
                for (k = 0; k < 2; k++) {
                    unsigned s = 0;
                    int j;

                    for (j = 0; j < 8; j++) {
                        int d = (int)pa[k * 8 + j] - (int)pb[k * 8 + j];

                        s += (unsigned)(d < 0 ? -d : d);
                    }
                    r[k * 2] = s;
                    r[k * 2 + 1] = 0;
                }
                vec_write(v, reg, 4, r, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }

        case 0xF8:
        case 0xF9:
        case 0xFA:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            return sse_intop(vm, v, run, &ea, reg, 4, op2 - 0xF8, 1, n, ripmask);

        case 0xFB:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            return sse_intop(vm, v, run, &ea, reg, 4, 3, 1, n, ripmask);

        case 0xFC:
        case 0xFD:
        case 0xFE:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            return sse_intop(vm, v, run, &ea, reg, 4, op2 - 0xFC, 0, n, ripmask);

        default:
            break;
    }
    return vfl_exit_exception(v, run, 6, 0);
}

static int media_conv(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                      int *pos, uint8_t op2, int pp, int asz, int seg,
                      int rex, int rex_r, int rex_b, int rex_x,
                      int lm, uint64_t ripmask) {
    int mod;
    int reg;
    vfl_ea_t ea;
    int n;

    (void)lm;
    switch (op2) {
        case 0x2A:
            if (pp != 2 && pp != 3) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            return sse_cvt_si2f(vm, v, run, &ea, reg, rex, pp == 2, n, ripmask);

        case 0x2C:
            if (pp != 2 && pp != 3) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (mod != 3) return vfl_exit_exception(v, run, 6, 0);
            return sse_cvt_f2si(v, ea.reg, rex, pp == 2, 1, n, ripmask);

        case 0x2D:
            if (pp != 2 && pp != 3) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (mod != 3) return vfl_exit_exception(v, run, 6, 0);
            return sse_cvt_f2si(v, ea.reg, rex, pp == 2, 0, n, ripmask);

        case 0x2E:
        case 0x2F:
            if (pp != 0 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            return sse_comi(vm, v, run, &ea, reg, pp == 1, op2 == 0x2E, n, ripmask);

        case 0xE6: {
            uint32_t t[8];
            uint32_t d[8];
            int rr;
            int k;

            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (ea.is_reg) vec_read(v, ea.reg, 4, t);
            else {
                rr = sse_load_vec(vm, v, run, &ea, 4, t);
                if (rr) return 1;
            }
            if (pp == 3) {
                for (k = 0; k < 2; k++) {
                    double f = (double)(int32_t)t[k];
                    uint64_t u;

                    memcpy(&u, &f, 8);
                    d[k * 2] = (uint32_t)u;
                    d[k * 2 + 1] = (uint32_t)(u >> 32);
                }
                vec_write(v, reg, 4, d, 0);
            } else if (pp == 1 || pp == 2) {
                uint64_t u0 = (uint64_t)t[0] | ((uint64_t)t[1] << 32);
                uint64_t u1 = (uint64_t)t[2] | ((uint64_t)t[3] << 32);
                double x0 = vfl_u64d(u0);
                double x1 = vfl_u64d(u1);

                if (pp == 1) {
                    if (!(x0 > -2.1474836e9) || !(x0 < 2.1474836e9)) d[0] = 0x80000000u;
                    else d[0] = (uint32_t)(int32_t)x0;
                    if (!(x1 > -2.1474836e9) || !(x1 < 2.1474836e9)) d[1] = 0x80000000u;
                    else d[1] = (uint32_t)(int32_t)x1;
                } else {
                    long double q0 = (long double)x0;
                    long double q1 = (long double)x1;

                    if (!(q0 > -2.1474836e9L) || !(q0 < 2.1474836e9L)) d[0] = 0x80000000u;
                    else d[0] = (uint32_t)(int32_t)q0;
                    if (!(q1 > -2.1474836e9L) || !(q1 < 2.1474836e9L)) d[1] = 0x80000000u;
                    else d[1] = (uint32_t)(int32_t)q1;
                }
                d[2] = 0;
                d[3] = 0;
                vec_write(v, reg, 4, d, 0);
            } else {
                return vfl_exit_exception(v, run, 6, 0);
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        default:
            break;
    }
    return vfl_exit_exception(v, run, 6, 0);
}

static int vfl_simd_media(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                          int *pos, uint8_t op2, int pp, int osz, int asz, int seg,
                          int rep, int rex, int rex_w, int rex_r, int rex_b,
                          int rex_x, int lm, uint64_t ripmask) {
    (void)osz;
    (void)rep;
    (void)rex_w;
    (void)lm;
    switch (op2) {
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
        case 0x16:
        case 0x17:
        case 0x28:
        case 0x29:
        case 0x2B:
        case 0x6E:
        case 0x6F:
        case 0x7E:
        case 0x7F:
        case 0xD6:
        case 0xE7:
        case 0x50:
        case 0xD7:
        case 0xF0:
            return media_mov(vm, v, run, buf, pos, op2, pp, asz, seg, rex, rex_w,
                             rex_r, rex_b, rex_x, lm, ripmask);

        case 0x54:
        case 0x55:
        case 0x56:
        case 0x57:
        case 0x58:
        case 0x59:
        case 0x5C:
        case 0x5E:
        case 0x51:
        case 0x5D:
        case 0x5F:
        case 0x52:
        case 0x53:
        case 0x5A:
        case 0x5B:
        case 0xC2:
        case 0xC6:
        case 0xD0:
        case 0xD1:
        case 0x7C:
        case 0x7D:
            return media_arith(vm, v, run, buf, pos, op2, pp, asz, seg, rex,
                               rex_r, rex_b, rex_x, lm, ripmask);

        case 0x60:
        case 0x61:
        case 0x62:
        case 0x63:
        case 0x64:
        case 0x65:
        case 0x66:
        case 0x67:
        case 0x68:
        case 0x69:
        case 0x6A:
        case 0x6B:
        case 0x70:
        case 0x71:
        case 0x72:
        case 0x73:
        case 0x74:
        case 0x75:
        case 0x76:
        case 0xC4:
        case 0xC5:
        case 0xD2:
        case 0xD3:
        case 0xD4:
        case 0xD5:
        case 0xD8:
        case 0xD9:
        case 0xDB:
        case 0xDC:
        case 0xDD:
        case 0xDE:
        case 0xDF:
        case 0xE0:
        case 0xE1:
        case 0xE2:
        case 0xE3:
        case 0xE4:
        case 0xE5:
        case 0xE8:
        case 0xE9:
        case 0xEA:
        case 0xEB:
        case 0xEC:
        case 0xED:
        case 0xEE:
        case 0xEF:
        case 0xF1:
        case 0xF2:
        case 0xF3:
        case 0xF4:
        case 0xF5:
        case 0xF6:
        case 0xF8:
        case 0xF9:
        case 0xFA:
        case 0xFB:
        case 0xFC:
        case 0xFD:
        case 0xFE:
            return media_int(vm, v, run, buf, pos, op2, pp, asz, seg, rex,
                             rex_r, rex_b, rex_x, lm, ripmask);

        case 0x2A:
        case 0x2C:
        case 0x2D:
        case 0x2E:
        case 0x2F:
        case 0xE6:
            return media_conv(vm, v, run, buf, pos, op2, pp, asz, seg, rex,
                              rex_r, rex_b, rex_x, lm, ripmask);

        case 0x14:
        case 0x15: {
            int mod;
            int reg;
            vfl_ea_t ea;
            int n;
            int el = pp == 1 ? 1 : 0;
            int hi = op2 == 0x15;

            if (pp != 0 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            return sse_unpck(vm, v, run, &ea, reg, 4, el, hi, n, ripmask);
        }

        case 0x38:
            return simd_38(vm, v, run, buf, pos, pp, asz, seg, rex, rex_r,
                           rex_b, rex_x, lm, ripmask);

        case 0x3A:
            return simd_3a(vm, v, run, buf, pos, pp, asz, seg, rex, rex_r,
                           rex_b, rex_x, lm, ripmask);

        default:
            break;
    }
    return vfl_exit_exception(v, run, 6, 0);
}

static int sse_scalar_store(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                            uint8_t *data, int size, int n, uint64_t ripmask) {
    int rr = vfl_mem_write(vm, v, ea->addr, (uint64_t)size, data);
    uint64_t phys;
    uint64_t val = 0;
    int i;

    if (rr == -2) return vfl_mem_fault(v, run);
    if (rr == 1) {
        if (vfl_translate(vm, v, ea->addr, VFL_MMU_WRITE, &phys))
            return vfl_mem_fault(v, run);
        for (i = 0; i < size && i < 8; i++)
            val |= (uint64_t)data[i] << (i * 8);
        return vfl_exit_mmio_write(vm, v, run, phys, val, size, n, ripmask);
    }
    if (rr < 0) return vfl_exit_exception(v, run, 13, 0);
    vfl_adv(v, n, ripmask);
    return 0;
}

static int media_mov(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                     int *pos, uint8_t op2, int pp, int asz, int seg,
                     int rex, int rex_w, int rex_r, int rex_b, int rex_x,
                     int lm, uint64_t ripmask) {
    int mod;
    int reg;
    vfl_ea_t ea;
    int n;
    int rr;

    (void)lm;
    (void)rex_w;
    switch (op2) {
        case 0x10:
        case 0x11: {
            int align = pp == 0 ? 0 : 16;

            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (pp == 2 || pp == 3) {
                int sad = pp == 3 ? 4 : 8;
                uint32_t d[4];

                if (op2 == 0x10) {
                    vec_read(v, reg, 4, d);
                    if (ea.is_reg) {
                        uint32_t t[4];

                        vec_read(v, ea.reg, 4, t);
                        d[0] = t[0];
                        if (sad == 8) d[1] = t[1];
                    } else {
                        uint8_t tmp[8];

                        rr = vfl_mem_read(vm, v, ea.addr, (uint64_t)sad, tmp);
                        if (rr == -2) return vfl_mem_fault(v, run);
                        if (rr == 1)
                            return vfl_mmio_or_fault(vm, v, run, ea.addr, sad,
                                                     VFL_P_MMIO_XMM, reg, 0, 0, 0,
                                                     n, ripmask);
                        if (rr < 0) return vfl_exit_exception(v, run, 13, 0);
                        memcpy(d, tmp, (size_t)sad);
                    }
                    vec_write(v, reg, 4, d, 0);
                } else {
                    vec_read(v, reg, 4, d);
                    if (ea.is_reg) {
                        uint32_t t[4];

                        vec_read(v, ea.reg, 4, t);
                        d[0] = t[0];
                        if (sad == 8) d[1] = t[1];
                        vec_write(v, ea.reg, 4, d, 0);
                    } else {
                        return sse_scalar_store(vm, v, run, &ea, (uint8_t *)d,
                                                sad, n, ripmask);
                    }
                }
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (pp != 0 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (op2 == 0x10)
                return sse_mov_load(vm, v, run, &ea, reg, 4, n, ripmask, align);
            return sse_mov_store(vm, v, run, &ea, reg, 4, n, ripmask, align);
        }

        case 0x12:
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (pp == 0) {
                uint32_t d[4];
                uint32_t t[4];

                vec_read(v, reg, 4, d);
                if (ea.is_reg) {
                    vec_read(v, ea.reg, 4, t);
                    d[0] = t[2];
                    d[1] = t[3];
                } else {
                    uint8_t tmp[8];

                    rr = vfl_mem_read(vm, v, ea.addr, 8, tmp);
                    if (rr == -2) return vfl_mem_fault(v, run);
                    if (rr == 1)
                        return vfl_mmio_or_fault(vm, v, run, ea.addr, 8,
                                                 VFL_P_MMIO_XMM, reg, 0, 0, 0,
                                                 n, ripmask);
                    if (rr < 0) return vfl_exit_exception(v, run, 13, 0);
                    memcpy(d, tmp, 8);
                }
                vec_write(v, reg, 4, d, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (pp == 3 || pp == 2) {
                uint32_t t[4];
                uint32_t r[4];

                if (ea.is_reg) vec_read(v, ea.reg, 4, t);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, 4, t);
                    if (rr) return 1;
                }
                if (pp == 3) {
                    r[0] = t[0];
                    r[1] = t[0];
                    r[2] = t[2];
                    r[3] = t[2];
                } else {
                    r[0] = t[0];
                    r[1] = t[1];
                    r[2] = t[0];
                    r[3] = t[1];
                }
                vec_write(v, reg, 4, r, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            return vfl_exit_exception(v, run, 6, 0);

        case 0x13:
            if (pp != 0) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (ea.is_reg) return vfl_exit_exception(v, run, 6, 0);
            {
                uint32_t d[4];

                vec_read(v, reg, 4, d);
                return sse_scalar_store(vm, v, run, &ea, (uint8_t *)d, 8, n, ripmask);
            }

        case 0x16:
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (pp == 0) {
                uint32_t d[4];
                uint32_t t[4];

                vec_read(v, reg, 4, d);
                if (ea.is_reg) {
                    vec_read(v, ea.reg, 4, t);
                    d[2] = t[0];
                    d[3] = t[1];
                } else {
                    uint8_t tmp[8];

                    rr = vfl_mem_read(vm, v, ea.addr, 8, tmp);
                    if (rr == -2) return vfl_mem_fault(v, run);
                    if (rr == 1)
                        return vfl_mmio_or_fault(vm, v, run, ea.addr, 8,
                                                 VFL_P_MMIO_XMM, reg, 0, 8, 0,
                                                 n, ripmask);
                    if (rr < 0) return vfl_exit_exception(v, run, 13, 0);
                    memcpy(d + 2, tmp, 8);
                }
                vec_write(v, reg, 4, d, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (pp == 3) {
                uint32_t t[4];
                uint32_t r[4];

                if (ea.is_reg) vec_read(v, ea.reg, 4, t);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, 4, t);
                    if (rr) return 1;
                }
                r[0] = t[1];
                r[1] = t[1];
                r[2] = t[3];
                r[3] = t[3];
                vec_write(v, reg, 4, r, 0);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            return vfl_exit_exception(v, run, 6, 0);

        case 0x17:
            if (pp != 0) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (ea.is_reg) return vfl_exit_exception(v, run, 6, 0);
            {
                uint32_t d[4];

                vec_read(v, reg, 4, d);
                rr = vfl_mem_write(vm, v, ea.addr, 8, (uint8_t *)(d + 2));
                if (rr) {
                    vfl_memerr(v, run, rr);
                    return 1;
                }
            }
            vfl_adv(v, n, ripmask);
            return 0;

        case 0x28:
        case 0x29: {
            int align = 16;

            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (pp != 0 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (op2 == 0x28)
                return sse_mov_load(vm, v, run, &ea, reg, 4, n, ripmask, align);
            return sse_mov_store(vm, v, run, &ea, reg, 4, n, ripmask, align);
        }

        case 0x2B:
            if (pp != 0 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (ea.is_reg) return vfl_exit_exception(v, run, 6, 0);
            return sse_mov_store(vm, v, run, &ea, reg, 4, n, ripmask, 0);

        case 0x6E: {
            int q = rex_w ? 8 : 4;

            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (pp != 0 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (ea.is_reg) {
                uint64_t gv = vfl_read_reg(v, ea.reg, rex, q);
                uint32_t d[4] = {0, 0, 0, 0};

                memcpy(d, &gv, (size_t)q);
                vec_write(v, reg, 4, d, 0);
            } else {
                uint8_t tmp[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                uint32_t d[4] = {0, 0, 0, 0};

                rr = vfl_mem_read(vm, v, ea.addr, (uint64_t)q, tmp);
                if (rr == -2) return vfl_mem_fault(v, run);
                if (rr == 1) {
                    vec_write(v, reg, 4, d, 0);
                    return vfl_mmio_or_fault(vm, v, run, ea.addr, q,
                                             VFL_P_MMIO_XMM, reg, 0, 0, 0,
                                             n, ripmask);
                }
                if (rr < 0) return vfl_exit_exception(v, run, 13, 0);
                memcpy(d, tmp, (size_t)q);
                vec_write(v, reg, 4, d, 0);
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x7E: {
            int q = (pp == 3 || rex_w) ? 8 : 4;

            if (pp != 0 && pp != 1 && pp != 3) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            {
                uint32_t d[4];

                vec_read(v, reg, 4, d);
                if (ea.is_reg) {
                    uint64_t gv = 0;

                    memcpy(&gv, d, (size_t)q);
                    vfl_write_reg(v, ea.reg, rex, q, gv);
                } else {
                    return sse_scalar_store(vm, v, run, &ea, (uint8_t *)d,
                                            q, n, ripmask);
                }
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x6F:
        case 0x7F: {
            int align = pp == 1 ? 16 : 0;

            if (pp != 1 && pp != 3) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (op2 == 0x6F)
                return sse_mov_load(vm, v, run, &ea, reg, 4, n, ripmask, align);
            if (ea.is_reg) return vfl_exit_exception(v, run, 6, 0);
            return sse_mov_store(vm, v, run, &ea, reg, 4, n, ripmask, align);
        }

        case 0xD6:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            {
                uint32_t d[4] = {0, 0, 0, 0};

                if (ea.is_reg) {
                    uint32_t t[4];

                    vec_read(v, ea.reg, 4, t);
                    d[0] = t[0];
                    d[1] = t[1];
                } else {
                    uint8_t tmp[8];

                    rr = vfl_mem_read(vm, v, ea.addr, 8, tmp);
                    if (rr == -2) return vfl_mem_fault(v, run);
                    if (rr == 1) {
                        vec_write(v, reg, 4, d, 0);
                        return vfl_mmio_or_fault(vm, v, run, ea.addr, 8,
                                                 VFL_P_MMIO_XMM, reg, 0, 0, 0,
                                                 n, ripmask);
                    }
                    if (rr < 0) return vfl_exit_exception(v, run, 13, 0);
                    memcpy(d, tmp, 8);
                }
                vec_write(v, reg, 4, d, 0);
            }
            vfl_adv(v, n, ripmask);
            return 0;

        case 0xE7:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (ea.is_reg) return vfl_exit_exception(v, run, 6, 0);
            return sse_mov_store(vm, v, run, &ea, reg, 4, n, ripmask, 0);

        case 0x50:
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (mod != 3) return vfl_exit_exception(v, run, 6, 0);
            if (pp != 0 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            return sse_movmsk(v, reg, ea.reg, rex, pp == 0 ? 1 : 2, n, ripmask);

        case 0xD7:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            if (mod != 3) return vfl_exit_exception(v, run, 6, 0);
            return sse_movmsk(v, reg, ea.reg, rex, 0, n, ripmask);

        case 0xF0:
            if (pp != 2) return vfl_exit_exception(v, run, 6, 0);
            n = media_pre(v, buf, pos, asz, seg, rex_r, rex_b, rex_x, ripmask, 0,
                          &mod, &reg, &ea);
            if (n < 0) return vfl_exit_internal(v, run);
            return sse_mov_load(vm, v, run, &ea, reg, 4, n, ripmask, 0);

        default:
            break;
    }
    return vfl_exit_exception(v, run, 6, 0);
}

static int simd_38(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                   int *pos, int pp, int asz, int seg, int rex, int rex_r,
                   int rex_b, int rex_x, int lm, uint64_t ripmask) {
    uint8_t op3;
    int mod;
    int reg;
    int rm;
    vfl_ea_t ea;
    int n;
    int rr;
    int i;

    (void)lm;
    (void)rex;
    if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
    if (*pos + 1 >= 15) return vfl_exit_internal(v, run);
    (*pos)++;
    op3 = buf[*pos];
    (*pos)++;
    if (escape_modrm(v, run, buf, pos, rex_r, rex_b, &mod, &reg, &rm) < 0)
        return vfl_exit_internal(v, run);
    n = escape_ea(v, buf, pos, mod, rm, asz, seg, rex_b, rex_x, &ea,
                  v->regs.rip, ripmask, 0);
    if (n < 0) return vfl_exit_internal(v, run);
    switch (op3) {
        case 0x00: {
            uint32_t a[4];
            uint32_t b[4];
            uint32_t r[4];
            uint8_t *pa;
            uint8_t *pb;
            uint8_t *pr;

            vec_read(v, reg, 4, a);
            if (ea.is_reg) vec_read(v, ea.reg, 4, b);
            else {
                rr = sse_load_vec(vm, v, run, &ea, 4, b);
                if (rr) return 1;
            }
            pa = (uint8_t *)a;
            pb = (uint8_t *)b;
            pr = (uint8_t *)r;
            for (i = 0; i < 16; i++)
                pr[i] = (pb[i] & 0x80) ? 0 : pa[pb[i] & 0xF];
            vec_write(v, reg, 4, r, 0);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x01:
        case 0x02:
        case 0x05:
        case 0x06:
            return sse_hadd(vm, v, run, &ea, reg, 4, op3 == 0x05 || op3 == 0x06,
                            op3 == 0x02 || op3 == 0x06, n, ripmask);

        case 0x14: {
            uint32_t a[4];
            uint32_t b[4];
            uint32_t m[4];
            uint32_t r[4];
            uint8_t *pa;
            uint8_t *pb;
            uint8_t *pm;
            uint8_t *pr;

            vec_read(v, reg, 4, a);
            if (ea.is_reg) vec_read(v, ea.reg, 4, b);
            else {
                rr = sse_load_vec(vm, v, run, &ea, 4, b);
                if (rr) return 1;
            }
            vec_read(v, 0, 4, m);
            pa = (uint8_t *)a;
            pb = (uint8_t *)b;
            pm = (uint8_t *)m;
            pr = (uint8_t *)r;
            for (i = 0; i < 16; i++)
                pr[i] = (pm[i] & 0x80) ? pb[i] : pa[i];
            vec_write(v, reg, 4, r, 0);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x15: {
            uint32_t a[4];
            uint32_t b[4];
            uint32_t m[4];
            uint32_t r[4];
            int k;

            vec_read(v, reg, 4, a);
            if (ea.is_reg) vec_read(v, ea.reg, 4, b);
            else {
                rr = sse_load_vec(vm, v, run, &ea, 4, b);
                if (rr) return 1;
            }
            vec_read(v, 0, 4, m);
            for (k = 0; k < 2; k++) {
                uint64_t mk = (uint64_t)m[k * 2] | ((uint64_t)m[k * 2 + 1] << 32);

                if ((mk >> 63) & 1) {
                    r[k * 2] = b[k * 2];
                    r[k * 2 + 1] = b[k * 2 + 1];
                } else {
                    r[k * 2] = a[k * 2];
                    r[k * 2 + 1] = a[k * 2 + 1];
                }
            }
            vec_write(v, reg, 4, r, 0);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x17: {
            uint32_t a[4];
            uint32_t b[4];

            vec_read(v, reg, 4, a);
            if (ea.is_reg) vec_read(v, ea.reg, 4, b);
            else {
                rr = sse_load_vec(vm, v, run, &ea, 4, b);
                if (rr) return 1;
            }
            {
                uint64_t andv = 0;
                uint64_t nand = 0;

                for (i = 0; i < 2; i++) {
                    uint64_t x = (uint64_t)a[i * 2] | ((uint64_t)a[i * 2 + 1] << 32);
                    uint64_t y = (uint64_t)b[i * 2] | ((uint64_t)b[i * 2 + 1] << 32);

                    if ((x & y) == 0) andv |= 1ULL << i;
                    if ((~x & y) == 0) nand |= 1ULL << i;
                }
                if (andv == 3) v->regs.rflags |= (1ULL << 6);
                else v->regs.rflags &= ~(1ULL << 6);
                if (nand == 3) v->regs.rflags |= (1ULL << 0);
                else v->regs.rflags &= ~(1ULL << 0);
                v->regs.rflags &= ~((1ULL << 2) | (1ULL << 4) | (1ULL << 7) | (1ULL << 11));
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x1C:
        case 0x1D:
        case 0x1E: {
            uint32_t t[4];
            uint32_t r[4];

            if (ea.is_reg) vec_read(v, ea.reg, 4, t);
            else {
                rr = sse_load_vec(vm, v, run, &ea, 4, t);
                if (rr) return 1;
            }
            if (op3 == 0x1C) {
                int8_t *p = (int8_t *)t;
                int8_t *q = (int8_t *)r;

                for (i = 0; i < 16; i++)
                    q[i] = p[i] == -128 ? p[i] : (p[i] < 0 ? -p[i] : p[i]);
            } else if (op3 == 0x1D) {
                int16_t *p = (int16_t *)t;
                int16_t *q = (int16_t *)r;

                for (i = 0; i < 8; i++)
                    q[i] = p[i] == -32768 ? p[i] : (p[i] < 0 ? -p[i] : p[i]);
            } else {
                int32_t *p = (int32_t *)t;
                int32_t *q = (int32_t *)r;

                for (i = 0; i < 4; i++)
                    q[i] = p[i] == (int32_t)0x80000000 ? p[i] : (p[i] < 0 ? -p[i] : p[i]);
            }
            vec_write(v, reg, 4, r, 0);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x20:
        case 0x21:
        case 0x22:
        case 0x23:
        case 0x24:
        case 0x25:
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x33:
        case 0x34:
        case 0x35: {
            uint32_t t[4];
            uint32_t r[4];
            int ssz;
            int dsz;
            int sgn = op3 < 0x30;

            if (ea.is_reg) vec_read(v, ea.reg, 4, t);
            else {
                rr = sse_load_vec(vm, v, run, &ea, 4, t);
                if (rr) return 1;
            }
            ssz = op3 == 0x20 || op3 == 0x30 ? 1 : (op3 == 0x21 || op3 == 0x31 || op3 == 0x23 || op3 == 0x33 ? 2 : 4);
            if (op3 == 0x22 || op3 == 0x32) { ssz = 1; dsz = 8; }
            else if (op3 == 0x24 || op3 == 0x34) { ssz = 2; dsz = 8; }
            else if (op3 == 0x25 || op3 == 0x35) { ssz = 4; dsz = 8; }
            else if (ssz == 1) dsz = 2;
            else dsz = 4;
            memset(r, 0, sizeof(r));
            if (ssz == 1 && dsz == 2) {
                uint8_t *p = (uint8_t *)t;
                uint16_t *q = (uint16_t *)r;

                for (i = 0; i < 8; i++)
                    q[i] = sgn ? (uint16_t)(int16_t)(int8_t)p[i] : p[i];
            } else if (ssz == 1 && dsz == 4) {
                uint8_t *p = (uint8_t *)t;
                uint32_t *q = r;

                for (i = 0; i < 4; i++)
                    q[i] = sgn ? (uint32_t)(int32_t)(int8_t)p[i] : p[i];
            } else if (ssz == 1) {
                uint8_t *p = (uint8_t *)t;
                uint64_t *q = (uint64_t *)r;

                for (i = 0; i < 2; i++)
                    q[i] = sgn ? (uint64_t)(int64_t)(int8_t)p[i] : p[i];
            } else if (ssz == 2 && dsz == 4) {
                uint16_t *p = (uint16_t *)t;
                uint32_t *q = r;

                for (i = 0; i < 4; i++)
                    q[i] = sgn ? (uint32_t)(int32_t)(int16_t)p[i] : p[i];
            } else if (ssz == 2) {
                uint16_t *p = (uint16_t *)t;
                uint64_t *q = (uint64_t *)r;

                for (i = 0; i < 2; i++)
                    q[i] = sgn ? (uint64_t)(int64_t)(int16_t)p[i] : p[i];
            } else {
                uint32_t *p = t;
                uint64_t *q = (uint64_t *)r;

                for (i = 0; i < 2; i++)
                    q[i] = sgn ? (uint64_t)(int64_t)(int32_t)p[i] : p[i];
            }
            vec_write(v, reg, 4, r, 0);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x28: {
            uint32_t t[4];
            uint32_t r[4];

            if (ea.is_reg) vec_read(v, ea.reg, 4, t);
            else {
                rr = sse_load_vec(vm, v, run, &ea, 4, t);
                if (rr) return 1;
            }
            for (i = 0; i < 2; i++) {
                int64_t x = (int64_t)(int32_t)t[i * 2];
                int64_t y = (int64_t)(int32_t)t[i * 2 + 1];
                __int128_t p = (__int128_t)x * y;

                r[i * 2] = (uint32_t)(uint64_t)p;
                r[i * 2 + 1] = (uint32_t)((uint64_t)p >> 32);
            }
            vec_write(v, reg, 4, r, 0);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x29:
            return sse_intop(vm, v, run, &ea, reg, 4, 3, 2, n, ripmask);

        case 0x2A:
            if (ea.is_reg) return vfl_exit_exception(v, run, 6, 0);
            return sse_move_load(vm, v, run, &ea, reg, 4, n, ripmask, 16, 0);

        case 0x2B:
            return sse_pack(vm, v, run, &ea, reg, 4, 3, n, ripmask);

        case 0x37:
            return sse_intop(vm, v, run, &ea, reg, 4, 3, 3, n, ripmask);

        case 0x38:
        case 0x39:
        case 0x3A:
        case 0x3B:
        case 0x3C:
        case 0x3D:
        case 0x3E:
        case 0x3F: {
            int el;
            int which;

            if (op3 == 0x38) { el = 0; which = 9; }
            else if (op3 == 0x39) { el = 2; which = 9; }
            else if (op3 == 0x3A) { el = 1; which = 7; }
            else if (op3 == 0x3B) { el = 2; which = 7; }
            else if (op3 == 0x3C) { el = 0; which = 10; }
            else if (op3 == 0x3D) { el = 2; which = 10; }
            else if (op3 == 0x3E) { el = 1; which = 8; }
            else { el = 2; which = 8; }
            return sse_intop(vm, v, run, &ea, reg, 4, el, which, n, ripmask);
        }

        case 0x40: {
            uint32_t t[4];
            uint32_t r[4];

            if (ea.is_reg) vec_read(v, ea.reg, 4, t);
            else {
                rr = sse_load_vec(vm, v, run, &ea, 4, t);
                if (rr) return 1;
            }
            {
                uint32_t a[4];

                vec_read(v, reg, 4, a);
                for (i = 0; i < 4; i++)
                    r[i] = a[i] * t[i];
            }
            vec_write(v, reg, 4, r, 0);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x41: {
            uint32_t t[4];
            uint32_t best = 0xFFFFu;
            int idx = 0;

            if (ea.is_reg) vec_read(v, ea.reg, 4, t);
            else {
                rr = sse_load_vec(vm, v, run, &ea, 4, t);
                if (rr) return 1;
            }
            for (i = 0; i < 8; i++) {
                uint16_t w = (uint16_t)(t[i >> 1] >> ((i & 1) * 16));

                if (w < best) {
                    best = w;
                    idx = i;
                }
            }
            {
                uint32_t d[4] = {0, 0, 0, 0};

                d[0] = best | ((uint32_t)idx << 16);
                vec_write(v, reg, 4, d, 0);
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x13: {
            uint32_t t[8];
            uint32_t r[8];
            int k;

            if (ea.is_reg) vec_read(v, ea.reg, 4, t);
            else {
                rr = sse_load_vec(vm, v, run, &ea, 4, t);
                if (rr) return 1;
            }
            for (k = 0; k < 4; k++) {
                uint16_t hw = (uint16_t)(t[k / 2] >> ((k & 1) * 16));
                uint16_t exp = (hw >> 10) & 31;
                uint16_t mant = hw & 1023;
                float f;

                if (exp == 0) {
                    f = (float)mant * 5.9604644775390625e-8f;
                    if (hw & 0x8000) f = -f;
                } else if (exp == 31) {
                    uint32_t u = ((uint32_t)(hw & 0x8000) << 16);

                    if (mant) u |= 0x7FC00000u;
                    else u |= 0x7F800000u;
                    memcpy(&f, &u, 4);
                } else {
                    uint32_t u = ((uint32_t)(hw & 0x8000) << 16) |
                                 ((uint32_t)(exp + 112) << 23) | ((uint32_t)mant << 13);

                    memcpy(&f, &u, 4);
                }
                memcpy(&r[k], &f, 4);
            }
            vec_write(v, reg, 4, r, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        default:
            break;
    }
    return vfl_exit_exception(v, run, 6, 0);
}

static int simd_3a(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                   int *pos, int pp, int asz, int seg, int rex, int rex_r,
                   int rex_b, int rex_x, int lm, uint64_t ripmask) {
    uint8_t op3;
    int mod;
    int reg;
    int rm;
    vfl_ea_t ea;
    int n;
    int rr;
    int i;
    int imm;

    (void)lm;
    (void)rex;
    if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
    if (*pos + 1 >= 15) return vfl_exit_internal(v, run);
    (*pos)++;
    op3 = buf[*pos];
    (*pos)++;
    if (escape_modrm(v, run, buf, pos, rex_r, rex_b, &mod, &reg, &rm) < 0)
        return vfl_exit_internal(v, run);
    n = escape_ea(v, buf, pos, mod, rm, asz, seg, rex_b, rex_x, &ea,
                  v->regs.rip, ripmask, 1);
    if (n < 0) return vfl_exit_internal(v, run);
    imm = buf[n - 1];
    switch (op3) {
        case 0x0E: {
            uint32_t a[4];
            uint32_t b[4];
            uint32_t r[4];
            uint16_t *pa;
            uint16_t *pb;
            uint16_t *pr;

            vec_read(v, reg, 4, a);
            if (ea.is_reg) vec_read(v, ea.reg, 4, b);
            else {
                rr = sse_load_vec(vm, v, run, &ea, 4, b);
                if (rr) return 1;
            }
            pa = (uint16_t *)a;
            pb = (uint16_t *)b;
            pr = (uint16_t *)r;
            for (i = 0; i < 8; i++)
                pr[i] = (imm >> i) & 1 ? pb[i] : pa[i];
            vec_write(v, reg, 4, r, 0);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x0F: {
            uint32_t a[4];
            uint32_t b[4];
            uint32_t r[4];
            uint8_t *pa;
            uint8_t *pb;
            uint8_t *pr;
            int sh = imm & 31;

            vec_read(v, reg, 4, a);
            if (ea.is_reg) vec_read(v, ea.reg, 4, b);
            else {
                rr = sse_load_vec(vm, v, run, &ea, 4, b);
                if (rr) return 1;
            }
            pa = (uint8_t *)a;
            pb = (uint8_t *)b;
            pr = (uint8_t *)r;
            for (i = 0; i < 16; i++) {
                int s = i + sh;

                pr[i] = s < 16 ? pb[s] : (s < 32 ? pa[s - 16] : 0);
            }
            vec_write(v, reg, 4, r, 0);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x14: {
            uint32_t t[4];
            uint8_t val;

            if (ea.is_reg) vec_read(v, ea.reg, 4, t);
            else {
                rr = sse_load_vec(vm, v, run, &ea, 4, t);
                if (rr) return 1;
            }
            val = ((uint8_t *)t)[imm & 15];
            if (mod == 3) {
                vfl_write_reg(v, reg, rex, 4, val);
            } else {
                rr = vfl_mem_write(vm, v, ea.addr, 1, &val);
                if (rr) {
                    vfl_memerr(v, run, rr);
                    return 1;
                }
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x16: {
            uint32_t t[4];
            uint64_t val = 0;

            if (ea.is_reg) vec_read(v, ea.reg, 4, t);
            else {
                rr = sse_load_vec(vm, v, run, &ea, 4, t);
                if (rr) return 1;
            }
            if (rex) {
                int lane = imm & 1;

                val = (uint64_t)t[lane * 2] | ((uint64_t)t[lane * 2 + 1] << 32);
            } else {
                val = t[imm & 3];
            }
            if (mod == 3) {
                vfl_write_reg(v, reg, rex, rex ? 8 : 4, val);
            } else {
                uint8_t wb[8];
                int sz = rex ? 8 : 4;

                vfl_put64(wb, val, sz);
                rr = vfl_mem_write(vm, v, ea.addr, (uint64_t)sz, wb);
                if (rr) {
                    vfl_memerr(v, run, rr);
                    return 1;
                }
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x17: {
            uint32_t t[4];
            uint32_t val;

            if (ea.is_reg) vec_read(v, ea.reg, 4, t);
            else {
                rr = sse_load_vec(vm, v, run, &ea, 4, t);
                if (rr) return 1;
            }
            val = t[imm & 3];
            if (mod == 3) {
                vfl_write_reg(v, reg, rex, 4, val);
            } else {
                uint8_t wb[4];

                vfl_put64(wb, val, 4);
                rr = vfl_mem_write(vm, v, ea.addr, 4, wb);
                if (rr) {
                    vfl_memerr(v, run, rr);
                    return 1;
                }
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x20: {
            uint8_t src = 0;

            if (ea.is_reg) src = (uint8_t)vfl_read_reg(v, ea.reg, rex, 4);
            else {
                rr = vfl_mem_read(vm, v, ea.addr, 1, &src);
                if (rr == -2) {
                    vfl_mem_fault(v, run);
                    return 1;
                }
                if (rr == 1) return vfl_mmio_fill_exit(vm, v, run, ea.addr, 1);
                if (rr < 0) {
                    vfl_exit_exception(v, run, 13, 0);
                    return 1;
                }
            }
            {
                uint32_t d[4];

                vec_read(v, reg, 4, d);
                ((uint8_t *)d)[imm & 15] = src;
                vec_write(v, reg, 4, d, 0);
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x22: {
            uint64_t gv;

            if (ea.is_reg) gv = vfl_read_reg(v, ea.reg, rex, rex ? 8 : 4);
            else {
                uint8_t tmp[8] = {0, 0, 0, 0, 0, 0, 0, 0};

                if (sse_mem_fill(vm, v, run, ea.addr, tmp, rex ? 8 : 4)) return 1;
                memcpy(&gv, tmp, rex ? 8 : 4);
            }
            {
                uint32_t d[4];

                vec_read(v, reg, 4, d);
                if (rex) {
                    int lane = imm & 1;

                    d[lane * 2] = (uint32_t)gv;
                    d[lane * 2 + 1] = (uint32_t)(gv >> 32);
                } else {
                    d[imm & 3] = (uint32_t)gv;
                }
                vec_write(v, reg, 4, d, 0);
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x21: {
            uint32_t d[4];
            uint32_t s[4];
            int sc = (imm >> 6) & 3;
            int dc = (imm >> 4) & 3;
            int zm = imm & 15;

            vec_read(v, reg, 4, d);
            if (ea.is_reg) vec_read(v, ea.reg, 4, s);
            else {
                rr = sse_load_vec(vm, v, run, &ea, 4, s);
                if (rr) return 1;
            }
            d[dc] = s[sc];
            for (i = 0; i < 4; i++)
                if ((zm >> i) & 1) d[i] = 0;
            vec_write(v, reg, 4, d, 0);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x40:
        case 0x41: {
            uint32_t a[4];
            uint32_t b[8];
            double dot = 0;
            float dotf = 0;
            vec_read(v, reg, 4, a);
            if (ea.is_reg) vec_read(v, ea.reg, 4, b);
            else {
                rr = sse_load_vec(vm, v, run, &ea, 4, b);
                if (rr) return 1;
            }
            if (op3 == 0x40) {
                for (i = 0; i < 4; i++)
                    if ((imm >> (4 + i)) & 1) dotf += vfl_u32f(a[i]) * vfl_u32f(b[i]);
                for (i = 0; i < 4; i++)
                    a[i] = (imm >> i) & 1 ? fu32(dotf) : 0;
            } else {
                for (i = 0; i < 2; i++) {
                    uint64_t au = (uint64_t)a[i * 2] | ((uint64_t)a[i * 2 + 1] << 32);
                    uint64_t bu = (uint64_t)b[i * 2] | ((uint64_t)b[i * 2 + 1] << 32);

                    if ((imm >> (4 + i)) & 1) dot += vfl_u64d(au) * vfl_u64d(bu);
                }
                for (i = 0; i < 2; i++) {
                    uint64_t o = 0;

                    if ((imm >> i) & 1) {
                        memcpy(&o, &dot, 8);
                    }
                    a[i * 2] = (uint32_t)o;
                    a[i * 2 + 1] = (uint32_t)(o >> 32);
                }
            }
            vec_write(v, reg, 4, a, 0);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x44: {
            uint32_t a[4];
            uint32_t b[4];
            uint64_t x;
            uint64_t y;
            __uint128_t p0;
            __uint128_t p1;
            uint32_t r[4];

            vec_read(v, reg, 4, a);
            if (ea.is_reg) vec_read(v, ea.reg, 4, b);
            else {
                rr = sse_load_vec(vm, v, run, &ea, 4, b);
                if (rr) return 1;
            }
            x = (uint64_t)a[0] | ((uint64_t)a[1] << 32);
            y = (uint64_t)b[0] | ((uint64_t)b[1] << 32);
            if (imm & 1) x >>= 32;
            if (imm & 2) y >>= 32;
            p0 = 0;
            p1 = 0;
            {
                uint64_t xx = x;
                uint64_t yy = y;
                int k;

                for (k = 0; k < 64; k++) {
                    if ((yy >> k) & 1) {
                        __uint128_t t = (__uint128_t)xx << k;

                        p0 ^= (uint64_t)t;
                        p1 ^= (uint64_t)(t >> 64);
                    }
                }
            }
            r[0] = (uint32_t)p0;
            r[1] = (uint32_t)(p0 >> 32);
            r[2] = (uint32_t)p1;
            r[3] = (uint32_t)(p1 >> 32);
            vec_write(v, reg, 4, r, 0);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        default:
            break;
    }
    return vfl_exit_exception(v, run, 6, 0);
}

static int x87_mem_read(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run,
                        uint64_t addr, uint64_t len, uint8_t *out) {
    int rr = vfl_mem_read(vm, v, addr, len, out);

    if (rr == -2) {
        vfl_mem_fault(v, run);
        return 1;
    }
    if (rr == 1) return vfl_mmio_fill_exit(vm, v, run, addr, (int)len);
    if (rr < 0) {
        vfl_exit_exception(v, run, 13, 0);
        return 1;
    }
    return 0;
}

static int x87_mem_write(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run,
                         uint64_t addr, uint64_t len, uint8_t *in) {
    int rr = vfl_mem_write(vm, v, addr, len, in);

    if (rr) {
        vfl_memerr(v, run, rr);
        return 1;
    }
    return 0;
}

static void fpu_compare(vfl_vcpu_t *v, long double a, long double b) {
    v->fpu.fsw &= ~((1 << 8) | (1 << 9) | (1 << 10) | (1 << 14));
    if (a != a || b != b) {
        v->fpu.fsw |= (1 << 8) | (1 << 10) | (1 << 14);
    } else if (a > b) {
    } else if (a < b) {
        v->fpu.fsw |= (1 << 8);
    } else {
        v->fpu.fsw |= (1 << 14);
    }
}

static void fpu_eflags(vfl_vcpu_t *v, long double a, long double b) {
    v->regs.rflags &= ~((1ULL << 0) | (1ULL << 2) | (1ULL << 6) |
                        (1ULL << 7) | (1ULL << 11) | (1ULL << 4));
    if (a != a || b != b) {
        v->regs.rflags |= (1ULL << 0) | (1ULL << 2) | (1ULL << 6);
    } else if (a == b) {
        v->regs.rflags |= (1ULL << 6);
    } else if (a < b) {
        v->regs.rflags |= (1ULL << 0);
    }
}

static __attribute__((always_inline)) inline long double x87_load_int(uint8_t *b, int size) {
    if (size == 2) {
        int16_t x = 0;

        memcpy(&x, b, 2);
        return (long double)x;
    }
    if (size == 4) {
        int32_t x = 0;

        memcpy(&x, b, 4);
        return (long double)x;
    }
    {
        int64_t x = 0;

        memcpy(&x, b, 8);
        return (long double)x;
    }
}

static int x87_store_int(long double x, uint8_t *b, int size, int trunc) {
    long double t = trunc ? x : f80_round(x);

    if (size == 2) {
        int16_t o;

        if (!(t > -32768.0L) || !(t < 32767.0L)) return -1;
        o = (int16_t)t;
        memcpy(b, &o, 2);
        return 0;
    }
    if (size == 4) {
        int32_t o;

        if (!(t > -2147483648.0L) || !(t < 2147483647.0L)) return -1;
        o = (int32_t)t;
        memcpy(b, &o, 4);
        return 0;
    }
    {
        int64_t o;

        if (!(t > -9.223372036854776e18L) || !(t < 9.223372036854776e18L)) return -1;
        o = (int64_t)t;
        memcpy(b, &o, 8);
        return 0;
    }
}

static int x87_bld(vfl_vcpu_t *v, uint8_t *b) {
    long double val = 0;
    int i;

    for (i = 8; i >= 0; i--) {
        unsigned d0 = b[i] & 0xF;
        unsigned d1 = (b[i] >> 4) & 0xF;

        if (d0 > 9 || d1 > 9) {
            v->fpu.fsw |= 1;
            return -1;
        }
        val = val * 100.0L + (long double)(d1 * 10 + d0);
    }
    if (b[9] & 0x80) val = -val;
    return fpu_push(v, val);
}

static int x87_bstp(vfl_vcpu_t *v, uint8_t *b) {
    long double x;
    long double t;
    int neg = 0;
    int i;

    if (fpu_pop(v, &x)) return -1;
    t = f80_round(x);
    if (t < 0) {
        neg = 1;
        t = -t;
    }
    if (!(t < 1e18L)) {
        v->fpu.fsw |= 1;
        memset(b, 0, 10);
        return 0;
    }
    for (i = 0; i < 9; i++) {
        long double q = t / 100.0L;
        long double qi = f80_round(q - 0.5L);
        unsigned pair;

        if (qi < 0) qi = 0;
        pair = (unsigned)(t - qi * 100.0L);
        t = qi;
        b[i] = (uint8_t)(((pair / 10) << 4) | (pair % 10));
    }
    b[9] = neg ? 0x80 : 0;
    return 0;
}

static int x87_env(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint64_t addr,
                   int n, uint64_t ripmask, int osz, int save) {
    int len = osz == 2 ? 14 : 28;
    uint8_t e[28];
    int ab = 0;
    int i;

    for (i = 0; i < 8; i++)
        if (((v->fpu.ftw >> (i * 2)) & 3) != 3) ab |= 1 << i;
    if (save) {
        memset(e, 0, (size_t)len);
        memcpy(e, &v->fpu.fcw, 2);
        memcpy(e + (osz == 2 ? 2 : 4), &v->fpu.fsw, 2);
        e[osz == 2 ? 4 : 8] = (uint8_t)ab;
        memcpy(e + (osz == 2 ? 6 : 12), &v->fpu.fop, 2);
        if (osz == 2) {
            memcpy(e + 8, &v->fpu.fip, 2);
            memcpy(e + 10, &v->fpu.fcs, 2);
            memcpy(e + 12, &v->fpu.fdp, 2);
        } else {
            memcpy(e + 14, &v->fpu.fip, 4);
            memcpy(e + 18, &v->fpu.fcs, 2);
            memcpy(e + 20, &v->fpu.fdp, 4);
            memcpy(e + 24, &v->fpu.fds, 2);
        }
        if (x87_mem_write(vm, v, run, addr, (uint64_t)len, e)) return 1;
    } else {
        if (x87_mem_read(vm, v, run, addr, (uint64_t)len, e)) return 1;
        memcpy(&v->fpu.fcw, e, 2);
        v->fpu.fcw |= 0x40;
        memcpy(&v->fpu.fsw, e + (osz == 2 ? 2 : 4), 2);
        v->fpu.ftw = 0;
        {
            uint8_t b = e[osz == 2 ? 4 : 8];

            for (i = 0; i < 8; i++)
                v->fpu.ftw |= (uint16_t)(((b >> i) & 1 ? 0 : 3) << (i * 2));
        }
        memcpy(&v->fpu.fop, e + (osz == 2 ? 6 : 12), 2);
        if (osz == 2) {
            uint16_t w;

            memcpy(&w, e + 8, 2);
            v->fpu.fip = w;
            memcpy(&v->fpu.fcs, e + 10, 2);
            memcpy(&w, e + 12, 2);
            v->fpu.fdp = w;
        } else {
            memcpy(&v->fpu.fip, e + 14, 4);
            memcpy(&v->fpu.fcs, e + 18, 2);
            memcpy(&v->fpu.fdp, e + 20, 4);
            memcpy(&v->fpu.fds, e + 24, 2);
        }
    }
    vfl_adv(v, n, ripmask);
    return 0;
}

static int x87_save(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint64_t addr,
                    int n, uint64_t ripmask, int osz, int save) {
    int envlen = osz == 2 ? 14 : 28;
    uint8_t e[108];
    int i;

    if (save) {
        int ab = 0;

        for (i = 0; i < 8; i++)
            if (((v->fpu.ftw >> (i * 2)) & 3) != 3) ab |= 1 << i;
        memset(e, 0, sizeof(e));
        memcpy(e, &v->fpu.fcw, 2);
        memcpy(e + (osz == 2 ? 2 : 4), &v->fpu.fsw, 2);
        e[osz == 2 ? 4 : 8] = (uint8_t)ab;
        for (i = 0; i < 8; i++) {
            int r = (fpu_top(v) + i) & 7;

            memcpy(e + envlen + i * 10, &v->fpu.fpr_m[r], 8);
            memcpy(e + envlen + i * 10 + 8, &v->fpu.fpr_e[r], 2);
        }
        if (x87_mem_write(vm, v, run, addr, (uint64_t)(envlen + 80), e)) return 1;
        fpu_init(v);
    } else {
        if (x87_mem_read(vm, v, run, addr, (uint64_t)(envlen + 80), e)) return 1;
        memcpy(&v->fpu.fcw, e, 2);
        v->fpu.fcw |= 0x40;
        memcpy(&v->fpu.fsw, e + (osz == 2 ? 2 : 4), 2);
        v->fpu.ftw = 0;
        {
            uint8_t b = e[osz == 2 ? 4 : 8];

            for (i = 0; i < 8; i++)
                v->fpu.ftw |= (uint16_t)(((b >> i) & 1 ? 0 : 3) << (i * 2));
        }
        for (i = 0; i < 8; i++) {
            memcpy(&v->fpu.fpr_m[i], e + envlen + i * 10, 8);
            memcpy(&v->fpu.fpr_e[i], e + envlen + i * 10 + 8, 2);
        }
    }
    vfl_adv(v, n, ripmask);
    return 0;
}

int vfl_x87_step(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf, int *pos,
                 int osz, int asz, int seg, int rex, int rex_r, int rex_b,
                 int lm, uint64_t ripmask, uint8_t op) {
    int mod;
    int reg;
    int rm;
    vfl_ea_t ea;
    int n;
    uint8_t m;

    (void)rex;
    (void)rex_r;
    (void)rex_b;
    (void)lm;
    if (*pos >= 15) return vfl_exit_internal(v, run);
    m = buf[*pos];
    (*pos)++;
    mod = (m >> 6) & 3;
    reg = (m >> 3) & 7;
    rm = m & 7;
    if (mod != 3) {
        if (vfl_decode_ea(v, buf, pos, mod, rm, asz, seg, 0, 0, &ea) < 0)
            return vfl_exit_internal(v, run);
        n = *pos;
        if (ea.rip_rel)
            ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
        switch (op) {
            case 0xD8:
            case 0xDC: {
                int dbl = op == 0xDC;
                uint8_t tb[8];
                long double b;
                long double a = fpu_get(v, 0);

                if (x87_mem_read(vm, v, run, ea.addr, dbl ? 8 : 4, tb)) return 1;
                if (dbl) {
                    double f = 0;

                    memcpy(&f, tb, 8);
                    b = (long double)f;
                } else {
                    float f = 0;

                    memcpy(&f, tb, 4);
                    b = (long double)f;
                }
                if (reg == 2) fpu_compare(v, a, b);
                else if (reg == 3) {
                    fpu_compare(v, a, b);
                    fpu_pop(v, 0);
                } else if (reg == 0) fpu_set(v, 0, a + b);
                else if (reg == 1) fpu_set(v, 0, a * b);
                else if (reg == 4) fpu_set(v, 0, a - b);
                else if (reg == 5) fpu_set(v, 0, b - a);
                else if (reg == 6) {
                    if (b == 0.0L) {
                        v->fpu.fsw |= 4;
                        fpu_set(v, 0, a < 0 ? -1.0L / 0.0L : 1.0L / 0.0L);
                    } else {
                        fpu_set(v, 0, a / b);
                    }
                } else {
                    if (a == 0.0L) {
                        v->fpu.fsw |= 4;
                        fpu_set(v, 0, b < 0 ? -1.0L / 0.0L : 1.0L / 0.0L);
                    } else {
                        fpu_set(v, 0, b / a);
                    }
                }
                vfl_adv(v, n, ripmask);
                return 0;
            }

            case 0xD9:
                if (reg == 0) {
                    uint8_t tb[4];
                    float f;

                    if (x87_mem_read(vm, v, run, ea.addr, 4, tb)) return 1;
                    memcpy(&f, tb, 4);
                    if (fpu_push(v, (long double)f)) return vfl_exit_exception(v, run, 7, 0);
                } else if (reg == 2 || reg == 3) {
                    int empty = fpu_tag(v, fpu_top(v)) == 3;
                    long double a = empty ? 0.0L : fpu_get(v, 0);

                    if (empty) {
                        v->fpu.fsw |= 1;
                        if (reg == 3) return vfl_exit_exception(v, run, 7, 0);
                    }
                    {
                        float f = (float)a;
                        uint8_t tb[4];

                        memcpy(tb, &f, 4);
                        if (x87_mem_write(vm, v, run, ea.addr, 4, tb)) return 1;
                    }
                    if (reg == 3) fpu_pop(v, 0);
                } else if (reg == 4) {
                    return x87_env(vm, v, run, ea.addr, n, ripmask, osz, 0);
                } else if (reg == 5) {
                    uint8_t tb[2];

                    if (x87_mem_read(vm, v, run, ea.addr, 2, tb)) return 1;
                    memcpy(&v->fpu.fcw, tb, 2);
                    v->fpu.fcw |= 0x40;
                    vfl_adv(v, n, ripmask);
                    return 0;
                } else if (reg == 6) {
                    return x87_env(vm, v, run, ea.addr, n, ripmask, osz, 1);
                } else if (reg == 7) {
                    uint8_t tb[2];

                    memcpy(tb, &v->fpu.fcw, 2);
                    if (x87_mem_write(vm, v, run, ea.addr, 2, tb)) return 1;
                    vfl_adv(v, n, ripmask);
                    return 0;
                } else {
                    return vfl_exit_exception(v, run, 6, 0);
                }
                vfl_adv(v, n, ripmask);
                return 0;

            case 0xDA:
            case 0xDE: {
                int w16 = op == 0xDE;
                uint8_t tb[2];
                long double b;
                long double a = fpu_get(v, 0);

                if (reg == 4) return vfl_exit_exception(v, run, 6, 0);
                if (x87_mem_read(vm, v, run, ea.addr, w16 ? 2 : 4, tb)) return 1;
                b = x87_load_int(tb, w16 ? 2 : 4);
                if (reg == 0) fpu_set(v, 0, a + b);
                else if (reg == 1) fpu_set(v, 0, a * b);
                else if (reg == 2) fpu_compare(v, a, b);
                else if (reg == 3) {
                    fpu_compare(v, a, b);
                    fpu_pop(v, 0);
                } else if (reg == 5) fpu_set(v, 0, a - b);
                else if (reg == 6) fpu_set(v, 0, b - a);
                else fpu_set(v, 0, a / b);
                vfl_adv(v, n, ripmask);
                return 0;
            }

            case 0xDB: {
                uint8_t tb[10];

                if (reg == 0 || reg == 2 || reg == 3) {
                    uint8_t ib[4];

                    if (x87_mem_read(vm, v, run, ea.addr, 4, ib)) return 1;
                    if (reg == 0) {
                        if (fpu_push(v, x87_load_int(ib, 4)))
                            return vfl_exit_exception(v, run, 7, 0);
                    } else if (reg == 2 || reg == 3) {
                        long double a;

                        if (fpu_pop(v, &a)) {
                            if (reg == 3) return vfl_exit_exception(v, run, 7, 0);
                        }
                        if (reg == 2) {
                            if (x87_store_int(a, ib, 4, 0)) {
                                v->fpu.fsw |= 1;
                                ib[0] = ib[1] = ib[2] = ib[3] = 0;
                                ib[3] = 0x80;
                            }
                            if (x87_mem_write(vm, v, run, ea.addr, 4, ib)) return 1;
                            fpu_push(v, a);
                        } else {
                            if (x87_store_int(a, ib, 4, 0)) {
                                v->fpu.fsw |= 1;
                                ib[0] = ib[1] = ib[2] = ib[3] = 0;
                                ib[3] = 0x80;
                            }
                            if (x87_mem_write(vm, v, run, ea.addr, 4, ib)) return 1;
                        }
                    }
                } else if (reg == 1) {
                    uint8_t ib[4];

                    if (x87_mem_read(vm, v, run, ea.addr, 4, ib)) return 1;
                    {
                        long double a;

                        if (fpu_pop(v, &a)) return vfl_exit_exception(v, run, 7, 0);
                        if (x87_store_int(a, ib, 4, 1)) {
                            v->fpu.fsw |= 1;
                            ib[0] = ib[1] = ib[2] = ib[3] = 0;
                            ib[3] = 0x80;
                        }
                        if (x87_mem_write(vm, v, run, ea.addr, 4, ib)) return 1;
                    }
                } else if (reg == 5) {
                    if (x87_mem_read(vm, v, run, ea.addr, 10, tb)) return 1;
                    {
                        long double x = f80_to_ld(vfl_get64(tb), vfl_get16(tb + 8));

                        if (fpu_push(v, x)) return vfl_exit_exception(v, run, 7, 0);
                    }
                } else if (reg == 7) {
                    long double a;

                    if (fpu_pop(v, &a)) return vfl_exit_exception(v, run, 7, 0);
                    ld_to_f80(a, (uint64_t *)tb, (uint16_t *)(tb + 8));
                    if (x87_mem_write(vm, v, run, ea.addr, 10, tb)) return 1;
                } else {
                    return vfl_exit_exception(v, run, 6, 0);
                }
                vfl_adv(v, n, ripmask);
                return 0;
            }

            case 0xDD: {
                if (reg == 0) {
                    uint8_t db[8];
                    double f = 0;

                    if (x87_mem_read(vm, v, run, ea.addr, 8, db)) return 1;
                    memcpy(&f, db, 8);
                    if (fpu_push(v, (long double)f))
                        return vfl_exit_exception(v, run, 7, 0);
                } else if (reg == 2 || reg == 3) {
                    uint8_t db[8];
                    int empty = fpu_tag(v, fpu_top(v)) == 3;
                    long double a = empty ? 0.0L : fpu_get(v, 0);

                        if (empty) {
                            v->fpu.fsw |= 1;
                            if (reg == 3) return vfl_exit_exception(v, run, 7, 0);
                        }
                        {
                            double o = (double)a;

                            memcpy(db, &o, 8);
                            if (x87_mem_write(vm, v, run, ea.addr, 8, db)) return 1;
                        }
                        if (reg == 3) fpu_pop(v, 0);
                } else if (reg == 1) {
                    long double a;
                    uint8_t db[8];

                    if (fpu_tag(v, fpu_top(v)) == 3)
                        return vfl_exit_exception(v, run, 7, 0);
                    a = fpu_get(v, 0);
                    fpu_pop(v, 0);
                    if (x87_store_int(a, db, 8, 1)) {
                        v->fpu.fsw |= 1;
                        memset(db, 0, 8);
                        db[7] = 0x80;
                    }
                    if (x87_mem_write(vm, v, run, ea.addr, 8, db)) return 1;
                } else if (reg == 4) {
                    return x87_save(vm, v, run, ea.addr, n, ripmask, osz, 0);
                } else if (reg == 5) {
                    uint8_t db[8];
                    double f = 0;
                    long double a = fpu_get(v, 0);

                    if (x87_mem_read(vm, v, run, ea.addr, 8, db)) return 1;
                    memcpy(&f, db, 8);
                    fpu_compare(v, a, (long double)f);
                    fpu_pop(v, 0);
                } else if (reg == 6) {
                    return x87_save(vm, v, run, ea.addr, n, ripmask, osz, 1);
                } else if (reg == 7) {
                    uint8_t wb[2];

                    memcpy(wb, &v->fpu.fsw, 2);
                    if (x87_mem_write(vm, v, run, ea.addr, 2, wb)) return 1;
                } else {
                    return vfl_exit_exception(v, run, 6, 0);
                }
                vfl_adv(v, n, ripmask);
                return 0;
            }

            case 0xDF: {
                uint8_t tb[10];

                if (reg == 0 || reg == 5) {
                    uint8_t ib[8];

                    if (x87_mem_read(vm, v, run, ea.addr, reg == 0 ? 2 : 8, ib)) return 1;
                    if (fpu_push(v, x87_load_int(ib, reg == 0 ? 2 : 8)))
                        return vfl_exit_exception(v, run, 7, 0);
                } else if (reg == 1) {
                    long double a;

                    if (fpu_pop(v, &a)) return vfl_exit_exception(v, run, 7, 0);
                    if (x87_store_int(a, tb, 2, 1)) {
                        v->fpu.fsw |= 1;
                        tb[0] = tb[1] = 0;
                        tb[1] = 0x80;
                    }
                    if (x87_mem_write(vm, v, run, ea.addr, 2, tb)) return 1;
                } else if (reg == 2 || reg == 3) {
                    long double a;

                    if (fpu_pop(v, &a)) {
                        if (reg == 3) return vfl_exit_exception(v, run, 7, 0);
                    }
                    if (reg == 2) {
                        if (x87_store_int(a, tb, 2, 0)) {
                            v->fpu.fsw |= 1;
                            tb[0] = tb[1] = 0;
                            tb[1] = 0x80;
                        }
                        if (x87_mem_write(vm, v, run, ea.addr, 2, tb)) return 1;
                        fpu_push(v, a);
                    } else {
                        if (x87_store_int(a, tb, 2, 0)) {
                            v->fpu.fsw |= 1;
                            tb[0] = tb[1] = 0;
                            tb[1] = 0x80;
                        }
                        if (x87_mem_write(vm, v, run, ea.addr, 2, tb)) return 1;
                    }
                } else if (reg == 4) {
                    if (x87_mem_read(vm, v, run, ea.addr, 10, tb)) return 1;
                    if (x87_bld(v, tb)) return vfl_exit_exception(v, run, 7, 0);
                } else if (reg == 6) {
                    if (x87_bstp(v, tb)) return vfl_exit_exception(v, run, 7, 0);
                    if (x87_mem_write(vm, v, run, ea.addr, 10, tb)) return 1;
                } else if (reg == 7) {
                    long double a;

                    if (fpu_pop(v, &a)) return vfl_exit_exception(v, run, 7, 0);
                    if (x87_store_int(a, tb, 8, 0)) {
                        v->fpu.fsw |= 1;
                        memset(tb, 0, 8);
                        tb[7] = 0x80;
                    }
                    if (x87_mem_write(vm, v, run, ea.addr, 8, tb)) return 1;
                } else {
                    return vfl_exit_exception(v, run, 6, 0);
                }
                vfl_adv(v, n, ripmask);
                return 0;
            }

            default:
                return vfl_exit_exception(v, run, 6, 0);
        }
    }
    n = *pos;
    switch (op) {
        case 0xD8:
            if (mod != 3) return vfl_exit_internal(v, run);
            if (reg == 0) fpu_set(v, 0, fpu_get(v, 0) + fpu_get(v, rm & 7));
            else if (reg == 1) fpu_set(v, 0, fpu_get(v, 0) * fpu_get(v, rm & 7));
            else if (reg == 2) fpu_compare(v, fpu_get(v, 0), fpu_get(v, rm & 7));
            else if (reg == 3) {
                fpu_compare(v, fpu_get(v, 0), fpu_get(v, rm & 7));
                fpu_pop(v, 0);
            } else if (reg == 4) fpu_set(v, 0, fpu_get(v, 0) - fpu_get(v, rm & 7));
            else if (reg == 5) fpu_set(v, 0, fpu_get(v, rm & 7) - fpu_get(v, 0));
            else if (reg == 6) {
                long double b = fpu_get(v, rm & 7);

                if (b == 0.0L) {
                    v->fpu.fsw |= 4;
                    fpu_set(v, 0, fpu_get(v, 0) < 0 ? -1.0L / 0.0L : 1.0L / 0.0L);
                } else {
                    fpu_set(v, 0, fpu_get(v, 0) / b);
                }
            } else {
                long double a = fpu_get(v, 0);

                if (a == 0.0L) {
                    v->fpu.fsw |= 4;
                    fpu_set(v, 0, fpu_get(v, rm & 7) < 0 ? -1.0L / 0.0L : 1.0L / 0.0L);
                } else {
                    fpu_set(v, 0, fpu_get(v, rm & 7) / a);
                }
            }
            break;

        case 0xD9:
            if (mod != 3) return vfl_exit_internal(v, run);
            if (m == 0xD0) break;
            else if (m == 0xE0) fpu_set(v, 0, -fpu_get(v, 0));
            else if (m == 0xE1) {
                long double x = fpu_get(v, 0);

                fpu_set(v, 0, x < 0 ? -x : x);
            } else if (m == 0xE4) fpu_compare(v, fpu_get(v, 0), 0.0L);
            else if (m == 0xE5) {
                long double x = fpu_get(v, 0);
                uint64_t mm;
                uint16_t ee;

                ld_to_f80(x, &mm, &ee);
                v->fpu.fsw &= ~((1 << 8) | (1 << 9) | (1 << 10) | (1 << 14));
                if (fpu_tag(v, fpu_top(v)) == 3) {
                    v->fpu.fsw |= (1 << 10) | (1 << 14);
                } else if ((ee & 0x7FFF) == 0x7FFF) {
                    if (mm & 0x3FFFFFFFFFFFFFFFULL) v->fpu.fsw |= (1 << 14);
                    else v->fpu.fsw |= (1 << 10) | (1 << 14);
                    if (mm >> 63) v->fpu.fsw |= (1 << 9);
                    if (ee >> 15) v->fpu.fsw |= (1 << 9);
                } else if ((ee & 0x7FFF) == 0 && mm == 0) {
                    v->fpu.fsw |= (1 << 14);
                    if (ee >> 15) v->fpu.fsw |= (1 << 9);
                } else if ((ee & 0x7FFF) == 0) {
                    v->fpu.fsw |= (1 << 10);
                    if (ee >> 15) v->fpu.fsw |= (1 << 9);
                } else {
                    v->fpu.fsw |= (1 << 10);
                    if (ee >> 15) v->fpu.fsw |= (1 << 9);
                }
            } else if (m >= 0xE8 && m <= 0xEE) {
                long double c;

                if (m == 0xE8) c = 1.0L;
                else if (m == 0xE9) c = 3.32192809488736234787L;
                else if (m == 0xEA) c = 1.44269504088896340736L;
                else if (m == 0xEB) c = 3.14159265358979323846L;
                else if (m == 0xEC) c = 0.30102999566398119521L;
                else if (m == 0xED) c = 0.69314718055994530942L;
                else c = 0.0L;
                if (fpu_push(v, c)) return vfl_exit_exception(v, run, 7, 0);
            } else if (m == 0xF0) {
                fpu_set(v, 0, f80_2xm1(fpu_get(v, 0)));
            } else if (m == 0xF1) {
                long double y = fpu_get(v, 1);
                long double x = fpu_get(v, 0);

                fpu_pop(v, 0);
                fpu_set(v, 0, f80_y12x(y, x));
            } else if (m == 0xF2) {
                long double t = fpu_get(v, 0);
                long double s = f80_sin(t);
                long double c = f80_cos(t);

                fpu_set(v, 0, s / c);
                if (fpu_push(v, 1.0L)) return vfl_exit_exception(v, run, 7, 0);
            } else if (m == 0xF3) {
                long double y = fpu_get(v, 1);
                long double x = fpu_get(v, 0);

                fpu_pop(v, 0);
                fpu_set(v, 0, f80_atan2(y, x));
            } else if (m == 0xF4) {
                long double x = fpu_get(v, 0);
                uint64_t mm;
                uint16_t ee;
                int64_t ex;
                long double sig = x;

                ld_to_f80(x, &mm, &ee);
                if ((ee & 0x7FFF) != 0 && (ee & 0x7FFF) != 0x7FFF) {
                    ex = (int64_t)(ee & 0x7FFF) - 16383;
                    sig = f80_to_ld((mm & 0x7FFFFFFFFFFFFFFFULL) | 0x3FFF000000000000ULL,
                                    (uint16_t)(0x3FFF | (ee & 0x8000)));
                } else {
                    ex = 0;
                }
                if (fpu_push(v, (long double)ex)) return vfl_exit_exception(v, run, 7, 0);
                fpu_set(v, 1, sig);
            } else if (m == 0xF5) {
                long double x = fpu_get(v, 0);
                long double y = fpu_get(v, 1);
                uint16_t sw = 0;
                long double r = f80_prem(x, y, 1, &sw);

                fpu_set(v, 0, r);
                v->fpu.fsw = (v->fpu.fsw & ~((1 << 8) | (1 << 9) | (1 << 10) | (1 << 14))) |
                             (sw & ((1 << 8) | (1 << 9) | (1 << 10) | (1 << 14)));
            } else if (m == 0xF8) {
                long double x = fpu_get(v, 0);
                long double y = fpu_get(v, 1);
                uint16_t sw = 0;
                long double r = f80_prem(x, y, 0, &sw);

                fpu_set(v, 0, r);
                v->fpu.fsw = (v->fpu.fsw & ~((1 << 8) | (1 << 9) | (1 << 10) | (1 << 14))) |
                             (sw & ((1 << 8) | (1 << 9) | (1 << 10) | (1 << 14)));
            } else if (m == 0xF9) {
                long double y = fpu_get(v, 1);
                long double x = fpu_get(v, 0);
                long double t = x + 1.0L;
                long double r;

                __asm__ volatile ("fyl2xp1" : "+t"(t) : "u"(y) : "st(1)");
                fpu_pop(v, 0);
                fpu_set(v, 0, t);
                r = t;
                (void)r;
            } else if (m == 0xFA) {
                fpu_set(v, 0, f80_sqrt(fpu_get(v, 0)));
            } else if (m == 0xFB) {
                return vfl_exit_exception(v, run, 6, 0);
            } else if (m == 0xFC) {
                fpu_set(v, 0, f80_round(fpu_get(v, 0)));
            } else if (m == 0xFD) {
                long double x = fpu_get(v, 0);
                long double y = fpu_get(v, 1);
                int64_t nn = (int64_t)y;
                uint64_t mm;
                uint16_t ee;
                int64_t ex;

                ld_to_f80(x, &mm, &ee);
                ex = (int64_t)(ee & 0x7FFF) + nn;
                if ((ee & 0x7FFF) == 0 || (ee & 0x7FFF) == 0x7FFF) {
                    fpu_set(v, 0, x);
                } else if (ex <= 0) {
                    fpu_set(v, 0, 0.0L);
                } else if (ex >= 0x7FFF) {
                    v->fpu.fsw |= 8;
                    fpu_set(v, 0, x < 0 ? -1.0L / 0.0L : 1.0L / 0.0L);
                } else {
                    ee = (uint16_t)((ee & 0x8000) | ex);
                    fpu_set(v, 0, f80_to_ld(mm, ee));
                }
            } else if (m == 0xFE) {
                fpu_set(v, 0, f80_sin(fpu_get(v, 0)));
            } else if (m == 0xFF) {
                fpu_set(v, 0, f80_cos(fpu_get(v, 0)));
            } else if (m >= 0xC8 && m <= 0xCF) {
                long double t = fpu_get(v, 0);

                fpu_set(v, 0, fpu_get(v, m & 7));
                fpu_set(v, m & 7, t);
            } else {
                return vfl_exit_exception(v, run, 6, 0);
            }
            break;

        case 0xDA:
            if (mod != 3) return vfl_exit_internal(v, run);
            if (reg <= 3) {
                long double b = fpu_get(v, rm & 7);

                if (!vfl_cond(v, reg == 0 ? 2 : (reg == 1 ? 4 : (reg == 2 ? 6 : 7))))
                    break;
                fpu_set(v, 0, b);
            } else if (reg == 5) {
                if ((rm & 7) == 1) {
                    fpu_compare(v, fpu_get(v, 1), fpu_get(v, 0));
                    fpu_pop(v, 0);
                    fpu_pop(v, 0);
                } else {
                    long double b = fpu_get(v, rm & 7);

                    if (!vfl_cond(v, 7))
                        break;
                    fpu_set(v, 0, b);
                }
            } else {
                return vfl_exit_exception(v, run, 6, 0);
            }
            break;

        case 0xDB:
            if (mod != 3) return vfl_exit_internal(v, run);
            if (reg <= 3) {
                long double b = fpu_get(v, rm & 7);

                if (!vfl_cond(v, reg == 0 ? 3 : (reg == 1 ? 5 : (reg == 2 ? 7 : 10))))
                    break;
                fpu_set(v, 0, b);
            } else if (reg == 4) {
                if ((rm & 7) == 2) {
                    v->fpu.fsw &= ~0x80FFu;
                } else if ((rm & 7) == 3) {
                    fpu_init(v);
                } else if ((rm & 7) < 5) {
                    break;
                } else {
                    return vfl_exit_exception(v, run, 6, 0);
                }
            } else if (reg == 5) {
                fpu_eflags(v, fpu_get(v, 0), fpu_get(v, rm & 7));
            } else if (reg == 6) {
                fpu_eflags(v, fpu_get(v, 0), fpu_get(v, rm & 7));
            } else {
                return vfl_exit_exception(v, run, 6, 0);
            }
            break;

        case 0xDC:
            if (mod != 3) return vfl_exit_internal(v, run);
            if (reg == 0) {
                long double b = fpu_get(v, rm & 7);

                fpu_set(v, rm & 7, fpu_get(v, rm & 7) + fpu_get(v, 0));
                (void)b;
            } else if (reg == 1) {
                fpu_set(v, rm & 7, fpu_get(v, rm & 7) * fpu_get(v, 0));
            } else if (reg == 4) {
                fpu_set(v, rm & 7, fpu_get(v, rm & 7) - fpu_get(v, 0));
            } else if (reg == 5) {
                fpu_set(v, rm & 7, fpu_get(v, 0) - fpu_get(v, rm & 7));
            } else if (reg == 6) {
                long double b = fpu_get(v, rm & 7);

                if (b == 0.0L) {
                    v->fpu.fsw |= 4;
                    fpu_set(v, rm & 7, fpu_get(v, 0) < 0 ? -1.0L / 0.0L : 1.0L / 0.0L);
                } else {
                    fpu_set(v, rm & 7, fpu_get(v, 0) / b);
                }
            } else if (reg == 7) {
                long double a = fpu_get(v, 0);

                if (a == 0.0L) {
                    v->fpu.fsw |= 4;
                    fpu_set(v, rm & 7, fpu_get(v, rm & 7) < 0 ? -1.0L / 0.0L : 1.0L / 0.0L);
                } else {
                    fpu_set(v, rm & 7, fpu_get(v, rm & 7) / a);
                }
            } else {
                return vfl_exit_exception(v, run, 6, 0);
            }
            break;

        case 0xDD:
            if (mod != 3) return vfl_exit_internal(v, run);
            if (reg == 0) {
                int r = (rm & 7);

                fpu_set_tag(v, r, 3);
            } else if (reg == 2) {
                fpu_set(v, rm & 7, fpu_get(v, 0));
            } else if (reg == 3) {
                if (fpu_pop(v, 0)) return vfl_exit_exception(v, run, 7, 0);
            } else if (reg == 4) {
                fpu_compare(v, fpu_get(v, rm & 7), fpu_get(v, 0));
            } else if (reg == 5) {
                fpu_compare(v, fpu_get(v, rm & 7), fpu_get(v, 0));
                fpu_pop(v, 0);
            } else {
                return vfl_exit_exception(v, run, 6, 0);
            }
            break;

        case 0xDE:
            if (mod != 3) return vfl_exit_internal(v, run);
            if (reg == 0) {
                long double b = fpu_get(v, rm & 7);

                fpu_set(v, rm & 7, fpu_get(v, rm & 7) + fpu_get(v, 0));
                fpu_pop(v, 0);
                (void)b;
            } else if (reg == 1) {
                fpu_set(v, rm & 7, fpu_get(v, rm & 7) * fpu_get(v, 0));
                fpu_pop(v, 0);
            } else if (reg == 4) {
                fpu_set(v, rm & 7, fpu_get(v, rm & 7) - fpu_get(v, 0));
                fpu_pop(v, 0);
            } else if (reg == 5) {
                fpu_set(v, rm & 7, fpu_get(v, 0) - fpu_get(v, rm & 7));
                fpu_pop(v, 0);
            } else if (reg == 6) {
                long double b = fpu_get(v, rm & 7);

                if (b == 0.0L) {
                    v->fpu.fsw |= 4;
                    fpu_set(v, rm & 7, fpu_get(v, 0) < 0 ? -1.0L / 0.0L : 1.0L / 0.0L);
                } else {
                    fpu_set(v, rm & 7, fpu_get(v, 0) / b);
                }
                fpu_pop(v, 0);
            } else if (reg == 7) {
                long double a = fpu_get(v, 0);

                if (a == 0.0L) {
                    v->fpu.fsw |= 4;
                    fpu_set(v, rm & 7, fpu_get(v, rm & 7) < 0 ? -1.0L / 0.0L : 1.0L / 0.0L);
                } else {
                    fpu_set(v, rm & 7, fpu_get(v, rm & 7) / a);
                }
                fpu_pop(v, 0);
            } else if (reg == 3 && rm == 1) {
                fpu_compare(v, fpu_get(v, 1), fpu_get(v, 0));
                fpu_pop(v, 0);
                fpu_pop(v, 0);
            } else {
                return vfl_exit_exception(v, run, 6, 0);
            }
            break;

        case 0xDF:
            if (mod != 3) return vfl_exit_internal(v, run);
            if (reg == 0) {
                int r = (rm & 7);

                fpu_set_tag(v, r, 3);
                fpu_pop(v, 0);
            } else if (reg == 4) {
                uint8_t wb[2];

                memcpy(wb, &v->fpu.fsw, 2);
                vfl_write_reg(v, 0, rex, 2, vfl_get16(wb));
            } else if (reg == 5) {
                fpu_eflags(v, fpu_get(v, 0), fpu_get(v, rm & 7));
                fpu_pop(v, 0);
            } else if (reg == 6) {
                fpu_eflags(v, fpu_get(v, 0), fpu_get(v, rm & 7));
            } else {
                return vfl_exit_exception(v, run, 6, 0);
            }
            break;

        default:
            return vfl_exit_exception(v, run, 6, 0);
    }
    vfl_adv(v, n, ripmask);
    return 0;
}

static void vfl_round_f32(float *r, float x, int mode) {
    uint32_t u;
    int64_t t;
    int ov = 0;

    memcpy(&u, &x, 4);
    if (x != x) {
        *r = x;
        return;
    }
    if (x > 3.4028235e38f || x < -3.4028235e38f) {
        *r = x;
        return;
    }
    if (mode == 0) {
        t = f32_rint_i64(u, &ov);
        if (ov) *r = x;
        else *r = (float)t;
        return;
    }
    if (x > -9223372036854775808.0f && x < 9223372036854775808.0f) {
        int64_t i = (int64_t)x;

        if (mode == 3) *r = (float)i;
        else if (mode == 1) {
            if ((float)i > x) i--;
            *r = (float)i;
        } else {
            if ((float)i < x) i++;
            *r = (float)i;
        }
        return;
    }
    *r = x;
}

static void vfl_round_f64(double *r, double x, int mode) {
    long double t;

    if (x != x) {
        *r = x;
        return;
    }
    if (x > 1.7976931348623157e308 || x < -1.7976931348623157e308) {
        *r = x;
        return;
    }
    if (mode == 0) {
        t = f80_round((long double)x);
        *r = (double)t;
        return;
    }
    if (x > -9.223372036854776e18 && x < 9.223372036854776e18) {
        int64_t i = (int64_t)x;

        if (mode == 3) *r = (double)i;
        else if (mode == 1) {
            if ((double)i > x) i--;
            *r = (double)i;
        } else {
            if ((double)i < x) i++;
            *r = (double)i;
        }
        return;
    }
    *r = x;
}

int vfl_vex_step(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf, int *pos,
                 int asz, int seg, int lm, uint64_t ripmask, int is_c4) {
    int R;
    int X;
    int B;
    int mmmmm;
    int W;
    int vvvv;
    int L;
    int pp;
    int op;
    int mod;
    int reg;
    int rm;
    int s1;
    int lanes;
    vfl_ea_t ea;
    int n;
    int rr;
    int i;

    (void)lm;
    if (is_c4) {
        uint8_t b1;
        uint8_t b2;

        if (*pos + 2 >= 15) return vfl_exit_internal(v, run);
        b1 = buf[*pos];
        b2 = buf[*pos + 1];
        R = (b1 >> 7) & 1;
        X = (b1 >> 6) & 1;
        B = (b1 >> 5) & 1;
        mmmmm = b1 & 31;
        W = (b2 >> 7) & 1;
        vvvv = (~(b2 >> 3)) & 15;
        L = (b2 >> 2) & 1;
        pp = b2 & 3;
        op = buf[*pos + 2];
        *pos += 3;
        if (mmmmm != 1) {
            if (mmmmm == 2 || mmmmm == 3) {
                return vfl_vex38_3a(vm, v, run, buf, pos, mmmmm, op, vvvv, L, pp,
                                    W, asz, seg, (R ? 0 : 8), (B ? 0 : 8),
                                    (X ? 0 : 8), ripmask);
            }
            return vfl_exit_exception(v, run, 6, 0);
        }
    } else {
        uint8_t b1;

        if (*pos + 1 >= 15) return vfl_exit_internal(v, run);
        b1 = buf[*pos];
        R = (b1 >> 7) & 1;
        vvvv = (~(b1 >> 3)) & 15;
        L = (b1 >> 2) & 1;
        pp = b1 & 3;
        X = 1;
        B = 1;
        mmmmm = 1;
        W = 0;
        op = buf[*pos + 1];
        *pos += 2;
    }
    s1 = vvvv;
    lanes = L ? 8 : 4;
    if (op == 0x77 && pp == 0) {
        int k;

        n = *pos;
        if (L) {
            for (k = 0; k < 16; k++) {
                v->fpu.xmm[k].lo = 0;
                v->fpu.xmm[k].hi = 0;
                v->fpu.ymm_hi[k].lo = 0;
                v->fpu.ymm_hi[k].hi = 0;
            }
        } else {
            for (k = 0; k < 16; k++) {
                v->fpu.ymm_hi[k].lo = 0;
                v->fpu.ymm_hi[k].hi = 0;
            }
        }
        vfl_adv(v, n, ripmask);
        return 0;
    }
    if (*pos >= 15) return vfl_exit_internal(v, run);
    {
        uint8_t m = buf[*pos];

        (*pos)++;
        mod = (m >> 6) & 3;
        reg = ((m >> 3) & 7) | (R ? 0 : 8);
        rm = (m & 7) | (B ? 0 : 8);
    }
    if (vfl_decode_ea(v, buf, pos, mod, rm, asz, seg, B ? 0 : 1, X ? 0 : 1, &ea) < 0)
        return vfl_exit_internal(v, run);
    n = *pos;
    if (!ea.is_reg && ea.rip_rel)
        ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
    switch (op) {
        case 0x10:
        case 0x11: {
            if ((pp == 2 || pp == 3) && L) return vfl_exit_exception(v, run, 6, 0);
            if (op == 0x10) {
                uint32_t d[8];

                if (ea.is_reg) {
                    vec_read(v, ea.reg, lanes, d);
                    vec_write(v, reg, lanes, d, 1);
                } else {
                    uint8_t tmp[32];

                    if (pp == 2 || pp == 3) {
                        int sb = pp == 3 ? 4 : 8;

                        rr = vfl_mem_read(vm, v, ea.addr, (uint64_t)sb, tmp);
                        if (rr == -2) return vfl_mem_fault(v, run);
                        if (rr == 1) {
                            vec_read(v, s1, lanes, d);
                            vec_write(v, reg, lanes, d, 1);
                            return vfl_mmio_or_fault(vm, v, run, ea.addr, sb,
                                                     VFL_P_MMIO_XMM, reg, 0, 0, 0,
                                                     n, ripmask);
                        }
                        if (rr < 0) return vfl_exit_exception(v, run, 13, 0);
                        vec_read(v, s1, lanes, d);
                        memcpy(d, tmp, (size_t)sb);
                        vec_write(v, reg, lanes, d, 1);
                    } else {
                        return sse_move_load(vm, v, run, &ea, reg, lanes, n,
                                             ripmask, 0, 1);
                    }
                }
                vfl_adv(v, n, ripmask);
                return 0;
            }
            {
                uint32_t d[8];

                if (pp == 2 || pp == 3) {
                    int sb = pp == 3 ? 4 : 8;

                    vec_read(v, reg, lanes, d);
                    if (ea.is_reg) {
                        uint32_t t[8];

                        vec_read(v, ea.reg, lanes, t);
                        t[0] = d[0];
                        if (sb == 8) t[1] = d[1];
                        vec_write(v, ea.reg, lanes, t, 1);
                    } else {
                        return sse_scalar_store(vm, v, run, &ea, (uint8_t *)d,
                                                sb, n, ripmask);
                    }
                    vfl_adv(v, n, ripmask);
                    return 0;
                }
                vec_read(v, reg, lanes, d);
                return sse_store_vec(vm, v, run, &ea, lanes, d, n, ripmask, 0);
            }
        }

        case 0x28:
        case 0x29: {
            int align = lanes * 4;

            if (pp != 0 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (op == 0x28)
                return sse_move_load(vm, v, run, &ea, reg, lanes, n, ripmask,
                                     align, 1);
            {
                uint32_t d[8];

                vec_read(v, reg, lanes, d);
                if (ea.is_reg) {
                    vec_write(v, ea.reg, lanes, d, 1);
                    vfl_adv(v, n, ripmask);
                    return 0;
                }
                if (ea.addr & (uint64_t)(align - 1))
                    return vfl_exit_exception(v, run, 13, 0);
                return sse_store_vec(vm, v, run, &ea, lanes, d, n, ripmask, 0);
            }
        }

        case 0x6E:
        case 0x7E: {
            int q = W ? 8 : 4;

            if (pp != 0 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (op == 0x6E) {
                uint32_t d[8] = {0, 0, 0, 0, 0, 0, 0, 0};

                if (ea.is_reg) {
                    uint64_t gv = vfl_read_reg(v, ea.reg, 0, q);

                    memcpy(d, &gv, (size_t)q);
                } else {
                    uint8_t tmp[8] = {0, 0, 0, 0, 0, 0, 0, 0};

                    rr = vfl_mem_read(vm, v, ea.addr, (uint64_t)q, tmp);
                    if (rr == -2) return vfl_mem_fault(v, run);
                    if (rr == 1)
                        return vfl_mmio_or_fault(vm, v, run, ea.addr, q,
                                                 VFL_P_MMIO_XMM, reg, 1, 0, 0,
                                                 n, ripmask);
                    if (rr < 0) return vfl_exit_exception(v, run, 13, 0);
                    memcpy(d, tmp, (size_t)q);
                }
                vec_write(v, reg, lanes, d, 1);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            {
                uint32_t d[8];

                vec_read(v, reg, lanes, d);
                if (ea.is_reg) {
                    uint64_t gv = 0;

                    memcpy(&gv, d, (size_t)q);
                    vfl_write_reg(v, ea.reg, 0, q, gv);
                } else {
                    return sse_scalar_store(vm, v, run, &ea, (uint8_t *)d,
                                            q, n, ripmask);
                }
                vfl_adv(v, n, ripmask);
                return 0;
            }
        }

        case 0x6F:
        case 0x7F:
            if (pp != 1 && pp != 3) return vfl_exit_exception(v, run, 6, 0);
            if (op == 0x6F)
                return sse_move_load(vm, v, run, &ea, reg, lanes, n, ripmask,
                                     0, 1);
            if (ea.is_reg) return vfl_exit_exception(v, run, 6, 0);
            {
                uint32_t d[8];

                vec_read(v, reg, lanes, d);
                return sse_store_vec(vm, v, run, &ea, lanes, d, n, ripmask, 0);
            }

        case 0x54:
        case 0x55:
        case 0x56:
        case 0x57: {
            uint32_t a[8];
            uint32_t b[8];
            int which = op == 0x54 ? 1 : (op == 0x55 ? 3 : (op == 0x56 ? 2 : 0));

            if (pp != 0 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            vec_read(v, s1, lanes, a);
            if (ea.is_reg) vec_read(v, ea.reg, lanes, b);
            else {
                rr = sse_load_vec(vm, v, run, &ea, lanes, b);
                if (rr) return 1;
            }
            for (i = 0; i < lanes; i++) {
                if (which == 0) a[i] ^= b[i];
                else if (which == 1) a[i] &= b[i];
                else if (which == 2) a[i] |= b[i];
                else a[i] = (~a[i]) & b[i];
            }
            vec_write(v, reg, lanes, a, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x58:
        case 0x59:
        case 0x5C:
        case 0x5E: {
            uint32_t a[8];
            uint32_t b[8];
            int which = op == 0x58 ? 0 : (op == 0x5C ? 1 : (op == 0x59 ? 2 : 3));

            if (pp != 0 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            vec_read(v, s1, lanes, a);
            if (ea.is_reg) vec_read(v, ea.reg, lanes, b);
            else {
                rr = sse_load_vec(vm, v, run, &ea, lanes, b);
                if (rr) return 1;
            }
            if (pp == 0) {
                for (i = 0; i < lanes; i++) {
                    float x = vfl_u32f(a[i]);
                    float y = vfl_u32f(b[i]);

                    if (which == 0) a[i] = fu32(vfl_f32_add(x, y));
                    else if (which == 1) a[i] = fu32(vfl_f32_sub(x, y));
                    else if (which == 2) a[i] = fu32(vfl_f32_mul(x, y));
                    else a[i] = fu32(vfl_f32_div(x, y));
                }
            } else {
                for (i = 0; i < lanes / 2; i++) {
                    uint64_t au = (uint64_t)a[i * 2] | ((uint64_t)a[i * 2 + 1] << 32);
                    uint64_t bu = (uint64_t)b[i * 2] | ((uint64_t)b[i * 2 + 1] << 32);
                    double x = vfl_u64d(au);
                    double y = vfl_u64d(bu);
                    uint64_t o;

                    if (which == 0) o = du64(vfl_f64_add(x, y));
                    else if (which == 1) o = du64(vfl_f64_sub(x, y));
                    else if (which == 2) o = du64(vfl_f64_mul(x, y));
                    else o = du64(vfl_f64_div(x, y));
                    a[i * 2] = (uint32_t)o;
                    a[i * 2 + 1] = (uint32_t)(o >> 32);
                }
            }
            vec_write(v, reg, lanes, a, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xC2: {
            int imm;

            if (pp != 0 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (*pos + 1 > 15) return vfl_exit_internal(v, run);
            imm = buf[*pos] & 7;
            (*pos)++;
            n = *pos;
            {
                uint32_t a[8];
                uint32_t b[8];

                vec_read(v, s1, lanes, a);
                if (ea.is_reg) vec_read(v, ea.reg, lanes, b);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, lanes, b);
                    if (rr) return 1;
                }
                if (pp == 0) {
                    for (i = 0; i < lanes; i++)
                        a[i] = f32_cmp(vfl_u32f(a[i]), vfl_u32f(b[i]), imm);
                } else {
                    for (i = 0; i < lanes / 2; i++) {
                        uint64_t au = (uint64_t)a[i * 2] | ((uint64_t)a[i * 2 + 1] << 32);
                        uint64_t bu = (uint64_t)b[i * 2] | ((uint64_t)b[i * 2 + 1] << 32);
                        double x = vfl_u64d(au);
                        double y = vfl_u64d(bu);
                        int eq = x == y;
                        int lt = x < y;
                        int un = x != x || y != y;
                        uint64_t m = 0;

                        if (imm == 0) m = eq && !un ? ~0ULL : 0;
                        else if (imm == 1) m = lt && !un ? ~0ULL : 0;
                        else if (imm == 2) m = (lt || eq) && !un ? ~0ULL : 0;
                        else if (imm == 3) m = un ? ~0ULL : 0;
                        else if (imm == 4) m = (!eq || un) ? ~0ULL : 0;
                        else if (imm == 5) m = (!lt || un) ? ~0ULL : 0;
                        else if (imm == 6) m = ((!lt && !eq) || un) ? ~0ULL : 0;
                        else m = !un ? ~0ULL : 0;
                        a[i * 2] = (uint32_t)m;
                        a[i * 2 + 1] = (uint32_t)(m >> 32);
                    }
                }
                vec_write(v, reg, lanes, a, 1);
                vfl_adv(v, n, ripmask);
                return 0;
            }
        }

        case 0xC4:
        case 0xC5:
        case 0xC6: {
            int imm;

            if (op == 0xC6) {
                if (pp != 0) return vfl_exit_exception(v, run, 6, 0);
            } else if (pp != 1) {
                return vfl_exit_exception(v, run, 6, 0);
            }
            if (*pos + 1 > 15) return vfl_exit_internal(v, run);
            imm = buf[*pos];
            (*pos)++;
            n = *pos;
            if (op == 0xC6) {
                uint32_t b[8];
                uint32_t r[8];
                uint32_t a[8];
                int h;

                vec_read(v, s1, lanes, a);
                if (ea.is_reg) vec_read(v, ea.reg, lanes, b);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, lanes, b);
                    if (rr) return 1;
                }
                for (h = 0; h < lanes / 4; h++) {
                    for (i = 0; i < 2; i++)
                        r[h * 4 + i] = a[h * 4 + ((imm >> (i * 2)) & 3)];
                    for (i = 0; i < 2; i++)
                        r[h * 4 + 2 + i] = b[h * 4 + ((imm >> (4 + i * 2)) & 3)];
                }
                vec_write(v, reg, lanes, r, 1);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (op == 0xC4) {
                uint16_t w;
                uint32_t d[8];

                if (ea.is_reg) {
                    uint32_t t[8];

                    vec_read(v, ea.reg, lanes, t);
                    w = (uint16_t)t[0];
                } else {
                    uint8_t tmp[2];

                    if (sse_mem_fill(vm, v, run, ea.addr, tmp, 2)) return 1;
                    w = vfl_get16(tmp);
                }
                vec_read(v, s1, lanes, d);
                ((uint16_t *)d)[imm & 7] = w;
                vec_write(v, reg, lanes, d, 1);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            {
                uint32_t t[8];
                uint16_t val;

                if (mod != 3) return vfl_exit_exception(v, run, 6, 0);
                vec_read(v, ea.reg, lanes, t);
                val = (uint16_t)(((uint16_t *)t)[imm & 7]);
                vfl_write_reg(v, reg, 0, 4, val);
                vfl_adv(v, n, ripmask);
                return 0;
            }
        }

        case 0x70: {
            int imm;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (*pos + 1 > 15) return vfl_exit_internal(v, run);
            imm = buf[*pos];
            (*pos)++;
            n = *pos;
            {
                uint32_t t[4];
                uint32_t r[8];

                if (ea.is_reg) vec_read(v, ea.reg, lanes, t);
                else {
                    rr = sse_load_vec(vm, v, run, &ea, lanes, t);
                    if (rr) return 1;
                }
                for (i = 0; i < lanes; i++)
                    r[i] = t[(imm >> ((i & 3) * 2)) & 3];
                vec_write(v, reg, lanes, r, 1);
                vfl_adv(v, n, ripmask);
                return 0;
            }
        }

        case 0xEB:
        case 0xEF: {
            uint32_t a[8];
            uint32_t b[8];

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            vec_read(v, s1, lanes, a);
            if (ea.is_reg) vec_read(v, ea.reg, lanes, b);
            else {
                rr = sse_load_vec(vm, v, run, &ea, lanes, b);
                if (rr) return 1;
            }
            for (i = 0; i < lanes; i++) {
                if (op == 0xEB) a[i] |= b[i];
                else a[i] ^= b[i];
            }
            vec_write(v, reg, lanes, a, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xDB:
        case 0xDF: {
            uint32_t a[8];
            uint32_t b[8];

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            vec_read(v, s1, lanes, a);
            if (ea.is_reg) vec_read(v, ea.reg, lanes, b);
            else {
                rr = sse_load_vec(vm, v, run, &ea, lanes, b);
                if (rr) return 1;
            }
            for (i = 0; i < lanes; i++) {
                if (op == 0xDB) a[i] &= b[i];
                else a[i] = (~a[i]) & b[i];
            }
            vec_write(v, reg, lanes, a, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x50: {
            uint32_t t[8];
            uint64_t mask = 0;

            if (pp != 0 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (ea.is_reg) vec_read(v, ea.reg, lanes, t);
            else {
                rr = sse_load_vec(vm, v, run, &ea, lanes, t);
                if (rr) return 1;
            }
            if (pp == 0) {
                for (i = 0; i < lanes; i++)
                    if ((t[i] >> 31) & 1) mask |= 1ULL << i;
            } else {
                for (i = 0; i < lanes / 2; i++) {
                    uint64_t q = (uint64_t)t[i * 2] | ((uint64_t)t[i * 2 + 1] << 32);

                    if ((q >> 63) & 1) mask |= 1ULL << i;
                }
            }
            vfl_write_reg(v, reg, 0, 4, mask);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x0E:
        case 0x0F: {
            uint32_t a[8];
            uint32_t b[8];

            if (pp != 0) return vfl_exit_exception(v, run, 6, 0);
            vec_read(v, s1, lanes, a);
            if (ea.is_reg) vec_read(v, ea.reg, lanes, b);
            else {
                rr = sse_load_vec(vm, v, run, &ea, lanes, b);
                if (rr) return 1;
            }
            if (op == 0x0E) {
                uint64_t andv = 0;
                uint64_t nand = 0;

                for (i = 0; i < lanes / 2; i++) {
                    uint64_t x = (uint64_t)a[i * 2] | ((uint64_t)a[i * 2 + 1] << 32);
                    uint64_t y = (uint64_t)b[i * 2] | ((uint64_t)b[i * 2 + 1] << 32);

                    if ((x & y) == 0) andv |= 1ULL << i;
                    if ((~x & y) == 0) nand |= 1ULL << i;
                }
                v->regs.rflags &= ~((1ULL << 0) | (1ULL << 2) | (1ULL << 4) |
                                    (1ULL << 6) | (1ULL << 7) | (1ULL << 11));
                if (andv == (uint64_t)((1ULL << (lanes / 2)) - 1))
                    v->regs.rflags |= (1ULL << 6);
                if (nand == (uint64_t)((1ULL << (lanes / 2)) - 1))
                    v->regs.rflags |= (1ULL << 0);
            } else {
                uint64_t andv = 0;
                uint64_t nand = 0;

                for (i = 0; i < lanes; i++) {
                    if ((a[i] & b[i]) == 0) andv |= 1ULL << i;
                    if ((~a[i] & b[i]) == 0) nand |= 1ULL << i;
                }
                v->regs.rflags &= ~((1ULL << 0) | (1ULL << 2) | (1ULL << 4) |
                                    (1ULL << 6) | (1ULL << 7) | (1ULL << 11));
                if (andv == (lanes == 8 ? 0xFFULL : 0xFULL))
                    v->regs.rflags |= (1ULL << 6);
                if (nand == (lanes == 8 ? 0xFFULL : 0xFULL))
                    v->regs.rflags |= (1ULL << 0);
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        default:
            break;
    }
    return vfl_vex_more(vm, v, run, buf, pos, op, pp, W, L, lanes, s1,
                        asz, seg, reg, rm, mod, &ea, n, ripmask);
}

static int vfl_vex_more(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                        int *pos, int op, int pp, int W, int L, int lanes, int s1,
                        int asz, int seg, int reg, int rm, int mod,
                        vfl_ea_t *ea, int n, uint64_t ripmask) {
    uint32_t A[8];
    uint32_t B[8];
    uint32_t R[8];
    int i;
    int rr;

    (void)buf;
    (void)pos;
    (void)asz;
    (void)seg;
    (void)rm;
    (void)mod;
    vec_read(v, s1, lanes, A);
    if (ea->is_reg) vec_read(v, ea->reg, lanes, B);
    else {
        rr = sse_load_vec(vm, v, run, ea, lanes, B);
        if (rr) return 1;
    }
    switch (op) {
        case 0x58:
        case 0x59:
        case 0x5C:
        case 0x5E: {
            int which = op == 0x58 ? 0 : (op == 0x5C ? 1 : (op == 0x59 ? 2 : 3));

            if (pp != 0 && pp != 1 && pp != 2 && pp != 3)
                return vfl_exit_exception(v, run, 6, 0);
            if (pp == 2 || pp == 3) {
                if (L) return vfl_exit_exception(v, run, 6, 0);
                if (pp == 2) {
                    float x = vfl_u32f(A[0]);
                    float y = vfl_u32f(B[0]);

                    if (which == 0) R[0] = fu32(vfl_f32_add(x, y));
                    else if (which == 1) R[0] = fu32(vfl_f32_sub(x, y));
                    else if (which == 2) R[0] = fu32(vfl_f32_mul(x, y));
                    else R[0] = fu32(vfl_f32_div(x, y));
                    R[1] = A[1];
                    R[2] = A[2];
                    R[3] = A[3];
                } else {
                    uint64_t au = (uint64_t)A[0] | ((uint64_t)A[1] << 32);
                    uint64_t bu = (uint64_t)B[0] | ((uint64_t)B[1] << 32);
                    double x = vfl_u64d(au);
                    double y = vfl_u64d(bu);
                    uint64_t o;

                    if (which == 0) o = du64(vfl_f64_add(x, y));
                    else if (which == 1) o = du64(vfl_f64_sub(x, y));
                    else if (which == 2) o = du64(vfl_f64_mul(x, y));
                    else o = du64(vfl_f64_div(x, y));
                    R[0] = (uint32_t)o;
                    R[1] = (uint32_t)(o >> 32);
                    R[2] = A[2];
                    R[3] = A[3];
                }
                vec_write(v, reg, lanes, R, 1);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (pp == 0) {
                for (i = 0; i < lanes; i++) {
                    float x = vfl_u32f(A[i]);
                    float y = vfl_u32f(B[i]);

                    if (which == 0) R[i] = fu32(vfl_f32_add(x, y));
                    else if (which == 1) R[i] = fu32(vfl_f32_sub(x, y));
                    else if (which == 2) R[i] = fu32(vfl_f32_mul(x, y));
                    else R[i] = fu32(vfl_f32_div(x, y));
                }
            } else {
                for (i = 0; i < lanes / 2; i++) {
                    uint64_t au = (uint64_t)A[i * 2] | ((uint64_t)A[i * 2 + 1] << 32);
                    uint64_t bu = (uint64_t)B[i * 2] | ((uint64_t)B[i * 2 + 1] << 32);
                    double x = vfl_u64d(au);
                    double y = vfl_u64d(bu);
                    uint64_t o;

                    if (which == 0) o = du64(vfl_f64_add(x, y));
                    else if (which == 1) o = du64(vfl_f64_sub(x, y));
                    else if (which == 2) o = du64(vfl_f64_mul(x, y));
                    else o = du64(vfl_f64_div(x, y));
                    R[i * 2] = (uint32_t)o;
                    R[i * 2 + 1] = (uint32_t)(o >> 32);
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x51: {
            if (pp != 0 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (pp == 0) {
                for (i = 0; i < lanes; i++)
                    R[i] = fu32(vfl_f32_sqrt(vfl_u32f(A[i])));
            } else {
                for (i = 0; i < lanes / 2; i++) {
                    uint64_t au = (uint64_t)A[i * 2] | ((uint64_t)A[i * 2 + 1] << 32);
                    uint64_t o = du64(vfl_f64_sqrt(vfl_u64d(au)));

                    R[i * 2] = (uint32_t)o;
                    R[i * 2 + 1] = (uint32_t)(o >> 32);
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x5D:
        case 0x5F: {
            if (pp != 0 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (pp == 0) {
                for (i = 0; i < lanes; i++)
                    R[i] = op == 0x5D ? f32_min(vfl_u32f(A[i]), vfl_u32f(B[i]))
                                      : f32_max(vfl_u32f(A[i]), vfl_u32f(B[i]));
            } else {
                for (i = 0; i < lanes / 2; i++) {
                    uint64_t au = (uint64_t)A[i * 2] | ((uint64_t)A[i * 2 + 1] << 32);
                    uint64_t bu = (uint64_t)B[i * 2] | ((uint64_t)B[i * 2 + 1] << 32);
                    uint64_t o = op == 0x5D ? f64_min(vfl_u64d(au), vfl_u64d(bu))
                                            : f64_max(vfl_u64d(au), vfl_u64d(bu));

                    R[i * 2] = (uint32_t)o;
                    R[i * 2 + 1] = (uint32_t)(o >> 32);
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x60:
        case 0x61:
        case 0x62:
        case 0x68:
        case 0x69:
        case 0x6A: {
            int el = op == 0x60 || op == 0x68 ? 0 : (op == 0x61 || op == 0x69 ? 1 : 2);
            int hi = op >= 0x68;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (el == 0) {
                uint8_t *pa = (uint8_t *)A;
                uint8_t *pb = (uint8_t *)B;
                uint8_t *pr = (uint8_t *)R;
                int h;
                int k;

                for (h = 0; h < lanes / 4; h++) {
                    for (k = 0; k < 8; k++) {
                        int base = hi ? 8 : 0;

                        pr[h * 16 + k * 2] = pa[h * 16 + base + k];
                        pr[h * 16 + k * 2 + 1] = pb[h * 16 + base + k];
                    }
                }
            } else if (el == 1) {
                uint16_t *pa = (uint16_t *)A;
                uint16_t *pb = (uint16_t *)B;
                uint16_t *pr = (uint16_t *)R;
                int h;
                int k;

                for (h = 0; h < lanes / 4; h++) {
                    for (k = 0; k < 4; k++) {
                        int base = hi ? 4 : 0;

                        pr[h * 8 + k * 2] = pa[h * 8 + base + k];
                        pr[h * 8 + k * 2 + 1] = pb[h * 8 + base + k];
                    }
                }
            } else {
                int h;
                int k;
                int base = hi ? 2 : 0;

                for (h = 0; h < lanes / 4; h++) {
                    for (k = 0; k < 2; k++) {
                        R[h * 4 + k * 2] = A[h * 4 + base + k];
                        R[h * 4 + k * 2 + 1] = B[h * 4 + base + k];
                    }
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x63:
        case 0x67:
        case 0x6B: {
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (op == 0x63) {
                int16_t *pa = (int16_t *)A;
                int16_t *pb = (int16_t *)B;
                uint8_t *pr = (uint8_t *)R;
                int h;
                int k;

                for (h = 0; h < lanes / 4; h++) {
                    for (k = 0; k < 16; k++) {
                        int src = k < 8 ? pa[h * 8 + k] : pb[h * 8 + k - 8];

                        pr[h * 16 + k] = (uint8_t)(src < -128 ? -128 : (src > 127 ? 127 : src));
                    }
                }
            } else if (op == 0x67) {
                int32_t *pa = (int32_t *)A;
                int32_t *pb = (int32_t *)B;
                uint16_t *pr = (uint16_t *)R;
                int h;
                int k;

                for (h = 0; h < lanes / 4; h++) {
                    for (k = 0; k < 8; k++) {
                        int32_t x = k < 4 ? pa[h * 4 + k] : pb[h * 4 + k - 4];

                        pr[h * 8 + k] = (uint16_t)(x < -32768 ? -32768 : (x > 32767 ? 32767 : x));
                    }
                }
            } else {
                int16_t *pa = (int16_t *)A;
                int16_t *pb = (int16_t *)B;
                uint8_t *pr = (uint8_t *)R;
                int h;
                int k;

                for (h = 0; h < lanes / 4; h++) {
                    for (k = 0; k < 16; k++) {
                        int src = k < 8 ? pa[h * 8 + k] : pb[h * 8 + k - 8];

                        pr[h * 16 + k] = (uint8_t)(src < 0 ? 0 : (src > 255 ? 255 : src));
                    }
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x64:
        case 0x65:
        case 0x66:
        case 0x74:
        case 0x75:
        case 0x76: {
            int el;
            int which;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (op <= 0x66) {
                el = op - 0x64;
                which = 3;
            } else {
                el = op - 0x74;
                which = 2;
            }
            if (el == 0) {
                uint8_t *pa = (uint8_t *)A;
                uint8_t *pb = (uint8_t *)B;
                uint8_t *pr = (uint8_t *)R;
                int k;

                for (k = 0; k < 4 * lanes; k++) {
                    if (which == 2) pr[k] = pa[k] == pb[k] ? 0xFF : 0;
                    else pr[k] = (int8_t)pa[k] > (int8_t)pb[k] ? 0xFF : 0;
                }
            } else if (el == 1) {
                uint16_t *pa = (uint16_t *)A;
                uint16_t *pb = (uint16_t *)B;
                uint16_t *pr = (uint16_t *)R;
                int k;

                for (k = 0; k < 2 * lanes; k++) {
                    if (which == 2) pr[k] = pa[k] == pb[k] ? 0xFFFF : 0;
                    else pr[k] = (int16_t)pa[k] > (int16_t)pb[k] ? 0xFFFF : 0;
                }
            } else {
                int k;

                for (k = 0; k < lanes; k++) {
                    if (which == 2) R[k] = A[k] == B[k] ? 0xFFFFFFFFu : 0;
                    else R[k] = (int32_t)A[k] > (int32_t)B[k] ? 0xFFFFFFFFu : 0;
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xFC:
        case 0xFD:
        case 0xFE:
        case 0xF8:
        case 0xF9:
        case 0xFA:
        case 0xFB:
        case 0xD4: {
            int el;
            int sub = op == 0xF8 || op == 0xF9 || op == 0xFA || op == 0xFB;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (op <= 0xFE && op >= 0xFC) el = op - 0xFC;
            else if (op == 0xD4) el = 3;
            else el = op - 0xF8;
            if (el == 0) {
                uint8_t *pa = (uint8_t *)A;
                uint8_t *pb = (uint8_t *)B;
                uint8_t *pr = (uint8_t *)R;
                int k;

                for (k = 0; k < 4 * lanes; k++)
                    pr[k] = sub ? pa[k] - pb[k] : pa[k] + pb[k];
            } else if (el == 1) {
                uint16_t *pa = (uint16_t *)A;
                uint16_t *pb = (uint16_t *)B;
                uint16_t *pr = (uint16_t *)R;
                int k;

                for (k = 0; k < 2 * lanes; k++)
                    pr[k] = sub ? pa[k] - pb[k] : pa[k] + pb[k];
            } else if (el == 2) {
                int k;

                for (k = 0; k < lanes; k++)
                    R[k] = sub ? A[k] - B[k] : A[k] + B[k];
            } else {
                int k;

                for (k = 0; k < lanes / 2; k++) {
                    uint64_t x = (uint64_t)A[k * 2] | ((uint64_t)A[k * 2 + 1] << 32);
                    uint64_t y = (uint64_t)B[k * 2] | ((uint64_t)B[k * 2 + 1] << 32);
                    uint64_t r = sub ? x - y : x + y;

                    R[k * 2] = (uint32_t)r;
                    R[k * 2 + 1] = (uint32_t)(r >> 32);
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x70: {
            int imm;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (*pos + 1 > 15) return vfl_exit_internal(v, run);
            imm = buf[*pos];
            (*pos)++;
            n = *pos;
            for (i = 0; i < lanes; i++)
                R[i] = B[(i & ~3) + ((imm >> ((i & 3) * 2)) & 3)];
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x14:
        case 0x15: {
            int el = pp == 1 ? 1 : 0;
            int hi = op == 0x15;

            if (pp != 0 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (el == 0) {
                int h;
                int k;

                for (h = 0; h < lanes / 4; h++) {
                    for (k = 0; k < 2; k++) {
                        R[h * 4 + k * 2] = A[h * 4 + (hi ? 2 : 0) + k];
                        R[h * 4 + k * 2 + 1] = B[h * 4 + (hi ? 2 : 0) + k];
                    }
                }
            } else {
                int h;

                for (h = 0; h < lanes / 4; h++) {
                    uint64_t x = (uint64_t)A[h * 4 + (hi ? 2 : 0)] |
                                 ((uint64_t)A[h * 4 + (hi ? 2 : 0) + 1] << 32);
                    uint64_t y = (uint64_t)B[h * 4 + (hi ? 2 : 0)] |
                                 ((uint64_t)B[h * 4 + (hi ? 2 : 0) + 1] << 32);

                    R[h * 4] = (uint32_t)x;
                    R[h * 4 + 1] = (uint32_t)(x >> 32);
                    R[h * 4 + 2] = (uint32_t)y;
                    R[h * 4 + 3] = (uint32_t)(y >> 32);
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xD7: {
            uint64_t mask = 0;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            for (i = 0; i < 4 * lanes; i++)
                if ((((uint8_t *)B)[i] >> 7) & 1) mask |= 1ULL << i;
            vfl_write_reg(v, reg, 0, 4, mask);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xE7:
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (ea->is_reg) return vfl_exit_exception(v, run, 6, 0);
            vec_read(v, reg, lanes, A);
            return sse_store_vec(vm, v, run, ea, lanes, A, n, ripmask, 0);

        case 0x71:
        case 0x72:
        case 0x73: {
            int imm;
            int grp;
            int el = op - 0x71;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (*pos + 1 > 15) return vfl_exit_internal(v, run);
            grp = reg & 7;
            imm = buf[*pos];
            (*pos)++;
            n = *pos;
            if (el == 2 && (grp == 3 || grp == 7)) {
                uint8_t b[32];
                uint8_t r[32];
                int k;
                int h;

                if (L) return vfl_exit_exception(v, run, 6, 0);
                for (k = 0; k < lanes * 4; k++)
                    b[k] = ((uint8_t *)A)[k];
                for (h = 0; h < lanes / 4; h++) {
                    if (grp == 3) {
                        for (k = 0; k < 16; k++)
                            r[h * 16 + k] = k + imm < 16 ? b[h * 16 + k + imm] : 0;
                    } else {
                        for (k = 15; k >= 0; k--)
                            r[h * 16 + k] = k - imm >= 0 ? b[h * 16 + k - imm] : 0;
                    }
                }
                for (k = 0; k < lanes; k++)
                    memcpy(&R[k], r + k * 4, 4);
                vec_write(v, reg, lanes, R, 1);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (grp != 2 && grp != 4 && grp != 6) return vfl_exit_exception(v, run, 6, 0);
            for (i = 0; i < lanes; i++)
                R[i] = A[i];
            if (el == 0) {
                uint16_t *pr = (uint16_t *)R;
                int k;

                if (imm > 15) imm = 16;
                for (k = 0; k < 2 * lanes; k++) {
                    if (grp == 2) pr[k] >>= imm;
                    else if (grp == 4) pr[k] = (uint16_t)((int16_t)pr[k] >> imm);
                    else pr[k] <<= imm;
                }
            } else if (el == 1) {
                if (imm > 31) imm = 32;
                for (i = 0; i < lanes; i++) {
                    if (grp == 2) R[i] >>= imm;
                    else if (grp == 4) R[i] = (uint32_t)((int32_t)R[i] >> imm);
                    else R[i] <<= imm;
                }
            } else {
                if (imm > 63) imm = 64;
                for (i = 0; i < lanes / 2; i++) {
                    uint64_t x = (uint64_t)R[i * 2] | ((uint64_t)R[i * 2 + 1] << 32);
                    uint64_t o;

                    if (grp == 2) o = imm >= 64 ? 0 : x >> imm;
                    else if (grp == 4) o = imm >= 64 ? ((int64_t)x < 0 ? ~0ULL : 0) : (uint64_t)((int64_t)x >> imm);
                    else o = imm >= 64 ? 0 : x << imm;
                    R[i * 2] = (uint32_t)o;
                    R[i * 2 + 1] = (uint32_t)(o >> 32);
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xD1:
        case 0xD2:
        case 0xD3: {
            uint64_t cnt = (uint64_t)B[0] | ((uint64_t)B[1] << 32);
            int el = op - 0xD1;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            for (i = 0; i < lanes; i++)
                R[i] = A[i];
            if (el == 0) {
                uint16_t *pr = (uint16_t *)R;
                int k;

                if (cnt > 15) cnt = 16;
                for (k = 0; k < 2 * lanes; k++)
                    pr[k] >>= (int)cnt;
            } else if (el == 1) {
                if (cnt > 31) cnt = 32;
                for (i = 0; i < lanes; i++)
                    R[i] >>= (int)cnt;
            } else {
                if (cnt > 63) cnt = 64;
                for (i = 0; i < lanes / 2; i++) {
                    uint64_t x = (uint64_t)R[i * 2] | ((uint64_t)R[i * 2 + 1] << 32);
                    uint64_t o = cnt >= 64 ? 0 : x >> cnt;

                    R[i * 2] = (uint32_t)o;
                    R[i * 2 + 1] = (uint32_t)(o >> 32);
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xE1:
        case 0xE2: {
            uint64_t cnt = (uint64_t)B[0] | ((uint64_t)B[1] << 32);
            int el = op - 0xE1;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            for (i = 0; i < lanes; i++)
                R[i] = A[i];
            if (el == 0) {
                uint16_t *pr = (uint16_t *)R;
                int k;

                if (cnt > 15) cnt = 16;
                for (k = 0; k < 2 * lanes; k++)
                    pr[k] = (uint16_t)((int16_t)pr[k] >> (int)cnt);
            } else {
                if (cnt > 31) cnt = 32;
                for (i = 0; i < lanes; i++)
                    R[i] = (uint32_t)((int32_t)R[i] >> (int)cnt);
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        default:
            break;
    }
    return vfl_vex_more2(vm, v, run, buf, pos, op, pp, W, L, lanes, s1,
                         reg, ea, n, ripmask);
}

static int vfl_vex_more2(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                         int *pos, int op, int pp, int W, int L, int lanes, int s1,
                         int reg, vfl_ea_t *ea, int n, uint64_t ripmask) {
    uint32_t A[8];
    uint32_t B[8];
    uint32_t R[8];
    int i;
    int rr;

    (void)buf;
    (void)pos;
    (void)W;
    vec_read(v, s1, lanes, A);
    if (ea->is_reg) vec_read(v, ea->reg, lanes, B);
    else {
        rr = sse_load_vec(vm, v, run, ea, lanes, B);
        if (rr) return 1;
    }
    switch (op) {
        case 0x60:
        case 0x61:
        case 0x62:
        case 0x68:
        case 0x69:
        case 0x6A: {
            int el = op == 0x60 || op == 0x68 ? 0 : (op == 0x61 || op == 0x69 ? 1 : 2);
            int hi = op >= 0x68;
            int h;
            int k;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (el == 0) {
                uint8_t *pa = (uint8_t *)A;
                uint8_t *pb = (uint8_t *)B;
                uint8_t *pr = (uint8_t *)R;

                for (h = 0; h < lanes / 4; h++) {
                    for (k = 0; k < 8; k++) {
                        int base = hi ? 8 : 0;

                        pr[h * 16 + k * 2] = pa[h * 16 + base + k];
                        pr[h * 16 + k * 2 + 1] = pb[h * 16 + base + k];
                    }
                }
            } else if (el == 1) {
                uint16_t *pa = (uint16_t *)A;
                uint16_t *pb = (uint16_t *)B;
                uint16_t *pr = (uint16_t *)R;
                int h;
                int k;

                for (h = 0; h < lanes / 4; h++) {
                    for (k = 0; k < 4; k++) {
                        int base = hi ? 4 : 0;

                        pr[h * 8 + k * 2] = pa[h * 8 + base + k];
                        pr[h * 8 + k * 2 + 1] = pb[h * 8 + base + k];
                    }
                }
            } else {
                int h;
                int k;
                int base = hi ? 2 : 0;

                for (h = 0; h < lanes / 4; h++) {
                    for (k = 0; k < 2; k++) {
                        R[h * 4 + k * 2] = A[h * 4 + base + k];
                        R[h * 4 + k * 2 + 1] = B[h * 4 + base + k];
                    }
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x64:
        case 0x65:
        case 0x66:
        case 0x74:
        case 0x75:
        case 0x76: {
            int el;
            int which;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (op <= 0x66) {
                el = op - 0x64;
                which = 3;
            } else {
                el = op - 0x74;
                which = 2;
            }
            if (el == 0) {
                uint8_t *pa = (uint8_t *)A;
                uint8_t *pb = (uint8_t *)B;
                uint8_t *pr = (uint8_t *)R;
                int k;

                for (k = 0; k < 4 * lanes; k++) {
                    if (which == 2) pr[k] = pa[k] == pb[k] ? 0xFF : 0;
                    else pr[k] = (int8_t)pa[k] > (int8_t)pb[k] ? 0xFF : 0;
                }
            } else if (el == 1) {
                uint16_t *pa = (uint16_t *)A;
                uint16_t *pb = (uint16_t *)B;
                uint16_t *pr = (uint16_t *)R;
                int k;

                for (k = 0; k < 2 * lanes; k++) {
                    if (which == 2) pr[k] = pa[k] == pb[k] ? 0xFFFF : 0;
                    else pr[k] = (int16_t)pa[k] > (int16_t)pb[k] ? 0xFFFF : 0;
                }
            } else {
                int k;

                for (k = 0; k < lanes; k++) {
                    if (which == 2) R[k] = A[k] == B[k] ? 0xFFFFFFFFu : 0;
                    else R[k] = (int32_t)A[k] > (int32_t)B[k] ? 0xFFFFFFFFu : 0;
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xFC:
        case 0xFD:
        case 0xFE:
        case 0xF8:
        case 0xF9:
        case 0xFA:
        case 0xFB:
        case 0xD4: {
            int el;
            int sub = op == 0xF8 || op == 0xF9 || op == 0xFA || op == 0xFB;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (op <= 0xFE && op >= 0xFC) el = op - 0xFC;
            else if (op == 0xD4) el = 3;
            else el = op - 0xF8;
            if (el == 0) {
                uint8_t *pa = (uint8_t *)A;
                uint8_t *pb = (uint8_t *)B;
                uint8_t *pr = (uint8_t *)R;
                int k;

                for (k = 0; k < 4 * lanes; k++)
                    pr[k] = sub ? pa[k] - pb[k] : pa[k] + pb[k];
            } else if (el == 1) {
                uint16_t *pa = (uint16_t *)A;
                uint16_t *pb = (uint16_t *)B;
                uint16_t *pr = (uint16_t *)R;
                int k;

                for (k = 0; k < 2 * lanes; k++)
                    pr[k] = sub ? pa[k] - pb[k] : pa[k] + pb[k];
            } else if (el == 2) {
                int k;

                for (k = 0; k < lanes; k++)
                    R[k] = sub ? A[k] - B[k] : A[k] + B[k];
            } else {
                int k;

                for (k = 0; k < lanes / 2; k++) {
                    uint64_t x = (uint64_t)A[k * 2] | ((uint64_t)A[k * 2 + 1] << 32);
                    uint64_t y = (uint64_t)B[k * 2] | ((uint64_t)B[k * 2 + 1] << 32);
                    uint64_t r = sub ? x - y : x + y;

                    R[k * 2] = (uint32_t)r;
                    R[k * 2 + 1] = (uint32_t)(r >> 32);
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xD5: {
            uint16_t *pa = (uint16_t *)A;
            uint16_t *pb = (uint16_t *)B;
            int k;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            for (k = 0; k < 2 * lanes; k++) {
                uint32_t p = (uint32_t)pa[k] * pb[k];

                R[k] = p >> 16;
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xF4: {
            int k;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            for (k = 0; k < lanes / 2; k++) {
                uint64_t p = (uint64_t)A[k * 2] * B[k * 2];

                R[k * 2] = (uint32_t)p;
                R[k * 2 + 1] = (uint32_t)(p >> 32);
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xF5: {
            int16_t *pa = (int16_t *)A;
            int16_t *pb = (int16_t *)B;
            int k;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            for (k = 0; k < lanes; k++)
                R[k] = (uint32_t)((int32_t)pa[k * 2] * pb[k * 2] +
                                  (int32_t)pa[k * 2 + 1] * pb[k * 2 + 1]);
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xF6: {
            uint8_t *pa = (uint8_t *)A;
            uint8_t *pb = (uint8_t *)B;
            int h;
            int k;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            for (h = 0; h < lanes / 4; h++) {
                for (k = 0; k < 2; k++) {
                    unsigned s = 0;
                    int j;

                    for (j = 0; j < 8; j++) {
                        int d = (int)pa[h * 16 + k * 8 + j] - (int)pb[h * 16 + k * 8 + j];

                        s += (unsigned)(d < 0 ? -d : d);
                    }
                    R[h * 4 + k * 2] = s;
                    R[h * 4 + k * 2 + 1] = 0;
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xE0:
        case 0xE3: {
            uint8_t *pa = (uint8_t *)A;
            uint8_t *pb = (uint8_t *)B;
            uint8_t *pr = (uint8_t *)R;
            int k;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            for (k = 0; k < 4 * lanes; k++)
                pr[k] = (uint8_t)(((uint16_t)pa[k] + pb[k] + 1) >> 1);
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xE4:
        case 0xE5: {
            uint16_t *pa = (uint16_t *)A;
            uint16_t *pb = (uint16_t *)B;
            int k;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            for (k = 0; k < 2 * lanes; k++) {
                uint32_t p = (uint32_t)pa[k] * pb[k];

                if (op == 0xE4) R[k] = p >> 16;
                else R[k] = (uint32_t)(((int32_t)p) >> 16);
            }
            {
                uint32_t t[8];
                int j;

                for (j = 0; j < lanes; j++)
                    t[j] = R[j * 2] | (R[j * 2 + 1] << 16);
                vec_write(v, reg, lanes, t, 1);
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xE8:
        case 0xE9:
        case 0xEC:
        case 0xED: {
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (op == 0xE8 || op == 0xEC) {
                uint8_t *pa = (uint8_t *)A;
                uint8_t *pb = (uint8_t *)B;
                uint8_t *pr = (uint8_t *)R;
                int k;

                for (k = 0; k < 4 * lanes; k++) {
                    if (op == 0xE8) {
                        int t = (int)(int8_t)pa[k] - (int)(int8_t)pb[k];

                        pr[k] = (uint8_t)(t < -128 ? -128 : (t > 127 ? 127 : t));
                    } else {
                        unsigned t = (unsigned)pa[k] + pb[k];

                        pr[k] = (uint8_t)(t > 255 ? 255 : t);
                    }
                }
            } else {
                uint16_t *pa = (uint16_t *)A;
                uint16_t *pb = (uint16_t *)B;
                uint16_t *pr = (uint16_t *)R;
                int k;

                for (k = 0; k < 2 * lanes; k++) {
                    if (op == 0xE9) {
                        int t = (int)(int16_t)pa[k] - (int)(int16_t)pb[k];

                        pr[k] = (uint16_t)(t < -32768 ? -32768 : (t > 32767 ? 32767 : t));
                    } else {
                        unsigned t = (unsigned)pa[k] + pb[k];

                        pr[k] = (uint16_t)(t > 65535 ? 65535 : t);
                    }
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xEA:
        case 0xEE: {
            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            {
                uint16_t *pa = (uint16_t *)A;
                uint16_t *pb = (uint16_t *)B;
                uint16_t *pr = (uint16_t *)R;
                int k;

                for (k = 0; k < 2 * lanes; k++) {
                    if (op == 0xEA) pr[k] = pa[k] < pb[k] ? pa[k] : pb[k];
                    else pr[k] = pa[k] > pb[k] ? pa[k] : pb[k];
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xD1:
        case 0xD2:
        case 0xD3: {
            uint64_t cnt = (uint64_t)B[0] | ((uint64_t)B[1] << 32);
            int el = op - 0xD1;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            for (i = 0; i < lanes; i++)
                R[i] = A[i];
            if (el == 0) {
                uint16_t *pr = (uint16_t *)R;
                int k;

                if (cnt > 15) cnt = 16;
                for (k = 0; k < 2 * lanes; k++)
                    pr[k] >>= (int)cnt;
            } else if (el == 1) {
                if (cnt > 31) cnt = 32;
                for (i = 0; i < lanes; i++)
                    R[i] >>= (int)cnt;
            } else {
                if (cnt > 63) cnt = 64;
                for (i = 0; i < lanes / 2; i++) {
                    uint64_t x = (uint64_t)R[i * 2] | ((uint64_t)R[i * 2 + 1] << 32);
                    uint64_t o = cnt >= 64 ? 0 : x >> cnt;

                    R[i * 2] = (uint32_t)o;
                    R[i * 2 + 1] = (uint32_t)(o >> 32);
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xE1:
        case 0xE2: {
            uint64_t cnt = (uint64_t)B[0] | ((uint64_t)B[1] << 32);
            int el = op - 0xE1;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            for (i = 0; i < lanes; i++)
                R[i] = A[i];
            if (el == 0) {
                uint16_t *pr = (uint16_t *)R;
                int k;

                if (cnt > 15) cnt = 16;
                for (k = 0; k < 2 * lanes; k++)
                    pr[k] = (uint16_t)((int16_t)pr[k] >> (int)cnt);
            } else {
                if (cnt > 31) cnt = 32;
                for (i = 0; i < lanes; i++)
                    R[i] = (uint32_t)((int32_t)R[i] >> (int)cnt);
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x71:
        case 0x72:
        case 0x73: {
            int imm;
            int grp;
            int el = op - 0x71;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (*pos + 1 > 15) return vfl_exit_internal(v, run);
            grp = reg & 7;
            imm = buf[*pos];
            (*pos)++;
            n = *pos;
            for (i = 0; i < lanes; i++)
                R[i] = A[i];
            if (el == 2 && (grp == 3 || grp == 7)) {
                uint8_t *pr = (uint8_t *)R;
                int h;
                int k;

                for (h = 0; h < lanes / 4; h++) {
                    if (grp == 3) {
                        for (k = 0; k < 16; k++)
                            pr[h * 16 + k] = k + imm < 16 ? ((uint8_t *)A)[h * 16 + k + imm] : 0;
                    } else {
                        for (k = 15; k >= 0; k--)
                            pr[h * 16 + k] = k - imm >= 0 ? ((uint8_t *)A)[h * 16 + k - imm] : 0;
                    }
                }
            } else {
                if (grp != 2 && grp != 4 && grp != 6)
                    return vfl_exit_exception(v, run, 6, 0);
                if (el == 0) {
                    uint16_t *pr = (uint16_t *)R;
                    int k;

                    if (imm > 15) imm = 16;
                    for (k = 0; k < 2 * lanes; k++) {
                        if (grp == 2) pr[k] >>= imm;
                        else if (grp == 4) pr[k] = (uint16_t)((int16_t)pr[k] >> imm);
                        else pr[k] <<= imm;
                    }
                } else if (el == 1) {
                    if (imm > 31) imm = 32;
                    for (i = 0; i < lanes; i++) {
                        if (grp == 2) R[i] >>= imm;
                        else if (grp == 4) R[i] = (uint32_t)((int32_t)R[i] >> imm);
                        else R[i] <<= imm;
                    }
                } else {
                    if (imm > 63) imm = 64;
                    for (i = 0; i < lanes / 2; i++) {
                        uint64_t x = (uint64_t)R[i * 2] | ((uint64_t)R[i * 2 + 1] << 32);
                        uint64_t o;

                        if (grp == 2) o = imm >= 64 ? 0 : x >> imm;
                        else if (grp == 4) o = imm >= 64 ? ((int64_t)x < 0 ? ~0ULL : 0) : (uint64_t)((int64_t)x >> imm);
                        else o = imm >= 64 ? 0 : x << imm;
                        R[i * 2] = (uint32_t)o;
                        R[i * 2 + 1] = (uint32_t)(o >> 32);
                    }
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x70: {
            int imm;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (*pos + 1 > 15) return vfl_exit_internal(v, run);
            imm = buf[*pos];
            (*pos)++;
            n = *pos;
            for (i = 0; i < lanes; i++)
                R[i] = B[(i & ~3) + ((imm >> ((i & 3) * 2)) & 3)];
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xC4:
        case 0xC5: {
            int imm;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (*pos + 1 > 15) return vfl_exit_internal(v, run);
            imm = buf[*pos];
            (*pos)++;
            n = *pos;
            if (op == 0xC4) {
                uint16_t w = (uint16_t)B[0];

                for (i = 0; i < lanes; i++)
                    R[i] = A[i];
                ((uint16_t *)R)[imm & (2 * lanes - 1)] = w;
            } else {
                uint16_t val = (uint16_t)(((uint16_t *)B)[imm & (2 * lanes - 1)]);

                vfl_write_reg(v, reg, 0, 4, val);
            }
            if (op == 0xC4) vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x14:
        case 0x15: {
            int el = pp == 1 ? 1 : 0;
            int hi = op == 0x15;
            int h;
            int k;

            if (pp != 0 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (el == 0) {
                for (h = 0; h < lanes / 4; h++) {
                    for (k = 0; k < 2; k++) {
                        R[h * 4 + k * 2] = A[h * 4 + (hi ? 2 : 0) + k];
                        R[h * 4 + k * 2 + 1] = B[h * 4 + (hi ? 2 : 0) + k];
                    }
                }
            } else {
                for (h = 0; h < lanes / 4; h++) {
                    uint64_t x = (uint64_t)A[h * 4 + (hi ? 2 : 0)] |
                                 ((uint64_t)A[h * 4 + (hi ? 2 : 0) + 1] << 32);
                    uint64_t y = (uint64_t)B[h * 4 + (hi ? 2 : 0)] |
                                 ((uint64_t)B[h * 4 + (hi ? 2 : 0) + 1] << 32);

                    R[h * 4] = (uint32_t)x;
                    R[h * 4 + 1] = (uint32_t)(x >> 32);
                    R[h * 4 + 2] = (uint32_t)y;
                    R[h * 4 + 3] = (uint32_t)(y >> 32);
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x50: {
            uint64_t mask = 0;

            if (pp != 0 && pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (pp == 0) {
                for (i = 0; i < lanes; i++)
                    if ((B[i] >> 31) & 1) mask |= 1ULL << i;
            } else {
                for (i = 0; i < lanes / 2; i++) {
                    uint64_t q = (uint64_t)B[i * 2] | ((uint64_t)B[i * 2 + 1] << 32);

                    if ((q >> 63) & 1) mask |= 1ULL << i;
                }
            }
            vfl_write_reg(v, reg, 0, 4, mask);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x0E:
        case 0x0F: {
            if (pp != 0) return vfl_exit_exception(v, run, 6, 0);
            if (op == 0x0E) {
                uint64_t andv = 0;
                uint64_t nand = 0;

                for (i = 0; i < lanes / 2; i++) {
                    uint64_t x = (uint64_t)A[i * 2] | ((uint64_t)A[i * 2 + 1] << 32);
                    uint64_t y = (uint64_t)B[i * 2] | ((uint64_t)B[i * 2 + 1] << 32);

                    if ((x & y) == 0) andv |= 1ULL << i;
                    if ((~x & y) == 0) nand |= 1ULL << i;
                }
                v->regs.rflags &= ~((1ULL << 0) | (1ULL << 2) | (1ULL << 4) |
                                    (1ULL << 6) | (1ULL << 7) | (1ULL << 11));
                if (andv == (uint64_t)((1ULL << (lanes / 2)) - 1))
                    v->regs.rflags |= (1ULL << 6);
                if (nand == (uint64_t)((1ULL << (lanes / 2)) - 1))
                    v->regs.rflags |= (1ULL << 0);
            } else {
                uint64_t andv = 0;
                uint64_t nand = 0;

                for (i = 0; i < lanes; i++) {
                    if ((A[i] & B[i]) == 0) andv |= 1ULL << i;
                    if ((~A[i] & B[i]) == 0) nand |= 1ULL << i;
                }
                v->regs.rflags &= ~((1ULL << 0) | (1ULL << 2) | (1ULL << 4) |
                                    (1ULL << 6) | (1ULL << 7) | (1ULL << 11));
                if (andv == (lanes == 8 ? 0xFFULL : 0xFULL))
                    v->regs.rflags |= (1ULL << 6);
                if (nand == (lanes == 8 ? 0xFFULL : 0xFULL))
                    v->regs.rflags |= (1ULL << 0);
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        default:
            break;
    }
    return vfl_vex_more3(vm, v, run, buf, pos, op, pp, W, L, lanes, s1,
                         reg, ea, n, ripmask);
}

static int vfl_vex_more3(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                         int *pos, int op, int pp, int W, int L, int lanes, int s1,
                         int reg, vfl_ea_t *ea, int n, uint64_t ripmask);

static int vfl_vex38_3a(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                        int *pos, int mmmmm, uint8_t op3, int vvvv, int L, int pp,
                        int W, int asz, int seg, int rxb, int rxr, int rxx,
                        uint64_t ripmask) {
    int mod;
    int reg;
    int rm;
    vfl_ea_t ea;
    int n;
    int rr;
    int i;
    int lanes = L ? 8 : 4;
    uint32_t A[8];
    uint32_t B[8];
    uint32_t R[8];

    (void)W;
    if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
    if (*pos >= 15) return vfl_exit_internal(v, run);
    {
        uint8_t m = buf[*pos];

        (*pos)++;
        mod = (m >> 6) & 3;
        reg = ((m >> 3) & 7) | rxr;
        rm = (m & 7) | rxb;
    }
    if (vfl_decode_ea(v, buf, pos, mod, rm, asz, seg, rxb & 1, rxx & 1, &ea) < 0)
        return vfl_exit_internal(v, run);
    n = *pos;
    if (!ea.is_reg && ea.rip_rel)
        ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
    if (mmmmm == 3) {
        int imm = 0;

        vec_read(v, vvvv, lanes, A);
        if (ea.is_reg) vec_read(v, ea.reg, lanes, B);
        else if (op3 < 0x16 || op3 > 0x19) {
            rr = sse_load_vec(vm, v, run, &ea, lanes, B);
            if (rr) return 1;
        }
        if (op3 == 0x04 || op3 == 0x05) {
            int k;
            int h;

            n = *pos;
            for (h = 0; h < lanes / 4; h++) {
                for (k = 0; k < 4; k++) {
                    if (op3 == 0x04) {
                        R[h * 4 + k] = B[h * 4 + (A[h * 4 + k] & 3)];
                    } else {
                        int q = k / 2;
                        int sel = (A[h * 4 + q * 2] >> 1) & 1;

                        R[h * 4 + k] = B[h * 4 + (q ^ sel) * 2 + (k & 1)];
                    }
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }
        if (*pos + 1 > 15) return vfl_exit_internal(v, run);
        imm = buf[*pos];
        (*pos)++;
        n = *pos;
        switch (op3) {
            case 0x0E: {
                uint16_t *pa = (uint16_t *)A;
                uint16_t *pb = (uint16_t *)B;
                uint16_t *pr = (uint16_t *)R;
                int k;

                for (k = 0; k < 2 * lanes; k++)
                    pr[k] = (imm >> k) & 1 ? pb[k] : pa[k];
                break;
            }

            case 0x0F: {
                uint8_t *pa = (uint8_t *)A;
                uint8_t *pb = (uint8_t *)B;
                uint8_t *pr = (uint8_t *)R;
                int sh = imm & 31;
                int h;
                int k;

                for (h = 0; h < lanes / 4; h++) {
                    for (k = 0; k < 16; k++) {
                        int s = k + sh;

                        pr[h * 16 + k] = s < 16 ? pb[h * 16 + s] : (s < 32 ? pa[h * 16 + s - 16] : 0);
                    }
                }
                break;
            }

            case 0x14: {
                uint8_t val = ((uint8_t *)B)[imm & 15];

                if (mod == 3) {
                    vfl_write_reg(v, reg, 0, 4, val);
                } else {
                    rr = vfl_mem_write(vm, v, ea.addr, 1, &val);
                    if (rr) {
                        vfl_memerr(v, run, rr);
                        return 1;
                    }
                }
                vfl_adv(v, n, ripmask);
                return 0;
            }

            case 0x16: {
                uint64_t val = 0;

                if (W) {
                    int lane = imm & 1;

                    val = (uint64_t)B[lane * 2] | ((uint64_t)B[lane * 2 + 1] << 32);
                } else {
                    val = B[imm & 3];
                }
                if (mod == 3) {
                    vfl_write_reg(v, reg, 0, W ? 8 : 4, val);
                } else {
                    uint8_t wb[8];
                    int sz = W ? 8 : 4;

                    vfl_put64(wb, val, sz);
                    rr = vfl_mem_write(vm, v, ea.addr, (uint64_t)sz, wb);
                    if (rr) {
                        vfl_memerr(v, run, rr);
                        return 1;
                    }
                }
                vfl_adv(v, n, ripmask);
                return 0;
            }

            case 0x17: {
                uint32_t val = B[imm & 3];

                if (mod == 3) {
                    vfl_write_reg(v, reg, 0, 4, val);
                } else {
                    uint8_t wb[4];

                    vfl_put64(wb, val, 4);
                    rr = vfl_mem_write(vm, v, ea.addr, 4, wb);
                    if (rr) {
                        vfl_memerr(v, run, rr);
                        return 1;
                    }
                }
                vfl_adv(v, n, ripmask);
                return 0;
            }

            case 0x20: {
                uint8_t src = 0;

                if (ea.is_reg) src = (uint8_t)vfl_read_reg(v, ea.reg, 0, 4);
                else {
                    rr = vfl_mem_read(vm, v, ea.addr, 1, &src);
                    if (rr) {
                        vfl_memerr(v, run, rr);
                        return 1;
                    }
                }
                for (i = 0; i < lanes; i++)
                    R[i] = A[i];
                ((uint8_t *)R)[imm & 15] = src;
                break;
            }

            case 0x22: {
                uint64_t gv = 0;

                if (ea.is_reg) gv = vfl_read_reg(v, ea.reg, 0, W ? 8 : 4);
                else {
                    uint8_t tmp[8] = {0, 0, 0, 0, 0, 0, 0, 0};

                    if (sse_mem_fill(vm, v, run, ea.addr, tmp, W ? 8 : 4)) return 1;
                    memcpy(&gv, tmp, W ? 8 : 4);
                }
                for (i = 0; i < lanes; i++)
                    R[i] = A[i];
                if (W) {
                    int lane = imm & 1;

                    R[lane * 2] = (uint32_t)gv;
                    R[lane * 2 + 1] = (uint32_t)(gv >> 32);
                } else {
                    R[imm & 3] = (uint32_t)gv;
                }
                break;
            }

            case 0x21: {
                uint32_t s = B[(imm >> 6) & 3];
                int dc = (imm >> 4) & 3;
                int zm = imm & 15;

                for (i = 0; i < lanes; i++)
                    R[i] = A[i];
                R[dc] = s;
                for (i = 0; i < 4; i++)
                    if ((zm >> i) & 1) R[i] = 0;
                break;
            }

            case 0x40:
            case 0x41: {
                int k;

                if (op3 == 0x40) {
                    float dot = 0;

                    for (k = 0; k < lanes; k++)
                        if ((imm >> (4 + k)) & 1) dot += vfl_u32f(A[k]) * vfl_u32f(B[k]);
                    for (k = 0; k < lanes; k++)
                        R[k] = (imm >> k) & 1 ? fu32(dot) : 0;
                } else {
                    double dot = 0;

                    for (k = 0; k < lanes / 2; k++) {
                        uint64_t au = (uint64_t)A[k * 2] | ((uint64_t)A[k * 2 + 1] << 32);
                        uint64_t bu = (uint64_t)B[k * 2] | ((uint64_t)B[k * 2 + 1] << 32);

                        if ((imm >> (4 + k)) & 1) dot += vfl_u64d(au) * vfl_u64d(bu);
                    }
                    for (k = 0; k < lanes / 2; k++) {
                        uint64_t o = 0;

                        if ((imm >> k) & 1) memcpy(&o, &dot, 8);
                        R[k * 2] = (uint32_t)o;
                        R[k * 2 + 1] = (uint32_t)(o >> 32);
                    }
                }
                break;
            }

            case 0x44: {
                uint64_t x;
                uint64_t y;
                __uint128_t p0 = 0;
                int k;

                x = (uint64_t)A[0] | ((uint64_t)A[1] << 32);
                y = (uint64_t)B[0] | ((uint64_t)B[1] << 32);
                if (imm & 1) x >>= 32;
                if (imm & 2) y >>= 32;
                for (k = 0; k < 64; k++) {
                    if ((y >> k) & 1)
                        p0 ^= (__uint128_t)x << k;
                }
                R[0] = (uint32_t)(uint64_t)p0;
                R[1] = (uint32_t)((uint64_t)p0 >> 32);
                R[2] = (uint32_t)((uint64_t)(p0 >> 64));
                R[3] = (uint32_t)((uint64_t)(p0 >> 64) >> 32);
                break;
            }

            case 0x1D: {
                int rc = imm & 3;
                int k;

                for (k = 0; k < lanes; k++) {
                    float x = vfl_u32f(B[k]);
                    uint32_t u;
                    uint16_t h;

                    memcpy(&u, &x, 4);
                    {
                        int s = (u >> 31) & 1;
                        int e = ((u >> 23) & 255) - 112;
                        int m = u & 0x7FFFFF;

                        if (((u >> 23) & 255) == 0) {
                            h = 0;
                        } else if (((u >> 23) & 255) == 255) {
                            h = (s << 15) | 0x7C00 | (m ? 0x0200 : 0);
                        } else if (e >= 31) {
                            h = (s << 15) | 0x7C00;
                        } else if (e <= 0) {
                            if (e < -10) {
                                h = 0;
                            } else {
                                m |= 0x800000;
                                m >>= 1 - e;
                                if (rc == 0) {
                                    m += 0xFFF + ((m >> 13) & 1);
                                } else if (rc == 2) {
                                    m += 0x1FFF;
                                }
                                h = (m >> 13) & 0x3FF;
                            }
                        } else {
                            if (rc == 0) {
                                m += 0xFFF + ((m >> 13) & 1);
                                if (m & 0x800000) {
                                    m = 0;
                                    e++;
                                    if (e >= 31) {
                                        h = (s << 15) | 0x7C00;
                                        R[k] = h;
                                        continue;
                                    }
                                }
                            } else if (rc == 2) {
                                m += 0x1FFF;
                            }
                            h = (s << 15) | (e << 10) | ((m >> 13) & 0x3FF);
                        }
                    }
                    R[k] = h;
                }
                vec_write(v, reg, lanes, R, 1);
                vfl_adv(v, n, ripmask);
                return 0;
            }

            case 0x0C:
            case 0x0D: {
                int k;
                int h;

                for (h = 0; h < lanes / 4; h++) {
                    for (k = 0; k < 4; k++) {
                        if (op3 == 0x0C) {
                            R[h * 4 + k] = B[h * 4 + ((imm >> (k * 2)) & 3)];
                        } else {
                            int q = k / 2;
                            int sel = (imm >> (h * 2 + q)) & 1;

                            R[h * 4 + k] = B[h * 4 + (q ^ sel) * 2 + (k & 1)];
                        }
                    }
                }
                break;
            }

            case 0x06: {
                int h;

                for (h = 0; h < lanes / 4; h++) {
                    int s0 = (imm >> (h * 4)) & 3;
                    int s1 = (imm >> (h * 4 + 2)) & 3;
                    int k;

                    for (k = 0; k < 2; k++) {
                        uint32_t v0;
                        uint32_t v1;

                        if (s0 < 2) {
                            v0 = A[h * 4 + s0 * 2 + k];
                        } else {
                            v0 = B[h * 4 + (s0 - 2) * 2 + k];
                        }
                        if (s1 < 2) {
                            v1 = A[h * 4 + s1 * 2 + k];
                        } else {
                            v1 = B[h * 4 + (s1 - 2) * 2 + k];
                        }
                        R[h * 4 + k] = v0;
                        R[h * 4 + 2 + k] = v1;
                    }
                }
                break;
            }

            case 0x19: {
                int sel = imm & 1;
                uint32_t S[8];
                int k;

                if (!L) return vfl_exit_exception(v, run, 6, 0);
                vec_read(v, reg, 8, S);
                if (mod == 3) {
                    uint32_t t[8] = {0, 0, 0, 0, 0, 0, 0, 0};

                    if (sel == 0) {
                        for (k = 0; k < 4; k++)
                            t[k] = S[k];
                    } else {
                        for (k = 0; k < 4; k++)
                            t[k] = S[4 + k];
                    }
                    vec_write(v, ea.reg, 8, t, 1);
                } else {
                    uint8_t wb[16];

                    if (sel == 0) {
                        for (k = 0; k < 4; k++)
                            memcpy(wb + k * 4, &S[k], 4);
                    } else {
                        for (k = 0; k < 4; k++)
                            memcpy(wb + k * 4, &S[4 + k], 4);
                    }
                    rr = vfl_mem_write(vm, v, ea.addr, 16, wb);
                    if (rr == -2) return vfl_mem_fault(v, run);
                    if (rr == 1) {
                        uint64_t phys;

                        if (vfl_translate(vm, v, ea.addr, VFL_MMU_WRITE, &phys))
                            return vfl_mem_fault(v, run);
                        return vfl_exit_mmio_write64(v, run, phys, wb, 16,
                                                     VFL_P_NONE, 0, n, ripmask);
                    }
                    if (rr < 0) return vfl_exit_exception(v, run, 13, 0);
                }
                vfl_adv(v, n, ripmask);
                return 0;
            }

            case 0x18: {
                if (!L) return vfl_exit_exception(v, run, 6, 0);
                {
                    uint32_t t[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                    int k;

                    if (ea.is_reg) {
                        uint32_t q[4];

                        vec_read(v, ea.reg, 4, q);
                        for (k = 0; k < 4; k++)
                            t[k] = q[k];
                    } else {
                        uint8_t mb[16];

                        rr = vfl_mem_read(vm, v, ea.addr, 16, mb);
                        if (rr == -2) return vfl_mem_fault(v, run);
                        if (rr == 1) return vfl_mmio_fill_exit(vm, v, run, ea.addr, 16);
                        if (rr < 0) return vfl_exit_exception(v, run, 13, 0);
                        for (k = 0; k < 4; k++)
                            memcpy(&t[k], mb + k * 4, 4);
                    }
                    if (imm & 1) {
                        for (k = 0; k < 4; k++)
                            R[k] = A[k];
                        for (k = 0; k < 4; k++)
                            R[4 + k] = t[k];
                    } else {
                        for (k = 0; k < 4; k++)
                            R[k] = t[k];
                        for (k = 0; k < 4; k++)
                            R[4 + k] = A[4 + k];
                    }
                }
                break;
            }

            default:
                return vfl_exit_exception(v, run, 6, 0);
        }
        vec_write(v, reg, lanes, R, 1);
        vfl_adv(v, n, ripmask);
        return 0;
    }
    vec_read(v, vvvv, lanes, A);
    if (ea.is_reg) vec_read(v, ea.reg, lanes, B);
    else {
        rr = sse_load_vec(vm, v, run, &ea, lanes, B);
        if (rr) return 1;
    }
    if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
    switch (op3) {
        case 0x00: {
            uint8_t *pa = (uint8_t *)A;
            uint8_t *pb = (uint8_t *)B;
            uint8_t *pr = (uint8_t *)R;
            int h;
            int k;

            for (h = 0; h < lanes / 4; h++) {
                for (k = 0; k < 16; k++)
                    pr[h * 16 + k] = (pb[h * 16 + k] & 0x80) ? 0 : pa[h * 16 + (pb[h * 16 + k] & 0xF)];
            }
            break;
        }

        case 0x01:
        case 0x02:
        case 0x05:
        case 0x06: {
            int sub = op3 == 0x05 || op3 == 0x06;
            int isd = op3 == 0x02 || op3 == 0x06;
            int h;
            int k;

            if (!isd) {
                int16_t *pa = (int16_t *)A;
                int16_t *pb = (int16_t *)B;
                int16_t *pr = (int16_t *)R;

                for (h = 0; h < lanes / 4; h++) {
                    for (k = 0; k < 4; k++) {
                        if (!sub) {
                            pr[h * 8 + k] = pa[h * 8 + k * 2] + pa[h * 8 + k * 2 + 1];
                            pr[h * 8 + 4 + k] = pb[h * 8 + k * 2] + pb[h * 8 + k * 2 + 1];
                        } else {
                            pr[h * 8 + k] = pa[h * 8 + k * 2] - pa[h * 8 + k * 2 + 1];
                            pr[h * 8 + 4 + k] = pb[h * 8 + k * 2] - pb[h * 8 + k * 2 + 1];
                        }
                    }
                }
            } else {
                int32_t *pa = (int32_t *)A;
                int32_t *pb = (int32_t *)B;
                int32_t *pr = (int32_t *)R;

                for (h = 0; h < lanes / 4; h++) {
                    for (k = 0; k < 2; k++) {
                        if (!sub) {
                            pr[h * 4 + k] = pa[h * 4 + k * 2] + pa[h * 4 + k * 2 + 1];
                            pr[h * 4 + 2 + k] = pb[h * 4 + k * 2] + pb[h * 4 + k * 2 + 1];
                        } else {
                            pr[h * 4 + k] = pa[h * 4 + k * 2] - pa[h * 4 + k * 2 + 1];
                            pr[h * 4 + 2 + k] = pb[h * 4 + k * 2] - pb[h * 4 + k * 2 + 1];
                        }
                    }
                }
            }
            break;
        }

        case 0x14: {
            uint32_t m[8];
            int k;

            vec_read(v, 0, lanes, m);
            for (k = 0; k < lanes; k++) {
                uint8_t *pa = (uint8_t *)A;
                uint8_t *pb = (uint8_t *)B;
                uint8_t *pm = (uint8_t *)m;

                R[k] = 0;
                (void)pa;
                (void)pb;
                (void)pm;
                {
                    int j;

                    for (j = 0; j < 4; j++)
                        ((uint8_t *)&R[k])[j] = (pm[k * 4 + j] & 0x80) ? pb[k * 4 + j] : pa[k * 4 + j];
                }
            }
            break;
        }

        case 0x17: {
            uint64_t andv = 0;
            uint64_t nand = 0;

            for (i = 0; i < lanes / 2; i++) {
                uint64_t x = (uint64_t)A[i * 2] | ((uint64_t)A[i * 2 + 1] << 32);
                uint64_t y = (uint64_t)B[i * 2] | ((uint64_t)B[i * 2 + 1] << 32);

                if ((x & y) == 0) andv |= 1ULL << i;
                if ((~x & y) == 0) nand |= 1ULL << i;
            }
            v->regs.rflags &= ~((1ULL << 0) | (1ULL << 2) | (1ULL << 4) |
                                (1ULL << 6) | (1ULL << 7) | (1ULL << 11));
            if (andv == (uint64_t)((1ULL << (lanes / 2)) - 1))
                v->regs.rflags |= (1ULL << 6);
            if (nand == (uint64_t)((1ULL << (lanes / 2)) - 1))
                v->regs.rflags |= (1ULL << 0);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x29: {
            int k;

            for (k = 0; k < lanes / 2; k++) {
                uint64_t x = (uint64_t)A[k * 2] | ((uint64_t)A[k * 2 + 1] << 32);
                uint64_t y = (uint64_t)B[k * 2] | ((uint64_t)B[k * 2 + 1] << 32);
                uint64_t m = x == y ? ~0ULL : 0;

                R[k * 2] = (uint32_t)m;
                R[k * 2 + 1] = (uint32_t)(m >> 32);
            }
            break;
        }

        case 0x1C:
        case 0x1D:
        case 0x1E:
        case 0x20:
        case 0x21:
        case 0x22:
        case 0x23:
        case 0x24:
        case 0x25:
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x33:
        case 0x34:
        case 0x35: {
            int ssz;
            int dsz;
            int sgn = op3 < 0x30;
            int h;
            int k;

            ssz = op3 == 0x20 || op3 == 0x30 ? 1 : (op3 == 0x21 || op3 == 0x31 || op3 == 0x23 || op3 == 0x33 ? 2 : 4);
            if (op3 == 0x22 || op3 == 0x32) { ssz = 1; dsz = 8; }
            else if (op3 == 0x24 || op3 == 0x34) { ssz = 2; dsz = 8; }
            else if (op3 == 0x25 || op3 == 0x35) { ssz = 4; dsz = 8; }
            else if (ssz == 1) dsz = 2;
            else dsz = 4;
            if (op3 >= 0x1C && op3 <= 0x1E) {
                if (op3 == 0x1C) {
                    int8_t *p = (int8_t *)B;
                    int8_t *q = (int8_t *)R;

                    for (h = 0; h < lanes / 4; h++)
                        for (k = 0; k < 16; k++)
                            q[h * 16 + k] = p[h * 16 + k] == -128 ? p[h * 16 + k] : (p[h * 16 + k] < 0 ? -p[h * 16 + k] : p[h * 16 + k]);
                } else if (op3 == 0x1D) {
                    int16_t *p = (int16_t *)B;
                    int16_t *q = (int16_t *)R;

                    for (h = 0; h < lanes / 4; h++)
                        for (k = 0; k < 8; k++)
                            q[h * 8 + k] = p[h * 8 + k] == -32768 ? p[h * 8 + k] : (p[h * 8 + k] < 0 ? -p[h * 8 + k] : p[h * 8 + k]);
                } else {
                    int32_t *p = (int32_t *)B;
                    int32_t *q = (int32_t *)R;

                    for (h = 0; h < lanes / 4; h++)
                        for (k = 0; k < 4; k++)
                            q[h * 4 + k] = p[h * 4 + k] == (int32_t)0x80000000 ? p[h * 4 + k] : (p[h * 4 + k] < 0 ? -p[h * 4 + k] : p[h * 4 + k]);
                }
                break;
            }
            for (h = 0; h < lanes / 4; h++) {
                if (ssz == 1 && dsz == 2) {
                    uint8_t *p = (uint8_t *)B + h * 16;
                    uint16_t *q = (uint16_t *)R + h * 8;

                    for (k = 0; k < 8; k++)
                        q[k] = sgn ? (uint16_t)(int16_t)(int8_t)p[k] : p[k];
                } else if (ssz == 1 && dsz == 4) {
                    uint8_t *p = (uint8_t *)B + h * 16;
                    uint32_t *q = R + h * 4;

                    for (k = 0; k < 4; k++)
                        q[k] = sgn ? (uint32_t)(int32_t)(int8_t)p[k] : p[k];
                } else if (ssz == 1) {
                    uint8_t *p = (uint8_t *)B + h * 16;
                    uint64_t *q = (uint64_t *)R + h * 2;

                    for (k = 0; k < 2; k++)
                        q[k] = sgn ? (uint64_t)(int64_t)(int8_t)p[k] : p[k];
                } else if (ssz == 2 && dsz == 4) {
                    uint16_t *p = (uint16_t *)B + h * 8;
                    uint32_t *q = R + h * 4;

                    for (k = 0; k < 4; k++)
                        q[k] = sgn ? (uint32_t)(int32_t)(int16_t)p[k] : p[k];
                } else if (ssz == 2) {
                    uint16_t *p = (uint16_t *)B + h * 8;
                    uint64_t *q = (uint64_t *)R + h * 2;

                    for (k = 0; k < 2; k++)
                        q[k] = sgn ? (uint64_t)(int64_t)(int16_t)p[k] : p[k];
                } else {
                    uint32_t *p = B + h * 4;
                    uint64_t *q = (uint64_t *)R + h * 2;

                    for (k = 0; k < 2; k++)
                        q[k] = sgn ? (uint64_t)(int64_t)(int32_t)p[k] : p[k];
                }
            }
            (void)A;
            break;
        }

        default:
            return vfl_exit_exception(v, run, 6, 0);
    }
    vec_write(v, reg, lanes, R, 1);
    vfl_adv(v, n, ripmask);
    return 0;
}

static int vfl_vex_more3(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                         int *pos, int op, int pp, int W, int L, int lanes, int s1,
                         int reg, vfl_ea_t *ea, int n, uint64_t ripmask) {
    uint32_t A[8];
    uint32_t B[8];
    uint32_t R[8];
    int i;
    int rr;

    (void)W;
    vec_read(v, s1, lanes, A);
    if (ea->is_reg) vec_read(v, ea->reg, lanes, B);
    else {
        rr = sse_load_vec(vm, v, run, ea, lanes, B);
        if (rr) return 1;
    }
    switch (op) {
        case 0x5A:
        case 0x5B: {
            if (op == 0x5A && pp == 0) {
                for (i = 0; i < lanes / 2; i++) {
                    uint64_t u = f32_to_f64(B[i]);

                    R[i * 2] = (uint32_t)u;
                    R[i * 2 + 1] = (uint32_t)(u >> 32);
                }
            } else if (op == 0x5A && pp == 1) {
                for (i = 0; i < lanes; i++) {
                    uint64_t u = (uint64_t)B[i * 2] | ((uint64_t)B[i * 2 + 1] << 32);

                    if (i < lanes / 2) R[i] = f64_to_f32(u);
                    else R[i] = 0;
                }
            } else if (op == 0x5A && pp == 3) {
                for (i = 0; i < lanes; i++)
                    R[i] = A[i];
                {
                    uint64_t u = f32_to_f64(B[0]);

                    R[0] = (uint32_t)u;
                    R[1] = (uint32_t)(u >> 32);
                }
            } else if (op == 0x5A && pp == 2) {
                uint64_t u = (uint64_t)B[0] | ((uint64_t)B[1] << 32);
                uint32_t f = f64_to_f32(u);

                for (i = 0; i < lanes; i++)
                    R[i] = A[i];
                R[0] = f;
            } else if (op == 0x5B && pp == 0) {
                for (i = 0; i < lanes; i++) {
                    float f = (float)(int32_t)B[i];

                    memcpy(&R[i], &f, 4);
                }
            } else if (op == 0x5B && pp == 1) {
                for (i = 0; i < lanes; i++) {
                    int ov = 0;
                    int64_t q = f32_rint_i64(B[i], &ov);

                    if (ov || q != (int64_t)(int32_t)q) R[i] = 0x80000000u;
                    else R[i] = (uint32_t)(int32_t)q;
                }
            } else if (op == 0x5B && pp == 3) {
                for (i = 0; i < lanes; i++) {
                    float x = vfl_u32f(B[i]);

                    if (!(x > -2.1474836e9f) || !(x < 2.1474836e9f)) R[i] = 0x80000000u;
                    else R[i] = (uint32_t)(int32_t)x;
                }
            } else {
                return vfl_exit_exception(v, run, 6, 0);
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xE6: {
            if (pp == 3) {
                for (i = 0; i < lanes / 2; i++) {
                    double f = (double)(int32_t)B[i];
                    uint64_t u;

                    memcpy(&u, &f, 8);
                    R[i * 2] = (uint32_t)u;
                    R[i * 2 + 1] = (uint32_t)(u >> 32);
                }
                for (i = lanes; i < lanes; i++)
                    R[i] = R[i];
            } else if (pp == 1 || pp == 2) {
                for (i = 0; i < lanes / 2; i++) {
                    uint64_t u = (uint64_t)B[i * 2] | ((uint64_t)B[i * 2 + 1] << 32);
                    double x = vfl_u64d(u);

                    if (pp == 1) {
                        if (!(x > -2.1474836e9) || !(x < 2.1474836e9)) R[i] = 0x80000000u;
                        else R[i] = (uint32_t)(int32_t)x;
                    } else {
                        long double q = (long double)x;

                        if (!(q > -2.1474836e9L) || !(q < 2.1474836e9L)) R[i] = 0x80000000u;
                        else R[i] = (uint32_t)(int32_t)q;
                    }
                }
                for (i = lanes / 2; i < lanes; i++)
                    R[i] = 0;
            } else {
                return vfl_exit_exception(v, run, 6, 0);
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x2A: {
            if (pp != 2 && pp != 3) return vfl_exit_exception(v, run, 6, 0);
            {
                int64_t sv = 0;

                if (ea->is_reg) {
                    sv = (int64_t)vfl_read_reg(v, ea->reg, 0, W ? 8 : 4);
                    if (!W) sv = (int32_t)(sv & 0xFFFFFFFFULL);
                } else {
                    uint8_t tmp[8] = {0, 0, 0, 0, 0, 0, 0, 0};

                    if (sse_mem_fill(vm, v, run, ea->addr, tmp, W ? 8 : 4)) return 1;
                    if (W) memcpy(&sv, tmp, 8);
                    else {
                        int32_t t = 0;

                        memcpy(&t, tmp, 4);
                        sv = t;
                    }
                }
                for (i = 0; i < lanes; i++)
                    R[i] = A[i];
                if (pp == 3) {
                    float f = (float)sv;

                    memcpy(&R[0], &f, 4);
                } else {
                    double f = (double)sv;
                    uint64_t u;

                    memcpy(&u, &f, 8);
                    R[0] = (uint32_t)u;
                    R[1] = (uint32_t)(u >> 32);
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x2C:
        case 0x2D: {
            int trunc = op == 0x2C;
            int isd = pp == 2;
            int k;

            if (pp == 0) {
                uint8_t tmp[64];

                if (op != 0x2C) return vfl_exit_exception(v, run, 6, 0);
                if (ea->is_reg) return vfl_exit_exception(v, run, 6, 0);
                rr = vfl_mem_read(vm, v, ea->addr, (uint64_t)lanes * 4, tmp);
                if (rr) {
                    vfl_memerr(v, run, rr);
                    return 1;
                }
                memset(R, 0, sizeof(R));
                for (k = 0; k < lanes; k++) {
                    int j;

                    for (j = 0; j < 4; j++) {
                        if ((A[k] >> 31) & 1)
                            ((uint8_t *)R)[k * 4 + j] = tmp[k * 4 + j];
                    }
                }
                vec_write(v, reg, lanes, R, 1);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (pp != 2 && pp != 3) return vfl_exit_exception(v, run, 6, 0);
            if (isd) {
                uint64_t u = (uint64_t)B[0] | ((uint64_t)B[1] << 32);
                double x = vfl_u64d(u);
                int64_t r = 0;

                if (trunc) {
                    if (!(x > -9.223372036854776e18) || !(x < 9.223372036854776e18))
                        r = W ? (int64_t)0x8000000000000000ULL : 0x80000000;
                    else r = (int64_t)x;
                } else {
                    long double t = f80_round((long double)x);

                    if (!(t > -9.223372036854776e18L) || !(t < 9.223372036854776e18L))
                        r = W ? (int64_t)0x8000000000000000ULL : 0x80000000;
                    else r = (int64_t)t;
                }
                if (W) *vfl_reg64(v, reg) = (uint64_t)r;
                else vfl_write_reg(v, reg, 0, 4, (uint64_t)(uint32_t)r);
            } else {
                float x = vfl_u32f(B[0]);
                int64_t r = 0;

                if (trunc) {
                    if (!W && (!(x > -2.1474836e9f) || !(x < 2.1474836e9f)))
                        r = 0x80000000;
                    else if (!(x > -9.223372036854776e18) || !(x < 9.223372036854776e18))
                        r = W ? (int64_t)0x8000000000000000ULL : 0x80000000;
                    else if (!W) r = (int32_t)x;
                    else r = (int64_t)x;
                } else {
                    long double t = f80_round((long double)x);

                    if (!W && (!(t > -2.1474836e9L) || !(t < 2.1474836e9L)))
                        r = 0x80000000;
                    else if (!(t > -9.223372036854776e18L) || !(t < 9.223372036854776e18L))
                        r = W ? (int64_t)0x8000000000000000ULL : 0x80000000;
                    else if (!W) r = (int32_t)t;
                    else r = (int64_t)t;
                }
                if (W) *vfl_reg64(v, reg) = (uint64_t)r;
                else vfl_write_reg(v, reg, 0, 4, (uint64_t)(uint32_t)r);
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x2E: {
            int k;

            if (pp != 0) return vfl_exit_exception(v, run, 6, 0);
            if (ea->is_reg) return vfl_exit_exception(v, run, 6, 0);
            {
                uint8_t tmp[64];

                rr = vfl_mem_read(vm, v, ea->addr, (uint64_t)lanes * 4, tmp);
                if (rr) {
                    vfl_memerr(v, run, rr);
                    return 1;
                }
                memset(R, 0, sizeof(R));
                for (k = 0; k < lanes / 2; k++) {
                    uint64_t msk = (uint64_t)A[k * 2] | ((uint64_t)A[k * 2 + 1] << 32);
                    int j;

                    for (j = 0; j < 8; j++) {
                        if ((msk >> 63) & 1)
                            ((uint8_t *)R)[k * 8 + j] = tmp[k * 8 + j];
                    }
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x08:
        case 0x09: {
            int imm;
            int isd = op == 0x09;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (*pos + 1 > 15) return vfl_exit_internal(v, run);
            imm = buf[*pos] & 3;
            (*pos)++;
            n = *pos;
            if (!isd) {
                for (i = 0; i < lanes; i++) {
                    float rf;

                    vfl_round_f32(&rf, vfl_u32f(B[i]), imm);
                    R[i] = fu32(rf);
                }
            } else {
                for (i = 0; i < lanes / 2; i++) {
                    uint64_t u = (uint64_t)B[i * 2] | ((uint64_t)B[i * 2 + 1] << 32);
                    double rd;

                    vfl_round_f64(&rd, vfl_u64d(u), imm);
                    uint64_t o = du64(rd);

                    R[i * 2] = (uint32_t)o;
                    R[i * 2 + 1] = (uint32_t)(o >> 32);
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x98:
        case 0x99:
        case 0x9A:
        case 0x9B:
        case 0xA8:
        case 0xA9:
        case 0xAA:
        case 0xAB:
        case 0xB8:
        case 0xB9:
        case 0xBA:
        case 0xBB: {
            int grp = op < 0xA8 ? 0 : (op < 0xB8 ? 1 : 2);
            int isd = op == 0x99 || op == 0x9B || op == 0xA9 || op == 0xAB ||
                      op == 0xB9 || op == 0xBB;
            int scalar = op == 0x9A || op == 0x9B || op == 0xAA || op == 0xAB ||
                         op == 0xBA || op == 0xBB;

            uint32_t D0[8];

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (scalar && L) return vfl_exit_exception(v, run, 6, 0);
            vec_read(v, reg, lanes, D0);
            if (!isd && !scalar) {
                for (i = 0; i < lanes; i++) {
                    float x;
                    float y;
                    float z;

                    if (grp == 0) {
                        x = vfl_u32f(D0[i]);
                        y = vfl_u32f(B[i]);
                        z = vfl_u32f(A[i]);
                    } else if (grp == 1) {
                        x = vfl_u32f(A[i]);
                        y = vfl_u32f(D0[i]);
                        z = vfl_u32f(B[i]);
                    } else {
                        x = vfl_u32f(A[i]);
                        y = vfl_u32f(B[i]);
                        z = vfl_u32f(D0[i]);
                    }
                    R[i] = fu32(x * y + z);
                }
            } else if (!scalar) {
                for (i = 0; i < lanes / 2; i++) {
                    uint64_t xu = (uint64_t)(grp == 0 ? D0 : A)[i * 2] |
                                  ((uint64_t)(grp == 0 ? D0 : A)[i * 2 + 1] << 32);
                    uint64_t yu = (uint64_t)(grp == 1 ? D0 : (grp == 0 ? B : A))[i * 2] |
                                  ((uint64_t)(grp == 1 ? D0 : (grp == 0 ? B : A))[i * 2 + 1] << 32);
                    uint64_t zu = (uint64_t)(grp == 2 ? D0 : (grp == 1 ? B : A))[i * 2] |
                                  ((uint64_t)(grp == 2 ? D0 : (grp == 1 ? B : A))[i * 2 + 1] << 32);
                    double x = vfl_u64d(xu);
                    double y = vfl_u64d(yu);
                    double z = vfl_u64d(zu);
                    uint64_t o = du64(x * y + z);

                    R[i * 2] = (uint32_t)o;
                    R[i * 2 + 1] = (uint32_t)(o >> 32);
                }
            } else if (!isd) {
                float x;
                float y;
                float z;

                if (grp == 0) {
                    x = vfl_u32f(D0[0]);
                    y = vfl_u32f(B[0]);
                    z = vfl_u32f(A[0]);
                } else if (grp == 1) {
                    x = vfl_u32f(A[0]);
                    y = vfl_u32f(D0[0]);
                    z = vfl_u32f(B[0]);
                } else {
                    x = vfl_u32f(A[0]);
                    y = vfl_u32f(B[0]);
                    z = vfl_u32f(D0[0]);
                }
                R[0] = fu32(x * y + z);
                for (i = 1; i < lanes; i++)
                    R[i] = A[i];
            } else {
                uint64_t xu;
                uint64_t yu;
                uint64_t zu;
                double x;
                double y;
                double z;
                uint64_t o;

                if (grp == 0) {
                    xu = (uint64_t)D0[0] | ((uint64_t)D0[1] << 32);
                    yu = (uint64_t)B[0] | ((uint64_t)B[1] << 32);
                    zu = (uint64_t)A[0] | ((uint64_t)A[1] << 32);
                } else if (grp == 1) {
                    xu = (uint64_t)A[0] | ((uint64_t)A[1] << 32);
                    yu = (uint64_t)D0[0] | ((uint64_t)D0[1] << 32);
                    zu = (uint64_t)B[0] | ((uint64_t)B[1] << 32);
                } else {
                    xu = (uint64_t)A[0] | ((uint64_t)A[1] << 32);
                    yu = (uint64_t)B[0] | ((uint64_t)B[1] << 32);
                    zu = (uint64_t)D0[0] | ((uint64_t)D0[1] << 32);
                }
                x = vfl_u64d(xu);
                y = vfl_u64d(yu);
                z = vfl_u64d(zu);
                o = du64(x * y + z);
                R[0] = (uint32_t)o;
                R[1] = (uint32_t)(o >> 32);
                for (i = 2; i < lanes; i++)
                    R[i] = A[i];
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x18:
        case 0x19:
        case 0x1A:
        case 0x58:
        case 0x59:
        case 0x78:
        case 0x79: {
            int esz = op == 0x78 ? 1 : (op == 0x79 ? 2 : (op == 0x58 ? 4 : 8));
            int broker = op == 0x1A ? 16 : esz;

            if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            if (op == 0x1A && !L) return vfl_exit_exception(v, run, 6, 0);
            {
                uint8_t src[32];
                int k;

                if (ea->is_reg) {
                    uint32_t t[8];

                    vec_read(v, ea->reg, 2, t);
                    memcpy(src, t, 8);
                } else {
                    rr = vfl_mem_read(vm, v, ea->addr, (uint64_t)broker, src);
                    if (rr) {
                        vfl_memerr(v, run, rr);
                        return 1;
                    }
                }
                for (k = 0; k < lanes * 4 / esz; k++) {
                    uint64_t val = 0;
                    int j;

                    for (j = 0; j < esz; j++)
                        val |= (uint64_t)src[(k * esz + j) % broker] << (j * 8);
                    if (esz == 1) ((uint8_t *)R)[k] = (uint8_t)val;
                    else if (esz == 2) ((uint16_t *)R)[k] = (uint16_t)val;
                    else if (esz == 4) R[k] = (uint32_t)val;
                    else {
                        R[k * 2] = (uint32_t)val;
                        R[k * 2 + 1] = (uint32_t)(val >> 32);
                    }
                }
            }
            vec_write(v, reg, lanes, R, 1);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x0C:
        case 0x0D:
        case 0x8C:
        case 0x8E:
        case 0xAC:
        case 0xAE: {
            int store = op == 0x0C || op == 0x0D || op == 0x8C || op == 0x8E;
            int isd = op == 0x0D || op == 0x8E || op == 0xAE;
            int el = isd ? 8 : 4;
            int nel = isd ? lanes / 2 : lanes;
            int k;

            if (op >= 0x8C) {
                if (pp != 1) return vfl_exit_exception(v, run, 6, 0);
            } else if (pp != 0) {
                return vfl_exit_exception(v, run, 6, 0);
            }
            if (ea->is_reg) return vfl_exit_exception(v, run, 6, 0);
            if (store) {
                uint8_t tmp[64];

                rr = vfl_mem_read(vm, v, ea->addr, (uint64_t)lanes * 4, tmp);
                if (rr) {
                    vfl_memerr(v, run, rr);
                    return 1;
                }
                for (k = 0; k < nel; k++) {
                    uint64_t msk;

                    if (!isd) msk = A[k];
                    else msk = (uint64_t)A[k * 2] | ((uint64_t)A[k * 2 + 1] << 32);
                    {
                        int j;

                        for (j = 0; j < el; j++) {
                            if ((msk >> (el * 8 - 1)) & 1)
                                tmp[k * el + j] = ((uint8_t *)B)[k * el + j];
                        }
                    }
                }
                rr = vfl_mem_write(vm, v, ea->addr, (uint64_t)lanes * 4, tmp);
                if (rr) {
                    vfl_memerr(v, run, rr);
                    return 1;
                }
            } else {
                uint8_t tmp[64];

                rr = vfl_mem_read(vm, v, ea->addr, (uint64_t)lanes * 4, tmp);
                if (rr) {
                    vfl_memerr(v, run, rr);
                    return 1;
                }
                memset(R, 0, sizeof(R));
                for (k = 0; k < nel; k++) {
                    uint64_t msk;

                    if (!isd) msk = A[k];
                    else msk = (uint64_t)A[k * 2] | ((uint64_t)A[k * 2 + 1] << 32);
                    {
                        int j;

                        for (j = 0; j < el; j++) {
                            if ((msk >> (el * 8 - 1)) & 1)
                                ((uint8_t *)R)[k * el + j] = tmp[k * el + j];
                        }
                    }
                }
            }
            if (!store) {
                vec_write(v, reg, lanes, R, 1);
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }



        default:
            break;
    }
    return vfl_exit_exception(v, run, 6, 0);
}
