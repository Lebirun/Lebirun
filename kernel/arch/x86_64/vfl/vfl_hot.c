#include "vfl_priv.h"
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <string.h>
#include <stdint.h>

enum {
    VFL_H_NOP = 1,
    VFL_H_MOV_R_IMM,
    VFL_H_ALU_R_IMM,
    VFL_H_PUSH_R,
    VFL_H_POP_R,
    VFL_H_PUSH_IMM,
    VFL_H_PUSHF,
    VFL_H_POPF,
    VFL_H_MOV_RM,
    VFL_H_ALU_RM,
    VFL_H_ALU_RM_IMM,
    VFL_H_TEST_RM,
    VFL_H_MOVZX,
    VFL_H_LEA,
    VFL_H_JCC,
    VFL_H_JMP,
    VFL_H_CALL,
    VFL_H_RET,
    VFL_H_LEAVE,
    VFL_H_LOOP,
    VFL_H_INCDEC,
    VFL_H_FFCALL,
    VFL_H_XCHG_AX,
    VFL_H_MOV_RM_IMM
};

static void vfl_hot_ea(vfl_vcpu_t *v, vfl_hot_op_t *h, vfl_ea_t *ea) {
    ea->is_reg = 0;
    ea->reg = 0;
    ea->addr = 0;
    ea->rip_rel = 0;
    ea->disp = h->disp;
    if (h->eamod == 3) {
        ea->is_reg = 1;
        ea->reg = h->eareg;
        return;
    }
    if (h->asz == 2) {
        uint16_t base = 0;

        switch (h->ea16rm) {
            case 0: base = (uint16_t)(*vfl_reg64(v, 3) + *vfl_reg64(v, 6)); break;
            case 1: base = (uint16_t)(*vfl_reg64(v, 3) + *vfl_reg64(v, 7)); break;
            case 2: base = (uint16_t)(*vfl_reg64(v, 5) + *vfl_reg64(v, 6)); break;
            case 3: base = (uint16_t)(*vfl_reg64(v, 5) + *vfl_reg64(v, 7)); break;
            case 4: base = (uint16_t)*vfl_reg64(v, 6); break;
            case 5: base = (uint16_t)*vfl_reg64(v, 7); break;
            case 6:
                if (h->eamod != 0) base = (uint16_t)*vfl_reg64(v, 5);
                break;
            default: base = (uint16_t)*vfl_reg64(v, 3); break;
        }
        ea->addr = vfl_seg_base(v, h->seg) + (uint64_t)(uint16_t)(base + h->disp);
        return;
    }
    {
        uint64_t base = 0;
        uint64_t idx = 0;

        if (h->sib_base == -2) {
            ea->rip_rel = 1;
            ea->addr = (v->regs.rip + h->len + (uint64_t)(int64_t)h->disp) & h->ripmask;
            return;
        }
        if (h->sib_base >= 0) base = *vfl_reg64(v, h->sib_base);
        if (h->sib_index >= 0) idx = *vfl_reg64(v, h->sib_index) << h->sib_scale;
        if (h->asz == 4)
            ea->addr = vfl_seg_base(v, h->seg) +
                       (uint64_t)(uint32_t)(base + idx + (int64_t)h->disp);
        else
            ea->addr = vfl_seg_base(v, h->seg) + base + idx + (uint64_t)(int64_t)h->disp;
    }
}

static int vfl_hot_nop(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    (void)vm;
    (void)v;
    (void)run;
    vfl_adv(v, h->len, h->ripmask);
    return 0;
}

static int vfl_hot_mov_r_imm(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    (void)vm;
    (void)run;
    vfl_write_reg(v, h->reg, h->rex, h->size, h->imm);
    vfl_adv(v, h->len, h->ripmask);
    return 0;
}

static int vfl_hot_alu_r_imm(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    uint64_t a;
    uint64_t r;

    (void)vm;
    (void)run;
    a = vfl_read_reg(v, 0, h->rex, h->size);
    if (h->sub2 == 2) {
        vfl_flag_logic(v, (a & h->imm) & vfl_mask(h->size), h->size);
        vfl_adv(v, h->len, h->ripmask);
        return 0;
    }
    r = vfl_do_alu(v, h->sub, a, h->imm, h->size);
    if (!h->sub2) vfl_write_reg(v, 0, h->rex, h->size, r);
    vfl_adv(v, h->len, h->ripmask);
    return 0;
}

