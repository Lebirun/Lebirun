#include "vfl_simd.h"

int simd_38(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
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

int simd_3a(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
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
