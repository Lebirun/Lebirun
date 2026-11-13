#include "vfl_simd.h"

int media_arith(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
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