static int vfl_hot_push_r(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    uint64_t a = vfl_read_reg(v, h->reg, h->rex, h->size);

    if (vfl_push(vm, v, run, a, h->size)) return 1;
    vfl_adv(v, h->len, h->ripmask);
    return 0;
}

static int vfl_hot_pop_r(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    uint64_t a = 0;

    if (vfl_pop(vm, v, run, h->size, &a)) return 1;
    vfl_write_reg(v, h->reg, h->rex, h->size, a);
    vfl_adv(v, h->len, h->ripmask);
    return 0;
}

static int vfl_hot_push_imm(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    if (vfl_push(vm, v, run, h->imm, h->size)) return 1;
    vfl_adv(v, h->len, h->ripmask);
    return 0;
}

static int vfl_hot_pushf(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    if (vfl_push(vm, v, run, v->regs.rflags, h->size)) return 1;
    vfl_adv(v, h->len, h->ripmask);
    return 0;
}

static int vfl_hot_popf(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    uint64_t a = 0;

    if (vfl_pop(vm, v, run, h->size, &a)) return 1;
    v->regs.rflags = (a & vfl_mask(h->size)) | 0x2ULL;
    vfl_adv(v, h->len, h->ripmask);
    return 0;
}

static int vfl_hot_mov_rm(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    vfl_ea_t ea;
    uint64_t a;

    vfl_hot_ea(v, h, &ea);
    if (h->sub & 1)
        return vfl_exec_load_reg(vm, v, run, &ea, h->reg, h->size, h->rex, h->len,
                                 h->ripmask);
    a = vfl_read_reg(v, h->reg, h->rex, h->size);
    return vfl_exec_store(vm, v, run, &ea, a, h->size, h->rex, h->len, h->ripmask);
}

static int vfl_hot_alu_rm(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    vfl_ea_t ea;
    uint64_t a;
    uint64_t src = 0;
    uint64_t r;
    int rr;

    vfl_hot_ea(v, h, &ea);
    if (h->sub & 1) {
        rr = vfl_ea_read(vm, v, &ea, h->size, h->rex, &src);
        if (rr == -2) return vfl_mem_fault(v, run);
        if (rr == 1 && !ea.is_reg)
            return vfl_mmio_or_fault(vm, v, run, ea.addr, h->size, VFL_P_MMIO_REG,
                                     h->reg, h->rex, 0, 0, h->len, h->ripmask);
        if (rr != 0) return vfl_exit_internal(v, run);
        src &= vfl_mask(h->size);
        a = vfl_read_reg(v, h->reg, h->rex, h->size);
        r = vfl_do_alu(v, h->sub >> 1, a, src, h->size);
        if ((h->sub >> 1) != VFL_RMW_CMP) vfl_write_reg(v, h->reg, h->rex, h->size, r);
    } else {
        a = vfl_read_reg(v, h->reg, h->rex, h->size);
        return vfl_exec_rmw(vm, v, run, &ea, h->sub >> 1, a, h->size, h->rex, h->len,
                            h->ripmask);
    }
    vfl_adv(v, h->len, h->ripmask);
    return 0;
}

static int vfl_hot_alu_rm_imm(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    vfl_ea_t ea;

    vfl_hot_ea(v, h, &ea);
    return vfl_exec_rmw(vm, v, run, &ea, h->sub, h->imm, h->size, h->rex, h->len,
                        h->ripmask);
}

static int vfl_hot_test_rm(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    vfl_ea_t ea;
    uint64_t a;
    uint64_t b = 0;
    int rr;

    vfl_hot_ea(v, h, &ea);
    a = vfl_read_reg(v, h->reg, h->rex, h->size);
    rr = vfl_ea_read(vm, v, &ea, h->size, h->rex, &b);
    if (rr == -2) return vfl_mem_fault(v, run);
    if (rr == 1 && !ea.is_reg)
        return vfl_mmio_or_fault(vm, v, run, ea.addr, h->size, VFL_P_MMIO_RMW,
                                 0, 0, a & vfl_mask(h->size), 14, h->len, h->ripmask);
    if (rr != 0) return vfl_exit_internal(v, run);
    vfl_flag_logic(v, (a & b) & vfl_mask(h->size), h->size);
    vfl_adv(v, h->len, h->ripmask);
    return 0;
}

