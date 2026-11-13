#include "vfl_simd.h"

static int vfl_vex_more3(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                         int *pos, int op, int pp, int W, int L, int lanes, int s1,
                         int reg, vfl_ea_t *ea, int n, uint64_t ripmask);

int vfl_vex_more2(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
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

int vfl_vex38_3a(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
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
