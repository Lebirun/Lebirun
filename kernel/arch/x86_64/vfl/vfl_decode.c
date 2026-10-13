#include "vfl_priv.h"
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

static const uint8_t vfl_reg_off[16] = {
    offsetof(vfl_regs_t, rax), offsetof(vfl_regs_t, rcx),
    offsetof(vfl_regs_t, rdx), offsetof(vfl_regs_t, rbx),
    offsetof(vfl_regs_t, rsp), offsetof(vfl_regs_t, rbp),
    offsetof(vfl_regs_t, rsi), offsetof(vfl_regs_t, rdi),
    offsetof(vfl_regs_t, r8), offsetof(vfl_regs_t, r9),
    offsetof(vfl_regs_t, r10), offsetof(vfl_regs_t, r11),
    offsetof(vfl_regs_t, r12), offsetof(vfl_regs_t, r13),
    offsetof(vfl_regs_t, r14), offsetof(vfl_regs_t, r15)
};

uint64_t vfl_mask(int size) {
    static const uint64_t t[9] = {
        0, 0xFFULL, 0xFFFFULL, 0, 0xFFFFFFFFULL, 0, 0, 0, ~0ULL
    };

    if ((unsigned)size <= 8) return t[size];
    return 0xFFULL;
}

uint16_t vfl_get16(uint8_t *b) {
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

uint32_t vfl_get32(uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

uint64_t vfl_get64(uint8_t *b) {
    uint64_t lo = vfl_get32(b);
    uint64_t hi = vfl_get32(b + 4);
    return lo | (hi << 32);
}

void vfl_put64(uint8_t *b, uint64_t v, int size) {
    int i;
    for (i = 0; i < size; i++)
        b[i] = (uint8_t)(v >> (i * 8));
}

int vfl_long_mode(vfl_vcpu_t *v) {
    return ((v->sregs.efer & 0x400ULL) != 0) &&
           ((v->sregs.cr0 & 0x80000000ULL) != 0);
}

uint64_t *vfl_reg64(vfl_vcpu_t *v, int r) {
    return (uint64_t *)((uint8_t *)&v->regs + vfl_reg_off[r & 15]);
}

uint64_t vfl_read_reg(vfl_vcpu_t *v, int r, int rex, int size) {
    uint64_t val;

    if (size == 1 && !rex && r >= 4 && r < 8)
        return (*vfl_reg64(v, r - 4) >> 8) & 0xFFULL;
    val = *vfl_reg64(v, r);
    if (size == 1) return val & 0xFFULL;
    return val & vfl_mask(size);
}

void vfl_write_reg(vfl_vcpu_t *v, int r, int rex, int size, uint64_t val) {
    uint64_t *p;

    if (size == 1 && !rex && r >= 4 && r < 8) {
        p = vfl_reg64(v, r - 4);
        *p = (*p & ~(0xFFULL << 8)) | ((val & 0xFFULL) << 8);
        return;
    }
    p = vfl_reg64(v, r);
    if (size == 1) {
        *p = (*p & ~0xFFULL) | (val & 0xFFULL);
        return;
    }
    if (size == 8) {
        *p = val;
        return;
    }
    *p = (*p & ~vfl_mask(size)) | (val & vfl_mask(size));
}

static const uint8_t vfl_seg_off[8] = {
    offsetof(vfl_sregs_t, es), offsetof(vfl_sregs_t, cs),
    offsetof(vfl_sregs_t, ss), offsetof(vfl_sregs_t, ds),
    offsetof(vfl_sregs_t, fs), offsetof(vfl_sregs_t, gs),
    offsetof(vfl_sregs_t, ds), offsetof(vfl_sregs_t, ds)
};

void vfl_seg_ptr(vfl_vcpu_t *v, int seg, vfl_segment_t **out) {
    *out = (vfl_segment_t *)((uint8_t *)&v->sregs + vfl_seg_off[seg & 7]);
}

uint64_t vfl_seg_base(vfl_vcpu_t *v, int seg) {
    vfl_segment_t *s;

    vfl_seg_ptr(v, seg, &s);
    if (v->sregs.cr0 & 1ULL) {
        if (vfl_long_mode(v) && seg != 4 && seg != 5) return 0;
        return s->base;
    }
    return (uint64_t)s->selector << 4;
}

void vfl_adv(vfl_vcpu_t *v, int n, uint64_t ripmask) {
    v->regs.rip = (v->regs.rip & ~ripmask) | ((v->regs.rip + (uint64_t)n) & ripmask);
}

static int vfl_parity(uint8_t b) {
    b ^= b >> 4;
    b ^= b >> 2;
    b ^= b >> 1;
    return !(b & 1);
}

void vfl_flag_logic(vfl_vcpu_t *v, uint64_t r, int size) {
    v->regs.rflags &= ~((1ULL << 0) | (1ULL << 2) | (1ULL << 4) |
                        (1ULL << 6) | (1ULL << 7) | (1ULL << 11));
    if ((r & vfl_mask(size)) == 0) v->regs.rflags |= (1ULL << 6);
    if (r & (1ULL << (size * 8 - 1))) v->regs.rflags |= (1ULL << 7);
    if (vfl_parity((uint8_t)r)) v->regs.rflags |= (1ULL << 2);
}

static void vfl_flag_add(vfl_vcpu_t *v, uint64_t a, uint64_t b, uint64_t carry,
                         uint64_t r, int size) {
    int bits = size * 8;
    __uint128_t full = (__uint128_t)(a & vfl_mask(size)) +
                       (__uint128_t)(b & vfl_mask(size)) + carry;
    uint64_t m = vfl_mask(size);

    v->regs.rflags &= ~((1ULL << 0) | (1ULL << 2) | (1ULL << 4) |
                        (1ULL << 6) | (1ULL << 7) | (1ULL << 11));
    if ((full >> bits) & 1) v->regs.rflags |= (1ULL << 0);
    if ((r & m) == 0) v->regs.rflags |= (1ULL << 6);
    if (r & (1ULL << (bits - 1))) v->regs.rflags |= (1ULL << 7);
    if ((((a ^ r) & (b ^ r)) >> (bits - 1)) & 1) v->regs.rflags |= (1ULL << 11);
    if ((a ^ b ^ r) & 0x10) v->regs.rflags |= (1ULL << 4);
    if (vfl_parity((uint8_t)r)) v->regs.rflags |= (1ULL << 2);
}

void vfl_flag_sub(vfl_vcpu_t *v, uint64_t a, uint64_t b, uint64_t borrow,
                  uint64_t r, int size) {
    int bits = size * 8;
    __uint128_t bb = (__uint128_t)(b & vfl_mask(size)) + borrow;
    uint64_t m = vfl_mask(size);

    v->regs.rflags &= ~((1ULL << 0) | (1ULL << 2) | (1ULL << 4) |
                        (1ULL << 6) | (1ULL << 7) | (1ULL << 11));
    if ((a & m) < (uint64_t)bb) v->regs.rflags |= (1ULL << 0);
    if ((r & m) == 0) v->regs.rflags |= (1ULL << 6);
    if (r & (1ULL << (bits - 1))) v->regs.rflags |= (1ULL << 7);
    if ((((a ^ b) & (a ^ r)) >> (bits - 1)) & 1) v->regs.rflags |= (1ULL << 11);
    if ((a ^ b ^ r) & 0x10) v->regs.rflags |= (1ULL << 4);
    if (vfl_parity((uint8_t)r)) v->regs.rflags |= (1ULL << 2);
}

uint64_t vfl_do_alu(vfl_vcpu_t *v, int op, uint64_t a, uint64_t b, int size) {
    uint64_t m = vfl_mask(size);
    uint64_t cf = (v->regs.rflags >> 0) & 1ULL;
    uint64_t r = 0;

    switch (op) {
        case VFL_RMW_ADD:
            r = (a + b) & m;
            vfl_flag_add(v, a, b, 0, r, size);
            break;
        case VFL_RMW_OR:
            r = (a | b) & m;
            vfl_flag_logic(v, r, size);
            break;
        case VFL_RMW_ADC:
            r = (uint64_t)((__uint128_t)(a & m) + (b & m) + cf) & m;
            vfl_flag_add(v, a, b, cf, r, size);
            break;
        case VFL_RMW_SBB:
            r = (a - b - cf) & m;
            vfl_flag_sub(v, a, b, cf, r, size);
            break;
        case VFL_RMW_AND:
            r = (a & b) & m;
            vfl_flag_logic(v, r, size);
            break;
        case VFL_RMW_SUB:
        case VFL_RMW_CMP:
            r = (a - b) & m;
            vfl_flag_sub(v, a, b, 0, r, size);
            break;
        case VFL_RMW_XOR:
            r = (a ^ b) & m;
            vfl_flag_logic(v, r, size);
            break;
        case VFL_RMW_INC:
            r = (a + 1) & m;
            {
                uint64_t saved = (v->regs.rflags >> 0) & 1ULL;
                vfl_flag_add(v, a, 1, 0, r, size);
                v->regs.rflags = (v->regs.rflags & ~(1ULL << 0)) | (saved << 0);
            }
            break;
        case VFL_RMW_DEC:
            r = (a - 1) & m;
            {
                uint64_t saved = (v->regs.rflags >> 0) & 1ULL;
                vfl_flag_sub(v, a, 1, 0, r, size);
                v->regs.rflags = (v->regs.rflags & ~(1ULL << 0)) | (saved << 0);
            }
            break;
        case VFL_RMW_NOT:
            r = (~a) & m;
            break;
        case VFL_RMW_NEG:
            r = (0 - a) & m;
            vfl_flag_sub(v, 0, a, 0, r, size);
            break;
        default:
            break;
    }
    return r;
}

uint64_t vfl_do_shift(vfl_vcpu_t *v, int op, uint64_t val, unsigned count, int size) {
    int bits = size * 8;
    uint64_t m = vfl_mask(size);
    uint64_t cf;
    uint64_t msb;

    if (size == 8) count &= 63;
    else count &= 31;
    if (count == 0) return val & m;
    val &= m;
    switch (op) {
        case 0:
            while (count--) {
                cf = (val >> (bits - 1)) & 1ULL;
                val = ((val << 1) | cf) & m;
                v->regs.rflags = (v->regs.rflags & ~(1ULL << 0)) | (cf << 0);
            }
            msb = (val >> (bits - 1)) & 1ULL;
            cf = (v->regs.rflags >> 0) & 1ULL;
            if (msb ^ cf) v->regs.rflags |= (1ULL << 11);
            else v->regs.rflags &= ~(1ULL << 11);
            break;
        case 1:
            while (count--) {
                cf = val & 1ULL;
                val = (val >> 1) | (cf << (bits - 1));
                val &= m;
                v->regs.rflags = (v->regs.rflags & ~(1ULL << 0)) | (cf << 0);
            }
            msb = (val >> (bits - 1)) & 1ULL;
            {
                uint64_t second = bits > 1 ? (val >> (bits - 2)) & 1ULL : 0;

                if (msb ^ second) v->regs.rflags |= (1ULL << 11);
                else v->regs.rflags &= ~(1ULL << 11);
            }
            break;
        case 2:
            while (count--) {
                uint64_t oldcf = (v->regs.rflags >> 0) & 1ULL;

                cf = (val >> (bits - 1)) & 1ULL;
                val = ((val << 1) | oldcf) & m;
                v->regs.rflags = (v->regs.rflags & ~(1ULL << 0)) | (cf << 0);
            }
            msb = (val >> (bits - 1)) & 1ULL;
            cf = (v->regs.rflags >> 0) & 1ULL;
            if (msb ^ cf) v->regs.rflags |= (1ULL << 11);
            else v->regs.rflags &= ~(1ULL << 11);
            break;
        case 3:
            while (count--) {
                uint64_t oldcf = (v->regs.rflags >> 0) & 1ULL;

                cf = val & 1ULL;
                val = (val >> 1) | (oldcf << (bits - 1));
                val &= m;
                v->regs.rflags = (v->regs.rflags & ~(1ULL << 0)) | (cf << 0);
            }
            msb = (val >> (bits - 1)) & 1ULL;
            {
                uint64_t second = bits > 1 ? (val >> (bits - 2)) & 1ULL : 0;

                if (msb ^ second) v->regs.rflags |= (1ULL << 11);
                else v->regs.rflags &= ~(1ULL << 11);
            }
            break;
        case 4:
        case 6:
            if (count > (unsigned)bits) {
                val = 0;
                vfl_flag_logic(v, 0, size);
                v->regs.rflags &= ~(1ULL << 0);
                break;
            }
            cf = (val >> (bits - count)) & 1ULL;
            val = (val << count) & m;
            vfl_flag_logic(v, val, size);
            v->regs.rflags |= (cf << 0);
            if (count == 1) {
                msb = (val >> (bits - 1)) & 1ULL;
                if (msb ^ cf) v->regs.rflags |= (1ULL << 11);
            }
            break;
        case 5:
            cf = (val >> (count - 1)) & 1ULL;
            val = (val >> count) & m;
            vfl_flag_logic(v, val, size);
            v->regs.rflags = (v->regs.rflags & ~((1ULL << 0) | (1ULL << 11))) | (cf << 0);
            break;
        default: {
            uint64_t sign = (val >> (bits - 1)) & 1ULL;
            int i;

            cf = (val >> (count - 1)) & 1ULL;
            if (count >= (unsigned)bits) {
                val = sign ? m : 0;
            } else {
                val = (val >> count) & m;
                if (sign) {
                    for (i = 0; i < (int)count; i++)
                        val |= 1ULL << (bits - 1 - i);
                }
            }
            vfl_flag_logic(v, val, size);
            v->regs.rflags = (v->regs.rflags & ~((1ULL << 0) | (1ULL << 11))) | (cf << 0);
            break;
        }
    }
    return val & m;
}

int vfl_cond(vfl_vcpu_t *v, int cc) {
    uint64_t f = v->regs.rflags;
    int cf = (f >> 0) & 1;
    int pf = (f >> 2) & 1;
    int zf = (f >> 6) & 1;
    int sf = (f >> 7) & 1;
    int of = (f >> 11) & 1;

    switch (cc & 15) {
        case 0: return of;
        case 1: return !of;
        case 2: return cf;
        case 3: return !cf;
        case 4: return zf;
        case 5: return !zf;
        case 6: return cf || zf;
        case 7: return !cf && !zf;
        case 8: return sf;
        case 9: return !sf;
        case 10: return pf;
        case 11: return !pf;
        case 12: return sf != of;
        case 13: return sf == of;
        case 14: return zf || (sf != of);
        default: return !zf && (sf == of);
    }
}

int vfl_decode_ea(vfl_vcpu_t *v, uint8_t *buf, int *pos, int mod, int rm,
                  int asz, int seg_ovr, int rex_b, int rex_x, vfl_ea_t *ea) {
    int lm = vfl_long_mode(v);

    ea->is_reg = 0;
    ea->reg = 0;
    ea->addr = 0;
    ea->rip_rel = 0;
    ea->disp = 0;
    if (mod == 3) {
        ea->is_reg = 1;
        ea->reg = (rm & 7) | (rex_b << 3);
        return 0;
    }
    if (asz == 2) {
        uint16_t base = 0;
        int seg = 3;
        int32_t disp = 0;

        switch (rm & 7) {
            case 0: base = (uint16_t)(*vfl_reg64(v, 3) + *vfl_reg64(v, 6)); break;
            case 1: base = (uint16_t)(*vfl_reg64(v, 3) + *vfl_reg64(v, 7)); break;
            case 2: base = (uint16_t)(*vfl_reg64(v, 5) + *vfl_reg64(v, 6)); seg = 2; break;
            case 3: base = (uint16_t)(*vfl_reg64(v, 5) + *vfl_reg64(v, 7)); seg = 2; break;
            case 4: base = (uint16_t)*vfl_reg64(v, 6); break;
            case 5: base = (uint16_t)*vfl_reg64(v, 7); break;
            case 6:
                if (mod == 0) break;
                base = (uint16_t)*vfl_reg64(v, 5);
                seg = 2;
                break;
            default: base = (uint16_t)*vfl_reg64(v, 3); break;
        }
        if (mod == 0 && (rm & 7) == 6) {
            if (*pos + 2 > 15) return -1;
            disp = (int16_t)vfl_get16(buf + *pos);
            *pos += 2;
        } else if (mod == 1) {
            if (*pos + 1 > 15) return -1;
            disp = (int8_t)buf[*pos];
            *pos += 1;
        } else if (mod == 2) {
            if (*pos + 2 > 15) return -1;
            disp = (int16_t)vfl_get16(buf + *pos);
            *pos += 2;
        }
        if (seg_ovr >= 0) seg = seg_ovr;
        ea->addr = vfl_seg_base(v, seg) + (uint64_t)(uint16_t)(base + disp);
        return 0;
    }
    {
        uint64_t base = 0;
        uint64_t idx = 0;
        int scl = 0;
        int seg = 3;
        int32_t disp = 0;

        if ((rm & 7) == 4) {
            uint8_t sib;
            int sc;
            int ix;
            int bs;

            if (*pos + 1 > 15) return -1;
            sib = buf[*pos];
            *pos += 1;
            sc = (sib >> 6) & 3;
            ix = ((sib >> 3) & 7) | (rex_x << 3);
            bs = (sib & 7) | (rex_b << 3);
            if (ix != 4 || rex_x) {
                idx = *vfl_reg64(v, ix);
                scl = sc;
            }
            if (mod == 0 && (sib & 7) == 5) {
                if (*pos + 4 > 15) return -1;
                disp = (int32_t)vfl_get32(buf + *pos);
                *pos += 4;
            } else {
                base = *vfl_reg64(v, bs);
                if ((bs & 7) == 4 || (bs & 7) == 5) seg = 2;
            }
        } else {
            int r = (rm & 7) | (rex_b << 3);

            if (mod == 0 && (rm & 7) == 5) {
                if (*pos + 4 > 15) return -1;
                disp = (int32_t)vfl_get32(buf + *pos);
                *pos += 4;
                if (lm && asz == 8) {
                    ea->rip_rel = 1;
                    ea->disp = disp;
                    return 0;
                }
            } else {
                base = *vfl_reg64(v, r);
                if ((rm & 7) == 4 || (rm & 7) == 5) seg = 2;
            }
        }
        if (mod == 1) {
            if (*pos + 1 > 15) return -1;
            disp = (int8_t)buf[*pos];
            *pos += 1;
        } else if (mod == 2) {
            if (*pos + 4 > 15) return -1;
            disp = (int32_t)vfl_get32(buf + *pos);
            *pos += 4;
        }
        if (seg_ovr >= 0) seg = seg_ovr;
        if (asz == 4)
            ea->addr = vfl_seg_base(v, seg) +
                       (uint64_t)(uint32_t)(base + (idx << scl) + (int64_t)disp);
        else
            ea->addr = vfl_seg_base(v, seg) + base + (idx << scl) + (uint64_t)(int64_t)disp;
        return 0;
    }
}

uint64_t vfl_stack_mask(vfl_vcpu_t *v) {
    if (vfl_long_mode(v)) return ~0ULL;
    if (v->sregs.cr0 & 1ULL) return 0xFFFFFFFFULL;
    return 0xFFFFULL;
}

uint64_t vfl_sp(vfl_vcpu_t *v) {
    return v->regs.rsp & vfl_stack_mask(v);
}

void vfl_set_sp(vfl_vcpu_t *v, uint64_t sp) {
    uint64_t m = vfl_stack_mask(v);

    v->regs.rsp = (v->regs.rsp & ~m) | (sp & m);
}

int vfl_push(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint64_t val, int size) {
    uint64_t sp = (vfl_sp(v) - (uint64_t)size) & vfl_stack_mask(v);
    uint8_t b[8];
    int r;

    vfl_put64(b, val, size);
    r = vfl_mem_write(vm, v, vfl_seg_base(v, 2) + sp, (uint64_t)size, b);
    if (r == -2) return vfl_mem_fault(v, run);
    if (r) return vfl_exit_exception(v, run, 12, 0);
    vfl_set_sp(v, sp);
    return 0;
}

int vfl_pop(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, int size, uint64_t *val) {
    uint64_t sp = vfl_sp(v);
    uint64_t out = 0;
    int r;

    r = vfl_mem_read(vm, v, vfl_seg_base(v, 2) + sp, (uint64_t)size, (uint8_t *)&out);
    if (r == -2) return vfl_mem_fault(v, run);
    if (r) return vfl_exit_exception(v, run, 12, 0);
    vfl_set_sp(v, (sp + (uint64_t)size) & vfl_stack_mask(v));
    *val = out & vfl_mask(size);
    return 0;
}

uint64_t vfl_get_si(vfl_vcpu_t *v, int asz) {
    if (asz == 2) return v->regs.rsi & 0xFFFFULL;
    if (asz == 4) return v->regs.rsi & 0xFFFFFFFFULL;
    return v->regs.rsi;
}

uint64_t vfl_get_di(vfl_vcpu_t *v, int asz) {
    if (asz == 2) return v->regs.rdi & 0xFFFFULL;
    if (asz == 4) return v->regs.rdi & 0xFFFFFFFFULL;
    return v->regs.rdi;
}

void vfl_set_si(vfl_vcpu_t *v, int asz, uint64_t val) {
    if (asz == 2) v->regs.rsi = (v->regs.rsi & ~0xFFFFULL) | (val & 0xFFFFULL);
    else if (asz == 4) v->regs.rsi = (v->regs.rsi & ~0xFFFFFFFFULL) | (val & 0xFFFFFFFFULL);
    else v->regs.rsi = val;
}

void vfl_set_di(vfl_vcpu_t *v, int asz, uint64_t val) {
    if (asz == 2) v->regs.rdi = (v->regs.rdi & ~0xFFFFULL) | (val & 0xFFFFULL);
    else if (asz == 4) v->regs.rdi = (v->regs.rdi & ~0xFFFFFFFFULL) | (val & 0xFFFFFFFFULL);
    else v->regs.rdi = val;
}

uint64_t vfl_get_count(vfl_vcpu_t *v, int asz) {
    if (asz == 2) return v->regs.rcx & 0xFFFFULL;
    if (asz == 4) return v->regs.rcx & 0xFFFFFFFFULL;
    return v->regs.rcx;
}

void vfl_set_count(vfl_vcpu_t *v, int asz, uint64_t val) {
    if (asz == 2) v->regs.rcx = (v->regs.rcx & ~0xFFFFULL) | (val & 0xFFFFULL);
    else if (asz == 4) v->regs.rcx = (v->regs.rcx & ~0xFFFFFFFFULL) | (val & 0xFFFFFFFFULL);
    else v->regs.rcx = val;
}