static int vfl_hot_movzx(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    vfl_ea_t ea;
    uint64_t a = 0;
    int rr;

    vfl_hot_ea(v, h, &ea);
    rr = vfl_ea_read(vm, v, &ea, h->sub & 3, h->rex, &a);
    if (rr == -2) return vfl_mem_fault(v, run);
    if (rr == 1 && !ea.is_reg)
        return vfl_mmio_or_fault(vm, v, run, ea.addr, h->sub & 3, VFL_P_MMIO_REG,
                                 h->reg, h->rex, 0, 0, h->len, h->ripmask);
    if (rr != 0) return vfl_exit_internal(v, run);
    a &= vfl_mask(h->sub & 3);
    if ((h->sub & 4) && (a & (1ULL << ((h->sub & 3) * 8 - 1))))
        a |= ~vfl_mask(h->sub & 3);
    if (h->sub2 == 0) *vfl_reg64(v, h->reg) = (uint64_t)(int64_t)(int8_t)a;
    else if (h->sub2 == 1) *vfl_reg64(v, h->reg) = (uint64_t)(int64_t)(int16_t)a;
    else vfl_write_reg(v, h->reg, h->rex, h->sub2 == 3 ? 2 : 4, a);
    vfl_adv(v, h->len, h->ripmask);
    return 0;
}

static int vfl_hot_lea(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    vfl_ea_t ea;

    (void)vm;
    (void)run;
    vfl_hot_ea(v, h, &ea);
    vfl_write_reg(v, h->reg, h->rex, h->size, ea.addr & h->imm);
    vfl_adv(v, h->len, h->ripmask);
    return 0;
}

static int vfl_hot_jcc(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    (void)vm;
    (void)run;
    if (vfl_cond(v, h->sub))
        v->regs.rip = (v->regs.rip & ~h->ripmask) |
            ((v->regs.rip + h->len + h->imm) & h->ripmask);
    else
        vfl_adv(v, h->len, h->ripmask);
    return 0;
}

static int vfl_hot_jmp(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    (void)vm;
    (void)run;
    v->regs.rip = (v->regs.rip & ~h->ripmask) |
        ((v->regs.rip + h->len + h->imm) & h->ripmask);
    return 0;
}

static int vfl_hot_call(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    uint64_t ret = (v->regs.rip + h->len) & h->ripmask;

    if (vfl_push(vm, v, run, ret, h->size)) return 1;
    v->regs.rip = (v->regs.rip & ~h->ripmask) |
        ((ret + h->imm) & h->ripmask);
    return 0;
}

static int vfl_hot_ret(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    uint64_t a = 0;

    if (vfl_pop(vm, v, run, h->size, &a)) return 1;
    v->regs.rip = (v->regs.rip & ~h->ripmask) | (a & h->ripmask);
    return 0;
}

static int vfl_hot_leave(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    uint64_t bp;
    uint64_t a = 0;

    if (h->size == 2) bp = v->regs.rbp & 0xFFFFULL;
    else if (h->size == 4) bp = v->regs.rbp & 0xFFFFFFFFULL;
    else bp = v->regs.rbp;
    vfl_set_sp(v, bp);
    if (vfl_pop(vm, v, run, h->size, &a)) return 1;
    vfl_write_reg(v, 5, h->rex, h->size, a);
    vfl_adv(v, h->len, h->ripmask);
    return 0;
}

static int vfl_hot_loop(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    uint64_t cnt = vfl_get_count(v, h->asz);
    int b = 0;

    (void)vm;
    (void)run;
    if (h->sub != 0xE3) {
        cnt = (cnt - 1) & vfl_mask(h->asz);
        vfl_set_count(v, h->asz, cnt);
    }
    if (h->sub == 0xE0 && cnt != 0 && !((v->regs.rflags >> 6) & 1)) b = 1;
    if (h->sub == 0xE1 && cnt != 0 && ((v->regs.rflags >> 6) & 1)) b = 1;
    if (h->sub == 0xE2 && cnt != 0) b = 1;
    if (h->sub == 0xE3 && cnt == 0) b = 1;
    if (b)
        v->regs.rip = (v->regs.rip & ~h->ripmask) |
            ((v->regs.rip + h->len + h->imm) & h->ripmask);
    else
        vfl_adv(v, h->len, h->ripmask);
    return 0;
}

