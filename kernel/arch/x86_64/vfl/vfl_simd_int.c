#include "vfl_simd.h"

int media_int(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
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
