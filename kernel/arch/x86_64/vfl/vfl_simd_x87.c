#include "vfl_simd.h"

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