static int vfl_hot_incdec(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    vfl_ea_t ea;
    uint64_t a = 0;
    uint64_t r;
    int rr;

    vfl_hot_ea(v, h, &ea);
    rr = vfl_ea_read(vm, v, &ea, h->size, h->rex, &a);
    if (rr == -2) return vfl_mem_fault(v, run);
    if (rr == 1 && !ea.is_reg)
        return vfl_mmio_or_fault(vm, v, run, ea.addr, h->size, VFL_P_MMIO_RMW,
                                 0, 0, 0, h->sub ? VFL_RMW_DEC : VFL_RMW_INC,
                                 h->len, h->ripmask);
    if (rr != 0) {
        if (ea.is_reg) return vfl_exit_internal(v, run);
        return vfl_memerr(v, run, rr);
    }
    r = vfl_do_alu(v, h->sub ? VFL_RMW_DEC : VFL_RMW_INC, a & vfl_mask(h->size), 0, h->size);
    return vfl_exec_store(vm, v, run, &ea, r, h->size, h->rex, h->len, h->ripmask);
}

static int vfl_hot_fficall(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    vfl_ea_t ea;
    uint64_t tgt = 0;
    uint64_t ret;
    int rr;

    vfl_hot_ea(v, h, &ea);
    rr = vfl_ea_read(vm, v, &ea, h->size, h->rex, &tgt);
    if (rr == -2) return vfl_mem_fault(v, run);
    if (rr == 1 && !ea.is_reg)
        return vfl_mmio_or_fault(vm, v, run, ea.addr, h->size, VFL_P_MMIO_CALL,
                                 0, 0, 0, 0, h->len, h->ripmask);
    if (rr != 0) {
        if (ea.is_reg) return vfl_exit_internal(v, run);
        return vfl_memerr(v, run, rr);
    }
    if (h->sub == 2) {
        ret = (v->regs.rip + h->len) & h->ripmask;
        if (vfl_push(vm, v, run, ret, h->size)) return 1;
        v->regs.rip = (v->regs.rip & ~h->ripmask) | (tgt & h->ripmask);
        return 0;
    }
    if (h->sub == 4) {
        v->regs.rip = (v->regs.rip & ~h->ripmask) | (tgt & h->ripmask);
        return 0;
    }
    return vfl_exit_internal(v, run);
}

static int vfl_hot_xchg_ax(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    uint64_t a;
    uint64_t b;

    (void)vm;
    (void)run;
    a = vfl_read_reg(v, 0, h->rex, h->size);
    b = vfl_read_reg(v, h->reg, h->rex, h->size);
    vfl_write_reg(v, 0, h->rex, h->size, b);
    vfl_write_reg(v, h->reg, h->rex, h->size, a);
    vfl_adv(v, h->len, h->ripmask);
    return 0;
}

static int vfl_hot_mov_rm_imm(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_hot_op_t *h) {
    vfl_ea_t ea;

    vfl_hot_ea(v, h, &ea);
    return vfl_exec_store(vm, v, run, &ea, h->imm, h->size, h->rex, h->len, h->ripmask);
}

int vfl_hot_lookup(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint64_t ip,
                   int lm, int prot) {
    vfl_hot_op_t *e = &v->hot[(ip ^ (ip >> 8)) & 63];
    uint64_t tag = ip | ((uint64_t)lm << 62) | ((uint64_t)prot << 61);
    uint64_t phys;
    vfl_mem_slot_t *sl;
    int i;

    (void)lm;
    (void)prot;
    if (e->epoch != vm->tlb_epoch || e->tag != tag || !e->fn || e->len == 0 ||
        e->len > 15)
        return 1;
    if (vfl_translate(vm, v, ip, VFL_MMU_EXEC, &phys)) return 1;
    sl = vfl_find_slot(vm, phys, e->len);
    if (!sl || !(sl->flags & VFL_MEM_FLAG_EXEC)) return 1;
    for (i = 0; i < e->len; i++)
        if (sl->host_mem[phys - sl->guest_phys + (uint64_t)i] != e->bytes[i])
            return 1;
    return e->fn(vm, v, run, e);
}

