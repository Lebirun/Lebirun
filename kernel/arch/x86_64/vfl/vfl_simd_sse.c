#include "vfl_simd.h"

static int media_mov(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                     int *pos, uint8_t op2, int pp, int asz, int seg,
                     int rex, int rex_w, int rex_r, int rex_b, int rex_x,
                     int lm, uint64_t ripmask);
static int media_conv(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                      int *pos, uint8_t op2, int pp, int asz, int seg,
                      int rex, int rex_r, int rex_b, int rex_x,
                      int lm, uint64_t ripmask);

uint32_t fu32(float f) {
    uint32_t x;

    memcpy(&x, &f, 4);
    return x;
}

uint64_t du64(double f) {
    uint64_t x;

    memcpy(&x, &f, 8);
    return x;
}

uint64_t f32_to_f64(uint32_t x) {
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

int64_t f32_rint_i64(uint32_t x, int *ov) {
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

uint32_t f64_to_f32(uint64_t x) {    uint32_t sign = (uint32_t)(x >> 63);
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

uint32_t f32_cmp(float a, float b, int pred) {
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

uint32_t f32_min(float a, float b) {
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

uint32_t f32_max(float a, float b) {
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

uint64_t f64_min(double a, double b) {
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

uint64_t f64_max(double a, double b) {
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

int sse_load_vec(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
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

int sse_mem_fill(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint64_t addr,
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

int sse_store_vec(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
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

int sse_move_load(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
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

int sse_bitop(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
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

int sse_arith_f32(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
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

int sse_arith_f64(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
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

int sse_cmp_f32(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
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

int sse_intop(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
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

int sse_shift_cnt(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
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

int sse_shufps(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
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

int sse_unpck(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
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

int sse_pack(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
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

int sse_hadd(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
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

int media_pre(vfl_vcpu_t *v, uint8_t *buf, int *pos, int asz, int seg,
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

int sse_scalar_f32(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
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

int sse_scalar_f64(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
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

int vfl_simd_media(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
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

int sse_scalar_store(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
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
