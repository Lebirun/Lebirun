#include "vfl_simd.h"

static int vfl_vex_more(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                        int *pos, int op, int pp, int W, int L, int lanes, int s1,
                        int asz, int seg, int reg, int rm, int mod,
                        vfl_ea_t *ea, int n, uint64_t ripmask);

void vfl_round_f32(float *r, float x, int mode) {
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

void vfl_round_f64(double *r, double x, int mode) {
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