static int vfl_hot_parse_ea(uint8_t *buf, int *pos, int mod, int rm, int asz,
                            int rex_b, int rex_x, int seg_ovr, int lm,
                            vfl_hot_op_t *h) {
    h->eamod = (uint8_t)mod;
    h->eareg = 0;
    h->ea16rm = 0;
    h->sib_scale = 0;
    h->sib_index = -1;
    h->sib_base = -1;
    h->disp = 0;
    if (mod == 3) {
        h->eareg = (uint8_t)((rm & 7) | (rex_b << 3));
        h->seg = 0;
        return 0;
    }
    if (asz == 2) {
        int seg = 3;

        h->ea16rm = (uint8_t)(rm & 7);
        if (mod == 0 && (rm & 7) == 6) {
            if (*pos + 2 > 15) return -1;
            h->disp = (int16_t)vfl_get16(buf + *pos);
            *pos += 2;
        } else if (mod == 1) {
            if (*pos + 1 > 15) return -1;
            h->disp = (int8_t)buf[*pos];
            *pos += 1;
        } else if (mod == 2) {
            if (*pos + 2 > 15) return -1;
            h->disp = (int16_t)vfl_get16(buf + *pos);
            *pos += 2;
        }
        if ((rm & 7) == 2 || (rm & 7) == 3 || ((rm & 7) == 6 && mod != 0)) seg = 2;
        h->seg = (uint8_t)(seg_ovr >= 0 ? seg_ovr : seg);
        return 0;
    }
    {
        int seg = 3;

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
            h->sib_scale = (int8_t)sc;
            h->sib_index = (ix != 4 || rex_x) ? (int8_t)ix : (int8_t)-1;
            if (mod == 0 && (sib & 7) == 5) {
                if (*pos + 4 > 15) return -1;
                h->disp = (int32_t)vfl_get32(buf + *pos);
                *pos += 4;
                h->sib_base = -1;
            } else {
                h->sib_base = (int8_t)bs;
                if ((bs & 7) == 4 || (bs & 7) == 5) seg = 2;
            }
        } else {
            h->sib_scale = 0;
            h->sib_index = -1;
            if (mod == 0 && (rm & 7) == 5) {
                if (*pos + 4 > 15) return -1;
                h->disp = (int32_t)vfl_get32(buf + *pos);
                *pos += 4;
                h->sib_base = (lm && asz == 8) ? (int8_t)-2 : (int8_t)-1;
            } else {
                h->sib_base = (int8_t)((rm & 7) | (rex_b << 3));
                if ((rm & 7) == 4 || (rm & 7) == 5) seg = 2;
            }
        }
        if (mod == 1) {
            if (*pos + 1 > 15) return -1;
            h->disp = (int8_t)buf[*pos];
            *pos += 1;
        } else if (mod == 2) {
            if (*pos + 4 > 15) return -1;
            h->disp = (int32_t)vfl_get32(buf + *pos);
            *pos += 4;
        }
        h->seg = (uint8_t)(seg_ovr >= 0 ? seg_ovr : seg);
        return 0;
    }
}

static int vfl_hot_modrm(vfl_hot_op_t *h, uint8_t *buf, int *pos, int rex_r, int rex_b,
                         int *mod, int *reg, int *rm) {
    uint8_t m;

    if (*pos >= 15) return -1;
    m = buf[*pos];
    (*pos)++;
    *mod = (m >> 6) & 3;
    *reg = ((m >> 3) & 7) | (rex_r << 3);
    *rm = (m & 7) | (rex_b << 3);
    h->reg = (uint8_t)*reg;
    return 0;
}

