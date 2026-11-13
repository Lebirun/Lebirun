#include "vfl_simd.h"

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

int sse_movmsk(vfl_vcpu_t *v, int dst, int src, int rex, int el, int n, uint64_t ripmask) {
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

int escape_modrm(vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf, int *pos,
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

int escape_ea(vfl_vcpu_t *v, uint8_t *buf, int *pos, int mod,
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