void vfl_hot_fill(vfl_vm_t *vm, vfl_vcpu_t *v, uint8_t *buf, uint64_t ip,
                  int lm, int prot, int osz, int asz) {
    int pos = 0;
    int p66 = 0;
    int p67 = 0;
    int seg = -1;
    int rex = 0;
    int rex_w = 0;
    int rex_r = 0;
    int rex_x = 0;
    int rex_b = 0;
    uint8_t op;
    uint8_t op2;
    uint64_t ripmask;
    vfl_hot_op_t h;
    vfl_hot_op_t *e;
    int mod = 0;
    int reg = 0;
    int rm = 0;
    int i;

    (void)v;
    (void)osz;
    (void)asz;
    for (;;) {
        if (pos >= 15) return;
        op = buf[pos];
        if (op == 0x66) { p66 = 1; pos++; continue; }
        if (op == 0x67) { p67 = 1; pos++; continue; }
        if (op == 0x26) { seg = 0; pos++; continue; }
        if (op == 0x2E) { seg = 1; pos++; continue; }
        if (op == 0x36) { seg = 2; pos++; continue; }
        if (op == 0x3E) { seg = 3; pos++; continue; }
        if (op == 0x64) { seg = 4; pos++; continue; }
        if (op == 0x65) { seg = 5; pos++; continue; }
        if (op == 0xF0) { pos++; continue; }
        if (op == 0xF2 || op == 0xF3) { pos++; continue; }
        if (lm && op >= 0x40 && op <= 0x4F) {
            rex = 1;
            rex_w = (op >> 3) & 1;
            rex_r = (op >> 2) & 1;
            rex_x = (op >> 1) & 1;
            rex_b = op & 1;
            pos++;
            continue;
        }
        break;
    }
    if (lm) {
        osz = rex_w ? 8 : (p66 ? 2 : 4);
        asz = p67 ? 4 : 8;
        ripmask = ~0ULL;
    } else if (prot) {
        osz = p66 ? 2 : 4;
        asz = p67 ? 2 : 4;
        ripmask = 0xFFFFFFFFULL;
    } else {
        osz = p66 ? 4 : 2;
        asz = p67 ? 4 : 2;
        ripmask = 0xFFFFULL;
    }
    if (pos >= 15) return;
    op = buf[pos++];
    memset(&h, 0, sizeof(h));
    h.size = (uint8_t)osz;
    h.asz = (uint8_t)asz;
    h.rex = (uint8_t)rex;
    h.ripmask = ripmask;
    h.seg = 0;
    switch (op) {
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:
            h.fn = vfl_hot_xchg_ax;
            h.reg = (uint8_t)((op & 7) | (rex_b << 3));
            break;
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
            int sz = op < 0xB8 ? 1 : osz;

            if (pos + sz > 15) return;
            h.fn = vfl_hot_mov_r_imm;
            h.reg = (uint8_t)((op & 7) | (rex_b << 3));
            h.size = (uint8_t)sz;
            h.imm = 0;
            if (sz == 1) h.imm = buf[pos];
            else if (sz == 2) h.imm = vfl_get16(buf + pos);
            else if (sz == 4) h.imm = vfl_get32(buf + pos);
            else h.imm = vfl_get64(buf + pos);
            pos += sz;
            break;
        }
        case 0x04: case 0x05: case 0x0C: case 0x0D:
        case 0x14: case 0x15: case 0x1C: case 0x1D:
        case 0x24: case 0x25: case 0x2C: case 0x2D:
        case 0x34: case 0x35: case 0x3C: case 0x3D:
        case 0xA8: case 0xA9: {
            int aluop;
            int sz;
            int nowrite = 0;

            if (op == 0xA8 || op == 0xA9) {
                aluop = 4;
                nowrite = 2;
                sz = (op & 1) ? osz : 1;
            } else {
                aluop = (op >> 3) & 7;
                sz = (op & 1) ? osz : 1;
                nowrite = ((op & 0x38) == 0x38) ? 1 : 0;
            }
            if (pos + sz > 15) return;
            h.fn = vfl_hot_alu_r_imm;
            h.sub = (uint8_t)aluop;
            h.sub2 = (uint8_t)nowrite;
            h.size = (uint8_t)sz;
            h.imm = 0;
            if (sz == 1) h.imm = buf[pos];
            else if (sz == 2) h.imm = vfl_get16(buf + pos);
            else if (sz == 4) h.imm = vfl_get32(buf + pos);
            else h.imm = vfl_get64(buf + pos);
            pos += sz;
            break;
        }
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            h.fn = vfl_hot_push_r;
            h.reg = (uint8_t)((op & 7) | (rex_b << 3));
            break;
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            h.fn = vfl_hot_pop_r;
            h.reg = (uint8_t)((op & 7) | (rex_b << 3));
            break;
        case 0x68:
            if (pos + osz > 15) return;
            h.fn = vfl_hot_push_imm;
            h.imm = 0;
            if (osz == 2) h.imm = vfl_get16(buf + pos);
            else if (osz == 4) h.imm = vfl_get32(buf + pos);
            else h.imm = vfl_get64(buf + pos);
            pos += osz;
            break;
        case 0x6A:
            if (pos + 1 > 15) return;
            h.fn = vfl_hot_push_imm;
            h.imm = (uint64_t)(int64_t)(int8_t)buf[pos] & vfl_mask(osz);
            pos += 1;
            break;
        case 0x9C:
            h.fn = vfl_hot_pushf;
            break;
        case 0x9D:
            h.fn = vfl_hot_popf;
            break;
        case 0x88: case 0x89: case 0x8A: case 0x8B:
            if (vfl_hot_modrm(&h, buf, &pos, rex_r, rex_b, &mod, &reg, &rm)) return;
            if (vfl_hot_parse_ea(buf, &pos, mod, rm, asz, rex_b, rex_x, seg, lm, &h))
                return;
            h.fn = vfl_hot_mov_rm;
            h.sub = (uint8_t)((op >> 1) & 1);
            h.size = (uint8_t)((op & 1) ? osz : 1);
            break;
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x08: case 0x09: case 0x0A: case 0x0B:
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x38: case 0x39: case 0x3A: case 0x3B:
            if (vfl_hot_modrm(&h, buf, &pos, rex_r, rex_b, &mod, &reg, &rm)) return;
            if (vfl_hot_parse_ea(buf, &pos, mod, rm, asz, rex_b, rex_x, seg, lm, &h))
                return;
            h.fn = vfl_hot_alu_rm;
            h.sub = (uint8_t)(((op >> 3) & 7) << 1 | ((op >> 1) & 1));
            h.size = (uint8_t)((op & 1) ? osz : 1);
            break;
        case 0x80:
        case 0x81:
        case 0x83: {
            int aluop;
            int sz;

            if (op == 0x82 && lm) return;
            if (vfl_hot_modrm(&h, buf, &pos, rex_r, rex_b, &mod, &reg, &rm)) return;
            aluop = reg;
            if (vfl_hot_parse_ea(buf, &pos, mod, rm, asz, rex_b, rex_x, seg, lm, &h))
                return;
            sz = (op == 0x80) ? 1 : osz;
            if (op == 0x83) {
                if (pos + 1 > 15) return;
                h.imm = (uint64_t)(int64_t)(int8_t)buf[pos] & vfl_mask(osz);
                pos += 1;
            } else {
                if (pos + sz > 15) return;
                h.imm = 0;
                if (sz == 1) h.imm = buf[pos];
                else if (sz == 2) h.imm = vfl_get16(buf + pos);
                else if (sz == 4) h.imm = vfl_get32(buf + pos);
                else h.imm = vfl_get64(buf + pos);
                pos += sz;
            }
            h.fn = vfl_hot_alu_rm_imm;
            h.sub = (uint8_t)aluop;
            h.size = (uint8_t)sz;
            break;
        }
        case 0x84:
        case 0x85:
            if (vfl_hot_modrm(&h, buf, &pos, rex_r, rex_b, &mod, &reg, &rm)) return;
            if (vfl_hot_parse_ea(buf, &pos, mod, rm, asz, rex_b, rex_x, seg, lm, &h))
                return;
            h.fn = vfl_hot_test_rm;
            h.size = (uint8_t)((op & 1) ? osz : 1);
            break;
        case 0x8D:
            if (vfl_hot_modrm(&h, buf, &pos, rex_r, rex_b, &mod, &reg, &rm)) return;
            if (mod == 3) return;
            if (vfl_hot_parse_ea(buf, &pos, mod, rm, asz, rex_b, rex_x, seg, lm, &h))
                return;
            h.fn = vfl_hot_lea;
            if (asz == 2) h.imm = 0xFFFFULL;
            else if (asz == 4 || !lm) h.imm = 0xFFFFFFFFULL;
            else h.imm = ~0ULL;
            break;
        case 0xC6:
        case 0xC7: {
            int sz = (op & 1) ? osz : 1;

            if (vfl_hot_modrm(&h, buf, &pos, rex_r, rex_b, &mod, &reg, &rm)) return;
            if (reg != 0) return;
            if (vfl_hot_parse_ea(buf, &pos, mod, rm, asz, rex_b, rex_x, seg, lm, &h))
                return;
            if (pos + sz > 15) return;
            h.imm = 0;
            if (sz == 1) h.imm = buf[pos];
            else if (sz == 2) h.imm = vfl_get16(buf + pos);
            else if (sz == 4) h.imm = vfl_get32(buf + pos);
            else h.imm = vfl_get64(buf + pos);
            pos += sz;
            h.fn = vfl_hot_mov_rm_imm;
            h.size = (uint8_t)sz;
            break;
        }
        case 0xFE:
            if (vfl_hot_modrm(&h, buf, &pos, rex_r, rex_b, &mod, &reg, &rm)) return;
            if (reg > 1) return;
            if (vfl_hot_parse_ea(buf, &pos, mod, rm, asz, rex_b, rex_x, seg, lm, &h))
                return;
            h.fn = vfl_hot_incdec;
            h.sub = (uint8_t)reg;
            h.size = 1;
            break;
        case 0xFF:
            if (vfl_hot_modrm(&h, buf, &pos, rex_r, rex_b, &mod, &reg, &rm)) return;
            if (reg != 0 && reg != 1 && reg != 2 && reg != 4) return;
            if (vfl_hot_parse_ea(buf, &pos, mod, rm, asz, rex_b, rex_x, seg, lm, &h))
                return;
            h.fn = reg <= 1 ? vfl_hot_incdec : vfl_hot_fficall;
            h.sub = (uint8_t)reg;
            h.size = (uint8_t)osz;
            break;
        case 0xEB:
            if (pos + 1 > 15) return;
            h.fn = vfl_hot_jmp;
            h.imm = (uint64_t)(int64_t)(int8_t)buf[pos];
            pos += 1;
            break;
        case 0xE9:
            if (osz == 2) {
                if (pos + 2 > 15) return;
                h.imm = (uint64_t)(int64_t)(int16_t)vfl_get16(buf + pos);
                pos += 2;
            } else {
                if (pos + 4 > 15) return;
                h.imm = (uint64_t)(int64_t)(int32_t)vfl_get32(buf + pos);
                pos += 4;
            }
            h.fn = vfl_hot_jmp;
            break;
        case 0xE8:
            if (osz == 2) {
                if (pos + 2 > 15) return;
                h.imm = (uint64_t)(int64_t)(int16_t)vfl_get16(buf + pos);
                pos += 2;
            } else {
                if (pos + 4 > 15) return;
                h.imm = (uint64_t)(int64_t)(int32_t)vfl_get32(buf + pos);
                pos += 4;
            }
            h.fn = vfl_hot_call;
            break;
        case 0xC3:
            h.fn = vfl_hot_ret;
            break;
        case 0xC9:
            h.fn = vfl_hot_leave;
            break;
        case 0xE0:
        case 0xE1:
        case 0xE2:
        case 0xE3:
            if (pos + 1 > 15) return;
            h.fn = vfl_hot_loop;
            h.sub = op;
            h.asz = (uint8_t)asz;
            h.imm = (uint64_t)(int64_t)(int8_t)buf[pos];
            pos += 1;
            break;
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            if (pos + 1 > 15) return;
            h.fn = vfl_hot_jcc;
            h.sub = (uint8_t)(op & 15);
            h.imm = (uint64_t)(int64_t)(int8_t)buf[pos];
            pos += 1;
            break;
        case 0x0F:
            if (pos >= 15) return;
            op2 = buf[pos++];
            if (op2 >= 0x80 && op2 <= 0x8F) {
                if (osz == 2) {
                    if (pos + 2 > 15) return;
                    h.imm = (uint64_t)(int64_t)(int16_t)vfl_get16(buf + pos);
                    pos += 2;
                } else {
                    if (pos + 4 > 15) return;
                    h.imm = (uint64_t)(int64_t)(int32_t)vfl_get32(buf + pos);
                    pos += 4;
                }
                h.fn = vfl_hot_jcc;
                h.sub = (uint8_t)(op2 & 15);
            } else if (op2 == 0x1F) {
                int m2;
                int r2;
                int mm2;

                if (vfl_hot_modrm(&h, buf, &pos, rex_r, rex_b, &m2, &r2, &mm2))
                    return;
                if (vfl_hot_parse_ea(buf, &pos, m2, mm2, asz, rex_b, rex_x, seg, lm, &h))
                    return;
                h.fn = vfl_hot_nop;
            } else if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) {
                int ssz = (op2 == 0xB6 || op2 == 0xBE) ? 1 : 2;
                int sx = (op2 == 0xBE || op2 == 0xBF);

                if (vfl_hot_modrm(&h, buf, &pos, rex_r, rex_b, &mod, &reg, &rm)) return;
                if (vfl_hot_parse_ea(buf, &pos, mod, rm, asz, rex_b, rex_x, seg, lm, &h))
                    return;
                h.fn = vfl_hot_movzx;
                h.sub = (uint8_t)(ssz | (sx << 2));
                if (lm && rex_w) h.sub2 = (uint8_t)(sx ? (ssz == 1 ? 0 : 1) : 2);
                else if (osz == 2) h.sub2 = 3;
                else h.sub2 = 4;
            } else {
                return;
            }
            break;
        default:
            return;
    }
    if (pos > 15) return;
    h.len = (uint8_t)pos;
    h.tag = ip | ((uint64_t)lm << 62) | ((uint64_t)prot << 61);
    h.epoch = vm->tlb_epoch;
    for (i = 0; i < pos; i++)
        h.bytes[i] = buf[i];
    e = &v->hot[(ip ^ (ip >> 8)) & 63];
    *e = h;
}
