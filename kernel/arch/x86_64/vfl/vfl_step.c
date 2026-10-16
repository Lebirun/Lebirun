#include "vfl_priv.h"
#include "../syscalls/syscall_defs.h"
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <string.h>
#include <stdint.h>

static int vfl_chunk_range(vfl_vm_t *vm, vfl_vcpu_t *v, uint64_t base, uint64_t k,
                           int s, int wr, uint64_t *pphys, uint8_t **phost) {
    uint64_t p0;
    vfl_mem_slot_t *sl;

    if (vfl_translate(vm, v, base, wr ? VFL_MMU_WRITE : VFL_MMU_READ, &p0))
        return 1;
    sl = vfl_find_slot(vm, p0, k * (uint64_t)s);
    if (!sl) return 1;
    if (wr && !(sl->flags & VFL_MEM_FLAG_WRITE)) return 1;
    if (!wr && !(sl->flags & VFL_MEM_FLAG_READ)) return 1;
    *pphys = p0;
    *phost = sl->host_mem + (p0 - sl->guest_phys);
    return 0;
}

static int vfl_string_chunk(vfl_vm_t *vm, vfl_vcpu_t *v, int op, int rep,
                            int asz, int osz, int sseg, int rex, int n,
                            uint64_t ripmask) {
    uint64_t cnt = vfl_get_count(v, asz);
    uint64_t si = vfl_get_si(v, asz);
    uint64_t di = vfl_get_di(v, asz);
    int df = (v->regs.rflags >> 10) & 1;
    int s = (op & 1) ? osz : 1;
    int64_t step = df ? -(int64_t)s : (int64_t)s;
    uint64_t sbase = vfl_seg_base(v, sseg >= 0 ? sseg : 3);
    uint64_t dbase = vfl_seg_base(v, 0);
    uint64_t k = cnt;
    uint64_t bl;
    uint64_t m;
    int64_t delta;

    if (!rep || cnt == 0) return 1;
    bl = 0x1000ULL - ((sbase + si) & 0xFFFULL);
    if (bl < (uint64_t)s) return 1;
    m = (bl + (uint64_t)s - 1) / (uint64_t)s;
    if (m < k) k = m;
    bl = 0x1000ULL - ((dbase + di) & 0xFFFULL);
    if (bl < (uint64_t)s) return 1;
    m = (bl + (uint64_t)s - 1) / (uint64_t)s;
    if (m < k) k = m;
    if (asz == 2) {
        if (!df) {
            m = (0xFFFFULL - (si & 0xFFFFULL)) / (uint64_t)s + 1;
            if (m < k) k = m;
            m = (0xFFFFULL - (di & 0xFFFFULL)) / (uint64_t)s + 1;
            if (m < k) k = m;
        } else {
            m = (si & 0xFFFFULL) / (uint64_t)s + 1;
            if (m < k) k = m;
            m = (di & 0xFFFFULL) / (uint64_t)s + 1;
            if (m < k) k = m;
        }
    }
    if (k == 0) return 1;
    delta = step * (int64_t)k;
    switch (op) {
        case 0xA4:
        case 0xA5: {
            uint64_t slo = df ? sbase + si - (k - 1) * (uint64_t)s : sbase + si;
            uint64_t dlo = df ? dbase + di - (k - 1) * (uint64_t)s : dbase + di;
            uint64_t sp;
            uint64_t dp;
            uint8_t *sh;
            uint8_t *dh;

            if (!df && dlo > slo && dlo < slo + k * (uint64_t)s) {
                k = (dlo - slo) / (uint64_t)s;
                if (k == 0) return 1;
                delta = step * (int64_t)k;
            } else if (df && slo > dlo && slo < dlo + k * (uint64_t)s) {
                return 1;
            }
            if (vfl_chunk_range(vm, v, slo, k, s, 0, &sp, &sh)) return 1;
            if (vfl_chunk_range(vm, v, dlo, k, s, 1, &dp, &dh)) return 1;
            memmove(dh, sh, (size_t)(k * (uint64_t)s));
            vfl_mark_dirty(vm, dp, k * (uint64_t)s);
            break;
        }
        case 0xAA:
        case 0xAB: {
            uint64_t dlo = df ? dbase + di - (k - 1) * (uint64_t)s : dbase + di;
            uint64_t dp;
            uint8_t *dh;
            uint8_t pat[8];
            uint64_t j;

            if (vfl_chunk_range(vm, v, dlo, k, s, 1, &dp, &dh)) return 1;
            vfl_put64(pat, vfl_read_reg(v, 0, rex, s) & vfl_mask(s), s);
            for (j = 0; j < k; j++)
                memcpy(dh + j * (uint64_t)s, pat, (size_t)s);
            vfl_mark_dirty(vm, dp, k * (uint64_t)s);
            break;
        }
        case 0xAC:
        case 0xAD: {
            uint64_t slo = df ? sbase + si - (k - 1) * (uint64_t)s : sbase + si;
            uint64_t sp;
            uint8_t *sh;
            uint64_t val = 0;

            if (vfl_chunk_range(vm, v, slo, k, s, 0, &sp, &sh)) return 1;
            memcpy(&val, sh + (df ? 0 : (k - 1) * (uint64_t)s), (size_t)s);
            vfl_write_reg(v, 0, rex, s, val & vfl_mask(s));
            break;
        }
        default: {
            uint64_t slo = df ? sbase + si - (k - 1) * (uint64_t)s : sbase + si;
            uint64_t dlo = df ? dbase + di - (k - 1) * (uint64_t)s : dbase + di;
            uint64_t sp;
            uint64_t dp;
            uint8_t *sh;
            uint8_t *dh;
            uint64_t j;
            uint64_t hit = k;
            uint64_t av = vfl_read_reg(v, 0, rex, s) & vfl_mask(s);

            if (op == 0xA6 || op == 0xA7) {
                if (vfl_chunk_range(vm, v, slo, k, s, 0, &sp, &sh)) return 1;
                if (vfl_chunk_range(vm, v, dlo, k, s, 0, &dp, &dh)) return 1;
            } else {
                if (vfl_chunk_range(vm, v, dlo, k, s, 0, &dp, &dh)) return 1;
                sh = dh;
                sp = dp;
            }
            for (j = 0; j < k; j++) {
                uint64_t o = df ? (k - 1 - j) * (uint64_t)s : j * (uint64_t)s;
                uint64_t x = 0;
                uint64_t y = 0;
                int eq;

                memcpy(&x, sh + o, (size_t)s);
                if (op == 0xA6 || op == 0xA7) memcpy(&y, dh + o, (size_t)s);
                else y = x, x = av;
                eq = (x & vfl_mask(s)) == (y & vfl_mask(s));
                if ((rep == 3 && !eq) || (rep == 2 && eq)) {
                    hit = j;
                    break;
                }
            }
            {
                uint64_t o = df ? (k - 1 - hit) * (uint64_t)s : hit * (uint64_t)s;
                uint64_t x = 0;
                uint64_t y = 0;

                if (hit >= k) {
                    o = df ? 0 : (k - 1) * (uint64_t)s;
                    hit = k - 1;
                }
                memcpy(&x, sh + o, (size_t)s);
                if (op == 0xA6 || op == 0xA7) memcpy(&y, dh + o, (size_t)s);
                else y = x, x = av;
                vfl_do_alu(v, VFL_RMW_CMP, x & vfl_mask(s), y & vfl_mask(s), s);
                m = hit + 1;
            }
            if (op == 0xA6 || op == 0xA7)
                vfl_set_si(v, asz, (si + (uint64_t)(step * (int64_t)m)) & vfl_mask(asz));
            vfl_set_di(v, asz, (di + (uint64_t)(step * (int64_t)m)) & vfl_mask(asz));
            vfl_set_count(v, asz, (cnt - m) & vfl_mask(asz));
            {
                int zf = (v->regs.rflags >> 6) & 1;
                int repz = (rep == 3);
                int repnz = (rep == 2);
                uint64_t left = (cnt - m) & vfl_mask(asz);

                if ((repz && !zf) || (repnz && zf) || left == 0) {
                    vfl_adv(v, n, ripmask);
                    return 0;
                }
                return 0;
            }
        }
    }
    if (op == 0xA4 || op == 0xA5 || op == 0xAC || op == 0xAD)
        vfl_set_si(v, asz, (si + (uint64_t)delta) & vfl_mask(asz));
    if (op != 0xAC && op != 0xAD)
        vfl_set_di(v, asz, (di + (uint64_t)delta) & vfl_mask(asz));
    vfl_set_count(v, asz, (cnt - k) & vfl_mask(asz));
    if (((cnt - k) & vfl_mask(asz)) == 0) {
        vfl_adv(v, n, ripmask);
        return 0;
    }
    return 0;
}
static int vfl_far_load_cs(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint16_t sel) {
    uint64_t base;
    uint64_t lim;
    uint64_t idx;
    uint8_t d[8];
    uint64_t segbase;
    uint64_t seglim;
    uint8_t access;
    int r;
    int cpl = v->sregs.cs.selector & 3;
    int rpl = sel & 3;
    int dpl;

    if (sel & 4) {
        base = v->sregs.ldt.base;
        lim = v->sregs.ldt.limit;
    } else {
        base = v->sregs.gdt_base;
        lim = v->sregs.gdt_limit;
    }
    idx = ((uint64_t)(sel >> 3)) * 8;
    if (idx + 7 > lim)
        return vfl_exit_exception(v, run, 13, 0);
    r = vfl_mem_read(vm, v, base + idx, 8, d);
    if (r == -2) return vfl_mem_fault(v, run);
    if (r) return vfl_memerr(v, run, r);
    access = d[5];
    if (!(access & 0x80))
        return vfl_exit_exception(v, run, 11, 0);
    if (!(access & 0x10) || !(access & 0x08))
        return vfl_exit_exception(v, run, 13, 0);
    dpl = (access >> 5) & 3;
    if (access & 0x04) {
        if (dpl > cpl)
            return vfl_exit_exception(v, run, 13, 0);
    } else {
        if (rpl > cpl || dpl != cpl)
            return vfl_exit_exception(v, run, 13, 0);
    }
    segbase = (uint64_t)d[2] | ((uint64_t)d[3] << 8) | ((uint64_t)d[4] << 16) |
              ((uint64_t)d[7] << 24);
    seglim = (uint64_t)vfl_get16(d) | ((uint64_t)(d[6] & 0x0F) << 16);
    if (d[6] & 0x80)
        seglim = (seglim << 12) | 0xFFFULL;
    v->sregs.cs.selector = sel;
    v->sregs.cs.base = segbase;
    v->sregs.cs.limit = (uint32_t)seglim;
    v->sregs.cs.attrib = 0x80 | (uint16_t)(dpl << 5) | 0x10 | (access & 0x0F) |
                         ((d[6] & 0x40) ? 0x400 : 0);
    return 0;
}

int vfl_step(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run) {
    uint8_t buf[15];
    uint64_t ip;
    uint64_t ripmask;
    int fr;
    int pos = 0;
    int p66 = 0;
    int p67 = 0;
    int seg = -1;
    int rep = 0;
    int rex = 0;
    int rex_w = 0;
    int rex_r = 0;
    int rex_x = 0;
    int rex_b = 0;
    int lm;
    int prot;
    int osz;
    int asz;
    uint8_t op;
    uint8_t m;
    int mod;
    int reg;
    int rm;
    vfl_ea_t ea;
    uint64_t a;
    uint64_t b;
    uint64_t r;
    int n;
    int sz;
    int rr;
    int sr;

    v->int_shadow = 0;
    lm = vfl_long_mode(v);
    prot = (v->sregs.cr0 & 1ULL) != 0;
    if (lm) ripmask = ~0ULL;
    else if (prot) ripmask = 0xFFFFFFFFULL;
    else ripmask = 0xFFFFULL;

    ip = vfl_seg_base(v, 1) + (v->regs.rip & ripmask);
    if (!vfl_hot_lookup(vm, v, run, ip, lm, prot)) return 0;
    fr = vfl_fetch_buf(vm, v, ip, buf, 15);
    if (fr == -2) return vfl_mem_fault(v, run);
    if (fr == 1) return vfl_exit_exception(v, run, 14, 0);
    if (fr < 0) return vfl_exit_exception(v, run, 13, 0);

    for (;;) {
        if (pos >= 15) return vfl_exit_internal(v, run);
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
        if (op == 0xF2) { rep = 2; pos++; continue; }
        if (op == 0xF3) { rep = 3; pos++; continue; }
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
    } else if (prot) {
        osz = p66 ? 2 : 4;
        asz = p67 ? 2 : 4;
    } else {
        osz = p66 ? 4 : 2;
        asz = p67 ? 4 : 2;
    }

    if (pos >= 15) return vfl_exit_internal(v, run);
    op = buf[pos++];
    vfl_hot_fill(vm, v, buf, ip, lm, prot, osz, asz);

    switch (op) {
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
        case 0x18:
        case 0x19:
        case 0x1A:
        case 0x1B:
        case 0x20:
        case 0x21:
        case 0x22:
        case 0x23:
        case 0x28:
        case 0x29:
        case 0x2A:
        case 0x2B:
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x33:
        case 0x38:
        case 0x39:
        case 0x3A:
        case 0x3B: {
            int aluop = (op >> 3) & 7;

            sz = (op & 1) ? osz : 1;
            if (pos >= 15) return vfl_exit_internal(v, run);
            m = buf[pos++];
            mod = (m >> 6) & 3;
            reg = ((m >> 3) & 7) | (rex_r << 3);
            rm = (m & 7) | (rex_b << 3);
            if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                return vfl_exit_internal(v, run);
            n = pos;
            if (!ea.is_reg && ea.rip_rel)
                ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
            if (op & 2) {
                uint64_t src = 0;

                rr = vfl_ea_read(vm, v, &ea, sz, rex, &src);
                if (rr == -2) return vfl_mem_fault(v, run);
                if (rr == 1 && !ea.is_reg)
                    return vfl_mmio_or_fault(vm, v, run, ea.addr, sz, VFL_P_MMIO_REG,
                                             reg, rex, 0, 0, n, ripmask);
                if (rr != 0) return vfl_exit_internal(v, run);
                src &= vfl_mask(sz);
                a = vfl_read_reg(v, reg, rex, sz);
                r = vfl_do_alu(v, aluop, a, src, sz);
                if (aluop != VFL_RMW_CMP) vfl_write_reg(v, reg, rex, sz, r);
            } else {
                a = vfl_read_reg(v, reg, rex, sz);
                return vfl_exec_rmw(vm, v, run, &ea, aluop, a, sz, rex, n, ripmask);
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x04:
        case 0x05:
        case 0x0C:
        case 0x0D:
        case 0x14:
        case 0x15:
        case 0x1C:
        case 0x1D:
        case 0x24:
        case 0x25:
        case 0x2C:
        case 0x2D:
        case 0x34:
        case 0x35:
        case 0x3C:
        case 0x3D: {
            int aluop = (op >> 3) & 7;

            sz = (op & 1) ? osz : 1;
            if (pos + sz > 15) return vfl_exit_internal(v, run);
            b = 0;
            if (sz == 1) b = buf[pos];
            else if (sz == 2) b = vfl_get16(buf + pos);
            else if (sz == 4) b = vfl_get32(buf + pos);
            else b = vfl_get64(buf + pos);
            pos += sz;
            n = pos;
            a = vfl_read_reg(v, 0, rex, sz);
            r = vfl_do_alu(v, aluop, a, b, sz);
            if (aluop != VFL_RMW_CMP) vfl_write_reg(v, 0, rex, sz, r);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x80:
        case 0x82:
        case 0x81:
        case 0x83: {
            int aluop;

            if (op == 0x82 && lm) return vfl_exit_exception(v, run, 6, 0);
            if (pos >= 15) return vfl_exit_internal(v, run);
            m = buf[pos++];
            mod = (m >> 6) & 3;
            aluop = (m >> 3) & 7;
            rm = (m & 7) | (rex_b << 3);
            if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                return vfl_exit_internal(v, run);
            sz = (op == 0x83) ? osz : ((op == 0x81) ? osz : 1);
            if (op == 0x83) {
                if (pos + 1 > 15) return vfl_exit_internal(v, run);
                b = (uint64_t)(int64_t)(int8_t)buf[pos] & vfl_mask(osz);
                pos += 1;
            } else {
                if (pos + sz > 15) return vfl_exit_internal(v, run);
                b = 0;
                if (sz == 1) b = buf[pos];
                else if (sz == 2) b = vfl_get16(buf + pos);
                else if (sz == 4) b = vfl_get32(buf + pos);
                else b = vfl_get64(buf + pos);
                pos += sz;
            }
            n = pos;
            if (!ea.is_reg && ea.rip_rel)
                ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
            return vfl_exec_rmw(vm, v, run, &ea, aluop, b, (op == 0x83 || op == 0x81) ? osz : 1,
                                rex, n, ripmask);
        }

        case 0x84:
        case 0x85:
            sz = (op & 1) ? osz : 1;
            if (pos >= 15) return vfl_exit_internal(v, run);
            m = buf[pos++];
            mod = (m >> 6) & 3;
            reg = ((m >> 3) & 7) | (rex_r << 3);
            rm = (m & 7) | (rex_b << 3);
            if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                return vfl_exit_internal(v, run);
            n = pos;
            if (!ea.is_reg && ea.rip_rel)
                ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
            a = vfl_read_reg(v, reg, rex, sz);
            rr = vfl_ea_read(vm, v, &ea, sz, rex, &b);
            if (rr == -2) return vfl_mem_fault(v, run);
            if (rr == 1 && !ea.is_reg)
                return vfl_mmio_or_fault(vm, v, run, ea.addr, sz, VFL_P_MMIO_RMW,
                                         0, 0, a & vfl_mask(sz), 14, n, ripmask);
            if (rr != 0) return vfl_exit_internal(v, run);
            vfl_flag_logic(v, (a & b) & vfl_mask(sz), sz);
            vfl_adv(v, n, ripmask);
            return 0;

        case 0xA8:
        case 0xA9:
            sz = (op & 1) ? osz : 1;
            if (pos + sz > 15) return vfl_exit_internal(v, run);
            b = 0;
            if (sz == 1) b = buf[pos];
            else if (sz == 2) b = vfl_get16(buf + pos);
            else if (sz == 4) b = vfl_get32(buf + pos);
            else b = vfl_get64(buf + pos);
            pos += sz;
            n = pos;
            a = vfl_read_reg(v, 0, rex, sz);
            vfl_flag_logic(v, (a & b) & vfl_mask(sz), sz);
            vfl_adv(v, n, ripmask);
            return 0;

        case 0x88:
        case 0x89:
        case 0x8A:
        case 0x8B:
            sz = (op & 1) ? osz : 1;
            if (pos >= 15) return vfl_exit_internal(v, run);
            m = buf[pos++];
            mod = (m >> 6) & 3;
            reg = ((m >> 3) & 7) | (rex_r << 3);
            rm = (m & 7) | (rex_b << 3);
            if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                return vfl_exit_internal(v, run);
            n = pos;
            if (!ea.is_reg && ea.rip_rel)
                ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
            if (op & 2)
                return vfl_exec_load_reg(vm, v, run, &ea, reg, sz, rex, n, ripmask);
            a = vfl_read_reg(v, reg, rex, sz);
            return vfl_exec_store(vm, v, run, &ea, a, sz, rex, n, ripmask);

        case 0x86:
        case 0x87: {
            uint64_t t1;
            uint64_t t2 = 0;

            sz = (op & 1) ? osz : 1;
            if (pos >= 15) return vfl_exit_internal(v, run);
            m = buf[pos++];
            mod = (m >> 6) & 3;
            reg = ((m >> 3) & 7) | (rex_r << 3);
            rm = (m & 7) | (rex_b << 3);
            if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                return vfl_exit_internal(v, run);
            n = pos;
            if (!ea.is_reg && ea.rip_rel)
                ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
            t1 = vfl_read_reg(v, reg, rex, sz);
            if (ea.is_reg) {
                t2 = vfl_read_reg(v, ea.reg, rex, sz);
                vfl_write_reg(v, ea.reg, rex, sz, t1);
                vfl_write_reg(v, reg, rex, sz, t2);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            rr = vfl_mem_read(vm, v, ea.addr, (uint64_t)sz, (uint8_t *)&t2);
            if (rr == -2) return vfl_mem_fault(v, run);
            if (rr == 1)
                return vfl_mmio_or_fault(vm, v, run, ea.addr, sz, VFL_P_MMIO_RMW,
                                         reg, rex, t1 & vfl_mask(sz), 12, n, ripmask);
            if (rr < 0)
                return vfl_exit_exception(v, run, 13, 0);
            t2 &= vfl_mask(sz);
            vfl_write_reg(v, reg, rex, sz, t2);
            return vfl_exec_store(vm, v, run, &ea, t1, sz, rex, n, ripmask);
        }

        case 0x90:
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0x91:
        case 0x92:
        case 0x93:
        case 0x94:
        case 0x95:
        case 0x96:
        case 0x97: {
            int q = (op & 7) | (rex_b << 3);

            a = vfl_read_reg(v, 0, rex, osz);
            b = vfl_read_reg(v, q, rex, osz);
            vfl_write_reg(v, 0, rex, osz, b);
            vfl_write_reg(v, q, rex, osz, a);
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xB0:
        case 0xB1:
        case 0xB2:
        case 0xB3:
        case 0xB4:
        case 0xB5:
        case 0xB6:
        case 0xB7:
            if (pos + 1 > 15) return vfl_exit_internal(v, run);
            vfl_write_reg(v, (op & 7) | (rex_b << 3), rex, 1, buf[pos]);
            pos += 1;
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0xB8:
        case 0xB9:
        case 0xBA:
        case 0xBB:
        case 0xBC:
        case 0xBD:
        case 0xBE:
        case 0xBF:
            if (pos + osz > 15) return vfl_exit_internal(v, run);
            b = 0;
            if (osz == 2) b = vfl_get16(buf + pos);
            else if (osz == 4) b = vfl_get32(buf + pos);
            else b = vfl_get64(buf + pos);
            vfl_write_reg(v, (op & 7) | (rex_b << 3), rex, osz, b);
            pos += osz;
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0xA0:
        case 0xA1:
        case 0xA2:
        case 0xA3: {
            uint64_t off;
            uint64_t addr;
            int s = seg >= 0 ? seg : 3;

            sz = (op & 1) ? osz : 1;
            if (asz == 8) {
                if (pos + 4 > 15) return vfl_exit_internal(v, run);
                off = vfl_get32(buf + pos);
                pos += 4;
            } else if (asz == 4) {
                if (pos + 4 > 15) return vfl_exit_internal(v, run);
                off = vfl_get32(buf + pos);
                pos += 4;
            } else {
                if (pos + 2 > 15) return vfl_exit_internal(v, run);
                off = vfl_get16(buf + pos);
                pos += 2;
            }
            n = pos;
            addr = vfl_seg_base(v, s) + off;
            if (op & 2) {
                a = vfl_read_reg(v, 0, rex, sz);
                ea.is_reg = 0;
                ea.addr = addr;
                return vfl_exec_store(vm, v, run, &ea, a, sz, rex, n, ripmask);
            }
            b = 0;
            rr = vfl_mem_read(vm, v, addr, (uint64_t)sz, (uint8_t *)&b);
            if (rr == -2) return vfl_mem_fault(v, run);
            if (rr == 1)
                return vfl_mmio_or_fault(vm, v, run, addr, sz, VFL_P_MMIO_REG,
                                         0, rex, 0, 0, n, ripmask);
            if (rr < 0)
                return vfl_exit_exception(v, run, 13, 0);
            vfl_write_reg(v, 0, rex, sz, b & vfl_mask(sz));
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xC6:
        case 0xC7:
            sz = (op & 1) ? osz : 1;
            if (pos >= 15) return vfl_exit_internal(v, run);
            m = buf[pos++];
            mod = (m >> 6) & 3;
            if (((m >> 3) & 7) != 0) return vfl_exit_exception(v, run, 6, 0);
            rm = (m & 7) | (rex_b << 3);
            if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                return vfl_exit_internal(v, run);
            if (pos + sz > 15) return vfl_exit_internal(v, run);
            b = 0;
            if (sz == 1) b = buf[pos];
            else if (sz == 2) b = vfl_get16(buf + pos);
            else if (sz == 4) b = vfl_get32(buf + pos);
            else b = vfl_get64(buf + pos);
            pos += sz;
            n = pos;
            if (!ea.is_reg && ea.rip_rel)
                ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
            return vfl_exec_store(vm, v, run, &ea, b, sz, rex, n, ripmask);

        case 0x8D:
            if (pos >= 15) return vfl_exit_internal(v, run);
            m = buf[pos++];
            mod = (m >> 6) & 3;
            reg = ((m >> 3) & 7) | (rex_r << 3);
            rm = (m & 7) | (rex_b << 3);
            if (mod == 3) return vfl_exit_exception(v, run, 6, 0);
            if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                return vfl_exit_internal(v, run);
            n = pos;
            if (ea.rip_rel)
                ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
            if (asz == 2) vfl_write_reg(v, reg, rex, osz, ea.addr & 0xFFFFULL);
            else if (asz == 4 || !lm) vfl_write_reg(v, reg, rex, osz, ea.addr & 0xFFFFFFFFULL);
            else vfl_write_reg(v, reg, rex, osz, ea.addr);
            vfl_adv(v, n, ripmask);
            return 0;

        case 0x8C:
        case 0x8E: {
            vfl_segment_t *s;

            if (pos >= 15) return vfl_exit_internal(v, run);
            m = buf[pos++];
            mod = (m >> 6) & 3;
            reg = (m >> 3) & 7;
            rm = (m & 7) | (rex_b << 3);
            if (reg > 5 || reg == 1) return vfl_exit_exception(v, run, 6, 0);
            if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                return vfl_exit_internal(v, run);
            n = pos;
            if (!ea.is_reg && ea.rip_rel)
                ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
            vfl_seg_ptr(v, reg, &s);
            if (op == 0x8C)
                return vfl_exec_store(vm, v, run, &ea, (uint64_t)s->selector, 2, rex, n, ripmask);
            b = 0;
            rr = vfl_ea_read(vm, v, &ea, 2, rex, &b);
            if (rr == -2) return vfl_mem_fault(v, run);
            if (rr != 0) {
                if (ea.is_reg) return vfl_exit_internal(v, run);
                return vfl_memerr(v, run, rr);
            }
            s->selector = (uint16_t)(b & 0xFFFFULL);
            if (!(v->sregs.cr0 & 1ULL)) s->base = (uint64_t)s->selector << 4;
            if (reg == 2) v->int_shadow = 1;
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x8F:
            if (pos >= 15) return vfl_exit_internal(v, run);
            m = buf[pos++];
            mod = (m >> 6) & 3;
            if (((m >> 3) & 7) != 0) return vfl_exit_exception(v, run, 6, 0);
            rm = (m & 7) | (rex_b << 3);
            if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                return vfl_exit_internal(v, run);
            n = pos;
            if (!ea.is_reg && ea.rip_rel)
                ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
            if (vfl_pop(vm, v, run, osz, &a)) return 1;
            return vfl_exec_store(vm, v, run, &ea, a, osz, rex, n, ripmask);

        case 0x50:
        case 0x51:
        case 0x52:
        case 0x53:
        case 0x54:
        case 0x55:
        case 0x56:
        case 0x57:
            a = vfl_read_reg(v, (op & 7) | (rex_b << 3), rex, osz);
            if (vfl_push(vm, v, run, a, osz)) return 1;
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0x58:
        case 0x59:
        case 0x5A:
        case 0x5B:
        case 0x5C:
        case 0x5D:
        case 0x5E:
        case 0x5F:
            if (vfl_pop(vm, v, run, osz, &a)) return 1;
            vfl_write_reg(v, (op & 7) | (rex_b << 3), rex, osz, a);
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

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
            if (lm) return vfl_exit_internal(v, run);
            a = vfl_read_reg(v, op & 7, 0, osz);
            if (op & 1) r = vfl_do_alu(v, VFL_RMW_DEC, a, 0, osz);
            else r = vfl_do_alu(v, VFL_RMW_INC, a, 0, osz);
            vfl_write_reg(v, op & 7, 0, osz, r);
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0x60:
            if (osz == 2) {
                uint16_t vals[8];
                int i;

                vals[0] = (uint16_t)v->regs.rax;
                vals[1] = (uint16_t)v->regs.rcx;
                vals[2] = (uint16_t)v->regs.rdx;
                vals[3] = (uint16_t)v->regs.rbx;
                vals[4] = (uint16_t)vfl_sp(v);
                vals[5] = (uint16_t)v->regs.rbp;
                vals[6] = (uint16_t)v->regs.rsi;
                vals[7] = (uint16_t)v->regs.rdi;
                for (i = 0; i < 8; i++)
                    if (vfl_push(vm, v, run, vals[i], 2)) return 1;
            } else {
                uint64_t vals[8];
                int i;

                vals[0] = v->regs.rax & vfl_mask(osz);
                vals[1] = v->regs.rcx & vfl_mask(osz);
                vals[2] = v->regs.rdx & vfl_mask(osz);
                vals[3] = v->regs.rbx & vfl_mask(osz);
                vals[4] = vfl_sp(v);
                vals[5] = v->regs.rbp & vfl_mask(osz);
                vals[6] = v->regs.rsi & vfl_mask(osz);
                vals[7] = v->regs.rdi & vfl_mask(osz);
                for (i = 0; i < 8; i++)
                    if (vfl_push(vm, v, run, vals[i], osz)) return 1;
            }
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0x61:
            if (osz == 2) {
                uint64_t vals[8];
                int i;

                for (i = 7; i >= 0; i--)
                    if (vfl_pop(vm, v, run, 2, &vals[i])) return 1;
                v->regs.rdi = (v->regs.rdi & ~0xFFFFULL) | vals[7];
                v->regs.rsi = (v->regs.rsi & ~0xFFFFULL) | vals[6];
                v->regs.rbp = (v->regs.rbp & ~0xFFFFULL) | vals[5];
                v->regs.rbx = (v->regs.rbx & ~0xFFFFULL) | vals[3];
                v->regs.rdx = (v->regs.rdx & ~0xFFFFULL) | vals[2];
                v->regs.rcx = (v->regs.rcx & ~0xFFFFULL) | vals[1];
                v->regs.rax = (v->regs.rax & ~0xFFFFULL) | vals[0];
            } else {
                uint64_t vals[8];
                int i;

                for (i = 7; i >= 0; i--)
                    if (vfl_pop(vm, v, run, osz, &vals[i])) return 1;
                vfl_write_reg(v, 7, rex, osz, vals[7]);
                vfl_write_reg(v, 6, rex, osz, vals[6]);
                vfl_write_reg(v, 5, rex, osz, vals[5]);
                vfl_write_reg(v, 3, rex, osz, vals[3]);
                vfl_write_reg(v, 2, rex, osz, vals[2]);
                vfl_write_reg(v, 1, rex, osz, vals[1]);
                vfl_write_reg(v, 0, rex, osz, vals[0]);
            }
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0x68:
            if (pos + osz > 15) return vfl_exit_internal(v, run);
            b = 0;
            if (osz == 2) b = vfl_get16(buf + pos);
            else if (osz == 4) b = vfl_get32(buf + pos);
            else b = vfl_get64(buf + pos);
            pos += osz;
            if (vfl_push(vm, v, run, b, osz)) return 1;
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0x6A:
            if (pos + 1 > 15) return vfl_exit_internal(v, run);
            b = (uint64_t)(int64_t)(int8_t)buf[pos] & vfl_mask(osz);
            pos += 1;
            if (vfl_push(vm, v, run, b, osz)) return 1;
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0x9C:
            if (vfl_push(vm, v, run, v->regs.rflags, osz)) return 1;
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0x9D:
            if (vfl_pop(vm, v, run, osz, &a)) return 1;
            v->regs.rflags = (a & vfl_mask(osz)) | 0x2ULL;
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0x9E:
            v->regs.rax = (v->regs.rax & ~0xFF00ULL) |
                (((v->regs.rflags & (1ULL << 7)) ? 0x80ULL : 0) |
                 ((v->regs.rflags & (1ULL << 6)) ? 0x40ULL : 0) |
                 ((v->regs.rflags & (1ULL << 4)) ? 0x10ULL : 0) |
                 ((v->regs.rflags & (1ULL << 2)) ? 0x04ULL : 0) |
                 0x02ULL |
                 ((v->regs.rflags & (1ULL << 0)) ? 0x01ULL : 0)) << 8;
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0x9F: {
            uint64_t ah = (v->regs.rax >> 8) & 0xFFULL;

            if (ah & 0x80) v->regs.rflags |= (1ULL << 7);
            else v->regs.rflags &= ~(1ULL << 7);
            if (ah & 0x40) v->regs.rflags |= (1ULL << 6);
            else v->regs.rflags &= ~(1ULL << 6);
            if (ah & 0x10) v->regs.rflags |= (1ULL << 4);
            else v->regs.rflags &= ~(1ULL << 4);
            if (ah & 0x04) v->regs.rflags |= (1ULL << 2);
            else v->regs.rflags &= ~(1ULL << 2);
            if (ah & 0x01) v->regs.rflags |= (1ULL << 0);
            else v->regs.rflags &= ~(1ULL << 0);
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x98:
            if (osz == 2) {
                int8_t al = (int8_t)(v->regs.rax & 0xFFULL);

                v->regs.rax = (v->regs.rax & ~0xFFFFULL) | (uint64_t)(uint16_t)(int16_t)al;
            } else if (osz == 4) {
                int16_t ax = (int16_t)(v->regs.rax & 0xFFFFULL);

                v->regs.rax = (v->regs.rax & ~0xFFFFFFFFULL) | (uint64_t)(uint32_t)(int32_t)ax;
            } else {
                int32_t eax = (int32_t)(v->regs.rax & 0xFFFFFFFFULL);

                v->regs.rax = (uint64_t)(int64_t)eax;
            }
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0x99:
            if (osz == 2) {
                if (v->regs.rax & 0x8000ULL) v->regs.rdx |= 0xFFFFULL;
                else v->regs.rdx &= ~0xFFFFULL;
            } else if (osz == 4) {
                if (v->regs.rax & 0x80000000ULL) v->regs.rdx |= 0xFFFFFFFFULL;
                else v->regs.rdx &= ~0xFFFFFFFFULL;
            } else {
                if (v->regs.rax & 0x8000000000000000ULL) v->regs.rdx = ~0ULL;
                else v->regs.rdx = 0;
            }
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0xF8:
            v->regs.rflags &= ~(1ULL << 0);
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0xF9:
            v->regs.rflags |= (1ULL << 0);
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0xF5:
            v->regs.rflags ^= (1ULL << 0);
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0xFC:
            v->regs.rflags &= ~(1ULL << 10);
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0xFD:
            v->regs.rflags |= (1ULL << 10);
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0xFA:
            v->regs.rflags &= ~(1ULL << 9);
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0xFB:
            v->regs.rflags |= (1ULL << 9);
            v->int_shadow = 1;
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0xF4:
            n = pos;
            vfl_adv(v, n, ripmask);
            v->halted = 1;
            run->exit_reason = VFL_EXIT_HLT;
            v->last_exit = *run;
            return 1;

        case 0xEB:
            if (pos + 1 > 15) return vfl_exit_internal(v, run);
            n = pos + 1;
            v->regs.rip = (v->regs.rip & ~ripmask) |
                ((v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)(int8_t)buf[pos]) & ripmask);
            return 0;

        case 0xE9:
            if (pos + 4 > 15) return vfl_exit_internal(v, run);
            if (osz == 2) {
                int16_t rel = (int16_t)vfl_get16(buf + pos);

                n = pos + 2;
                v->regs.rip = (v->regs.rip & ~ripmask) |
                    ((v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)rel) & ripmask);
            } else {
                int32_t rel = (int32_t)vfl_get32(buf + pos);

                n = pos + 4;
                v->regs.rip = (v->regs.rip & ~ripmask) |
                    ((v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)rel) & ripmask);
            }
            return 0;

        case 0x70:
        case 0x71:
        case 0x72:
        case 0x73:
        case 0x74:
        case 0x75:
        case 0x76:
        case 0x77:
        case 0x78:
        case 0x79:
        case 0x7A:
        case 0x7B:
        case 0x7C:
        case 0x7D:
        case 0x7E:
        case 0x7F:
            if (pos + 1 > 15) return vfl_exit_internal(v, run);
            n = pos + 1;
            if (vfl_cond(v, op & 15))
                v->regs.rip = (v->regs.rip & ~ripmask) |
                    ((v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)(int8_t)buf[pos]) & ripmask);
            else
                vfl_adv(v, n, ripmask);
            return 0;

        case 0xE0:
        case 0xE1:
        case 0xE2:
        case 0xE3: {
            uint64_t cnt = vfl_get_count(v, asz);

            if (pos + 1 > 15) return vfl_exit_internal(v, run);
            n = pos + 1;
            if (op != 0xE3) {
                cnt = (cnt - 1) & vfl_mask(asz);
                vfl_set_count(v, asz, cnt);
            }
            b = 0;
            if (op == 0xE0 && cnt != 0 && !((v->regs.rflags >> 6) & 1)) b = 1;
            if (op == 0xE1 && cnt != 0 && ((v->regs.rflags >> 6) & 1)) b = 1;
            if (op == 0xE2 && cnt != 0) b = 1;
            if (op == 0xE3 && cnt == 0) b = 1;
            if (b)
                v->regs.rip = (v->regs.rip & ~ripmask) |
                    ((v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)(int8_t)buf[pos]) & ripmask);
            else
                vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xE8: {
            uint64_t ret;
            int64_t rel;

            if (osz == 2) {
                if (pos + 2 > 15) return vfl_exit_internal(v, run);
                rel = (int16_t)vfl_get16(buf + pos);
                n = pos + 2;
            } else {
                if (pos + 4 > 15) return vfl_exit_internal(v, run);
                rel = (int32_t)vfl_get32(buf + pos);
                n = pos + 4;
            }
            ret = (v->regs.rip + (uint64_t)n) & ripmask;
            if (vfl_push(vm, v, run, ret, osz)) return 1;
            v->regs.rip = (v->regs.rip & ~ripmask) | ((ret + (uint64_t)rel) & ripmask);
            return 0;
        }

        case 0xC3:
            if (vfl_pop(vm, v, run, osz, &a)) return 1;
            v->regs.rip = (v->regs.rip & ~ripmask) | (a & ripmask);
            return 0;

        case 0xC2: {
            uint64_t val;

            if (pos + 2 > 15) return vfl_exit_internal(v, run);
            val = vfl_get16(buf + pos);
            n = pos + 2;
            if (vfl_pop(vm, v, run, osz, &a)) return 1;
            v->regs.rip = (v->regs.rip & ~ripmask) | (a & ripmask);
            vfl_set_sp(v, (vfl_sp(v) + val) & vfl_stack_mask(v));
            return 0;
        }

        case 0xC9: {
            uint64_t bp;

            if (osz == 2) bp = v->regs.rbp & 0xFFFFULL;
            else if (osz == 4) bp = v->regs.rbp & 0xFFFFFFFFULL;
            else bp = v->regs.rbp;
            vfl_set_sp(v, bp);
            if (vfl_pop(vm, v, run, osz, &a)) return 1;
            vfl_write_reg(v, 5, rex, osz, a);
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xC8: {
            uint64_t iw;
            uint64_t level;
            uint64_t frame;
            uint64_t i;

            if (pos + 3 > 15) return vfl_exit_internal(v, run);
            iw = vfl_get16(buf + pos);
            level = buf[pos + 2] & 31;
            n = pos + 3;
            if (vfl_push(vm, v, run, vfl_read_reg(v, 5, rex, osz), osz)) return 1;
            frame = vfl_sp(v);
            if (level > 0) {
                for (i = 1; i < level; i++) {
                    uint64_t t = 0;

                    rr = vfl_mem_read(vm, v, vfl_seg_base(v, 2) + frame - i * (uint64_t)osz,
                                      (uint64_t)osz, (uint8_t *)&t);
                    if (rr) return vfl_memerr(v, run, rr);
                    if (vfl_push(vm, v, run, t & vfl_mask(osz), osz)) return 1;
                }
                if (vfl_push(vm, v, run, frame, osz)) return 1;
            }
            vfl_write_reg(v, 5, rex, osz, frame);
            vfl_set_sp(v, (vfl_sp(v) - iw) & vfl_stack_mask(v));
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xCB:
        case 0xCA:
            if (!prot) {
                uint64_t newip;
                uint64_t newcs;

                if (vfl_pop(vm, v, run, 2, &newip)) return 1;
                if (vfl_pop(vm, v, run, 2, &newcs)) return 1;
                v->sregs.cs.selector = (uint16_t)newcs;
                v->sregs.cs.base = (uint64_t)(uint16_t)newcs << 4;
                v->regs.rip = newip & 0xFFFFULL;
                return 0;
            }
            n = pos;
            vfl_adv(v, n, ripmask);
            return vfl_exit_shutdown(v, run);

        case 0xEA:
        case 0x9A:
            if (!prot) {
                uint64_t off;
                uint64_t sel;

                if (osz == 2) {
                    if (pos + 4 > 15) return vfl_exit_internal(v, run);
                    off = vfl_get16(buf + pos);
                    sel = vfl_get16(buf + pos + 2);
                    n = pos + 4;
                } else {
                    if (pos + 6 > 15) return vfl_exit_internal(v, run);
                    off = vfl_get32(buf + pos);
                    sel = vfl_get16(buf + pos + 4);
                    n = pos + 6;
                }
                if (op == 0x9A) {
                    if (vfl_push(vm, v, run, v->sregs.cs.selector, 2)) return 1;
                    if (vfl_push(vm, v, run, v->regs.rip + (uint64_t)n, 2)) return 1;
                }
                v->sregs.cs.selector = (uint16_t)sel;
                v->sregs.cs.base = sel << 4;
                v->regs.rip = off & 0xFFFFULL;
                return 0;
            }
            if (lm)
                return vfl_exit_exception(v, run, 6, 0);
            {
                uint64_t off;
                uint16_t sel;

                if (osz == 2) {
                    if (pos + 4 > 15) return vfl_exit_internal(v, run);
                    off = vfl_get16(buf + pos);
                    sel = vfl_get16(buf + pos + 2);
                    n = pos + 4;
                } else {
                    if (pos + 6 > 15) return vfl_exit_internal(v, run);
                    off = vfl_get32(buf + pos);
                    sel = vfl_get16(buf + pos + 4);
                    n = pos + 6;
                }
                if (op == 0x9A) {
                    if (vfl_push(vm, v, run, v->sregs.cs.selector, osz)) return 1;
                    if (vfl_push(vm, v, run, v->regs.rip + (uint64_t)n, osz)) return 1;
                }
                if (vfl_far_load_cs(vm, v, run, sel)) return 1;
                v->regs.rip = off & ripmask;
                return 0;
            }

        case 0xC4:
        case 0xC5:
            if (lm || prot) {
                if (op == 0xC4) {
                    uint8_t b1;

                    if (pos >= 15) return vfl_exit_internal(v, run);
                    b1 = buf[pos];
                    if (((b1 >> 5) & 3) == 3) {
                        int mm = b1 & 31;

                        if (mm != 1 && mm != 2 && mm != 3)
                            return vfl_exit_exception(v, run, 6, 0);
                        return vfl_vex_step(vm, v, run, buf, &pos, asz, seg, lm,
                                            ripmask, 1);
                    }
                    if (lm) return vfl_exit_exception(v, run, 6, 0);
                } else {
                    return vfl_vex_step(vm, v, run, buf, &pos, asz, seg, lm,
                                        ripmask, 0);
                }
            }
            if (pos >= 15) return vfl_exit_internal(v, run);
            m = buf[pos++];
            mod = (m >> 6) & 3;
            reg = ((m >> 3) & 7) | (rex_r << 3);
            rm = (m & 7) | (rex_b << 3);
            if (mod == 3) return vfl_exit_exception(v, run, 6, 0);
            if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                return vfl_exit_internal(v, run);
            n = pos;
            sz = prot ? 4 : 2;
            {
                vfl_segment_t *s;
                uint64_t off = 0;
                uint64_t sel = 0;
                uint8_t tb[8];

                vfl_seg_ptr(v, op == 0xC4 ? 0 : 3, &s);
                rr = vfl_mem_read(vm, v, ea.addr, (uint64_t)(sz + 2), tb);
                if (rr) return vfl_memerr(v, run, rr);
                if (sz == 2) {
                    off = vfl_get16(tb);
                    sel = vfl_get16(tb + 2);
                } else {
                    off = vfl_get32(tb);
                    sel = vfl_get16(tb + 4);
                }
                vfl_write_reg(v, reg, rex, osz, off);
                s->selector = (uint16_t)sel;
                if (!prot) s->base = sel << 4;
            }
            vfl_adv(v, n, ripmask);
            return 0;

        case 0xFF:
            if (pos >= 15) return vfl_exit_internal(v, run);
            m = buf[pos++];
            mod = (m >> 6) & 3;
            reg = (m >> 3) & 7;
            rm = (m & 7) | (rex_b << 3);
            if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                return vfl_exit_internal(v, run);
            n = pos;
            if (!ea.is_reg && ea.rip_rel)
                ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
            switch (reg) {
                case 0:
                    a = 0;
                    rr = vfl_ea_read(vm, v, &ea, osz, rex, &a);
                    if (rr == -2) return vfl_mem_fault(v, run);
                    if (rr == 1 && !ea.is_reg)
                        return vfl_mmio_or_fault(vm, v, run, ea.addr, osz, VFL_P_MMIO_RMW,
                                                 0, 0, 0, VFL_RMW_INC, n, ripmask);
                    if (rr != 0) {
                        if (ea.is_reg) return vfl_exit_internal(v, run);
                        return vfl_memerr(v, run, rr);
                    }
                    r = vfl_do_alu(v, VFL_RMW_INC, a & vfl_mask(osz), 0, osz);
                    return vfl_exec_store(vm, v, run, &ea, r, osz, rex, n, ripmask);
                case 1:
                    a = 0;
                    rr = vfl_ea_read(vm, v, &ea, osz, rex, &a);
                    if (rr == -2) return vfl_mem_fault(v, run);
                    if (rr == 1 && !ea.is_reg)
                        return vfl_mmio_or_fault(vm, v, run, ea.addr, osz, VFL_P_MMIO_RMW,
                                                 0, 0, 0, VFL_RMW_DEC, n, ripmask);
                    if (rr != 0) {
                        if (ea.is_reg) return vfl_exit_internal(v, run);
                        return vfl_memerr(v, run, rr);
                    }
                    r = vfl_do_alu(v, VFL_RMW_DEC, a & vfl_mask(osz), 0, osz);
                    return vfl_exec_store(vm, v, run, &ea, r, osz, rex, n, ripmask);
                case 2: {
                    uint64_t tgt = 0;
                    uint64_t ret;

                    rr = vfl_ea_read(vm, v, &ea, osz, rex, &tgt);
                    if (rr == -2) return vfl_mem_fault(v, run);
                    if (rr == 1 && !ea.is_reg)
                        return vfl_mmio_or_fault(vm, v, run, ea.addr, osz, VFL_P_MMIO_CALL,
                                                 0, 0, 0, 0, n, ripmask);
                    if (rr != 0) {
                        if (ea.is_reg) return vfl_exit_internal(v, run);
                        return vfl_memerr(v, run, rr);
                    }
                    ret = (v->regs.rip + (uint64_t)n) & ripmask;
                    if (vfl_push(vm, v, run, ret, osz)) return 1;
                    v->regs.rip = (v->regs.rip & ~ripmask) | (tgt & ripmask);
                    return 0;
                }
                case 3:
                    if (!prot) {
                        uint64_t off;
                        uint64_t sel;

                        if (osz == 2) {
                            if (pos + 4 > 15) return vfl_exit_internal(v, run);
                            off = vfl_get16(buf + pos);
                            sel = vfl_get16(buf + pos + 2);
                            n = pos + 4;
                        } else {
                            if (pos + 6 > 15) return vfl_exit_internal(v, run);
                            off = vfl_get32(buf + pos);
                            sel = vfl_get16(buf + pos + 4);
                            n = pos + 6;
                        }
                        if (vfl_push(vm, v, run, v->sregs.cs.selector, 2)) return 1;
                        if (vfl_push(vm, v, run, v->regs.rip + (uint64_t)n, 2)) return 1;
                        v->sregs.cs.selector = (uint16_t)sel;
                        v->sregs.cs.base = sel << 4;
                        v->regs.rip = off & 0xFFFFULL;
                        return 0;
                    }
                    n = pos;
                    vfl_adv(v, n, ripmask);
                    return vfl_exit_shutdown(v, run);
                case 4: {
                    uint64_t tgt = 0;

                    rr = vfl_ea_read(vm, v, &ea, osz, rex, &tgt);
                    if (rr == -2) return vfl_mem_fault(v, run);
                    if (rr == 1 && !ea.is_reg)
                        return vfl_mmio_or_fault(vm, v, run, ea.addr, osz, VFL_P_MMIO_JMP,
                                                 0, 0, 0, 0, n, ripmask);
                    if (rr != 0) {
                        if (ea.is_reg) return vfl_exit_internal(v, run);
                        return vfl_memerr(v, run, rr);
                    }
                    v->regs.rip = (v->regs.rip & ~ripmask) | (tgt & ripmask);
                    return 0;
                }
                case 5:
                    if (!prot) {
                        uint64_t off;
                        uint64_t sel;

                        if (osz == 2) {
                            if (pos + 4 > 15) return vfl_exit_internal(v, run);
                            off = vfl_get16(buf + pos);
                            sel = vfl_get16(buf + pos + 2);
                            n = pos + 4;
                        } else {
                            if (pos + 6 > 15) return vfl_exit_internal(v, run);
                            off = vfl_get32(buf + pos);
                            sel = vfl_get16(buf + pos + 4);
                            n = pos + 6;
                        }
                        v->sregs.cs.selector = (uint16_t)sel;
                        v->sregs.cs.base = sel << 4;
                        v->regs.rip = off & 0xFFFFULL;
                        return 0;
                    }
                    n = pos;
                    vfl_adv(v, n, ripmask);
                    return vfl_exit_shutdown(v, run);
                case 6: {
                    uint64_t val = 0;

                    rr = vfl_ea_read(vm, v, &ea, osz, rex, &val);
                    if (rr == -2) return vfl_mem_fault(v, run);
                    if (rr == 1 && !ea.is_reg)
                        return vfl_mmio_or_fault(vm, v, run, ea.addr, osz, VFL_P_MMIO_PUSH,
                                                 0, 0, 0, 0, n, ripmask);
                    if (rr != 0) {
                        if (ea.is_reg) return vfl_exit_internal(v, run);
                        return vfl_memerr(v, run, rr);
                    }
                    if (vfl_push(vm, v, run, val & vfl_mask(osz), osz)) return 1;
                    vfl_adv(v, n, ripmask);
                    return 0;
                }
                default:
                    return vfl_exit_exception(v, run, 6, 0);
            }

        case 0xFE:
            if (pos >= 15) return vfl_exit_internal(v, run);
            m = buf[pos++];
            mod = (m >> 6) & 3;
            reg = (m >> 3) & 7;
            rm = (m & 7) | (rex_b << 3);
            if (reg > 1) return vfl_exit_exception(v, run, 6, 0);
            if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                return vfl_exit_internal(v, run);
            n = pos;
            if (!ea.is_reg && ea.rip_rel)
                ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
            a = 0;
            rr = vfl_ea_read(vm, v, &ea, 1, rex, &a);
            if (rr == -2) return vfl_mem_fault(v, run);
            if (rr != 0) {
                if (ea.is_reg) return vfl_exit_internal(v, run);
                return vfl_memerr(v, run, rr);
            }
            if (reg == 0) r = vfl_do_alu(v, VFL_RMW_INC, a & 0xFFULL, 0, 1);
            else r = vfl_do_alu(v, VFL_RMW_DEC, a & 0xFFULL, 0, 1);
            return vfl_exec_store(vm, v, run, &ea, r, 1, rex, n, ripmask);

        case 0xF6:
        case 0xF7: {
            uint64_t src = 0;

            sz = (op & 1) ? osz : 1;
            if (pos >= 15) return vfl_exit_internal(v, run);
            m = buf[pos++];
            mod = (m >> 6) & 3;
            reg = (m >> 3) & 7;
            rm = (m & 7) | (rex_b << 3);
            if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                return vfl_exit_internal(v, run);
            if (reg == 0 || reg == 1) {
                if (op == 0xF6) {
                    if (pos + 1 > 15) return vfl_exit_internal(v, run);
                    b = buf[pos];
                    pos += 1;
                } else {
                    if (pos + sz > 15) return vfl_exit_internal(v, run);
                    b = 0;
                    if (sz == 2) b = vfl_get16(buf + pos);
                    else if (sz == 4) b = vfl_get32(buf + pos);
                    else b = vfl_get64(buf + pos);
                    pos += sz;
                }
            }
            n = pos;
            if (!ea.is_reg && ea.rip_rel)
                ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
            rr = vfl_ea_read(vm, v, &ea, sz, rex, &src);
            if (rr == -2) return vfl_mem_fault(v, run);
            if (rr != 0) {
                if (ea.is_reg) return vfl_exit_internal(v, run);
                return vfl_memerr(v, run, rr);
            }
            src &= vfl_mask(sz);
            switch (reg) {
                case 0:
                    vfl_flag_logic(v, (src & b) & vfl_mask(sz), sz);
                    break;
                case 1:
                    vfl_flag_logic(v, (src & b) & vfl_mask(sz), sz);
                    break;
                case 2:
                    r = vfl_do_alu(v, VFL_RMW_NOT, src, 0, sz);
                    return vfl_exec_store(vm, v, run, &ea, r, sz, rex, n, ripmask);
                case 3:
                    r = vfl_do_alu(v, VFL_RMW_NEG, src, 0, sz);
                    return vfl_exec_store(vm, v, run, &ea, r, sz, rex, n, ripmask);
                case 4:
                case 5: {
                    int neg = (reg == 5);

                    if (sz == 1) {
                        uint64_t al = v->regs.rax & 0xFFULL;
                        uint16_t p;

                        if (!neg) p = (uint16_t)(al * src);
                        else p = (uint16_t)(int16_t)((int8_t)al * (int8_t)src);
                        v->regs.rax = (v->regs.rax & ~0xFFFFULL) | p;
                        if (!neg && (p & 0xFF00)) v->regs.rflags |= (1ULL << 0) | (1ULL << 11);
                        else if (neg && (int16_t)p != (int8_t)p) v->regs.rflags |= (1ULL << 0) | (1ULL << 11);
                        else v->regs.rflags &= ~((1ULL << 0) | (1ULL << 11));
                    } else {
                        uint64_t ax = vfl_read_reg(v, 0, rex, sz);
                        uint64_t lo;
                        uint64_t hi;
                        int ov = 0;

                        if (!neg) {
                            __uint128_t full = (__uint128_t)ax * src;

                            lo = (uint64_t)full & vfl_mask(sz);
                            hi = (uint64_t)(full >> (sz * 8)) & vfl_mask(sz);
                            ov = hi != 0;
                        } else {
                            __int128_t sa;
                            __int128_t sb;
                            __int128_t prod;

                            if (sz == 2) { sa = (int16_t)ax; sb = (int16_t)src; }
                            else if (sz == 4) { sa = (int32_t)ax; sb = (int32_t)src; }
                            else { sa = (int64_t)ax; sb = (int64_t)src; }
                            prod = sa * sb;
                            lo = (uint64_t)prod & vfl_mask(sz);
                            if (sz == 2) {
                                hi = (uint64_t)((prod >> 16) & 0xFFFF);
                                ov = prod != (int16_t)lo;
                            } else if (sz == 4) {
                                hi = (uint64_t)((prod >> 32) & 0xFFFFFFFFULL);
                                ov = prod != (int32_t)lo;
                            } else {
                                hi = (uint64_t)(prod >> 64);
                                ov = prod != (int64_t)lo;
                            }
                        }
                        vfl_write_reg(v, 0, rex, sz, lo);
                        vfl_write_reg(v, 2, rex, sz, hi);
                        if (ov) v->regs.rflags |= (1ULL << 0) | (1ULL << 11);
                        else v->regs.rflags &= ~((1ULL << 0) | (1ULL << 11));
                    }
                    break;
                }
                case 6:
                case 7: {
                    int neg = (reg == 7);

                    if (src == 0) return vfl_exit_exception(v, run, 0, 0);
                    if (sz == 1) {
                        uint64_t ax = v->regs.rax & 0xFFFFULL;
                        uint64_t q;
                        uint64_t w;

                        if (!neg) {
                            q = ax / src;
                            w = ax % src;
                            if (q > 0xFF) return vfl_exit_exception(v, run, 0, 0);
                        } else {
                            int16_t sax = (int16_t)ax;
                            int8_t ssrc = (int8_t)src;
                            int16_t sq = sax / ssrc;
                            int8_t sw = sax % ssrc;

                            if (sq > 127 || sq < -128)
                                return vfl_exit_exception(v, run, 0, 0);
                            q = (uint64_t)(uint8_t)sq;
                            w = (uint64_t)(uint8_t)sw;
                        }
                        v->regs.rax = (v->regs.rax & ~0xFFFFULL) | (w << 8) | q;
                    } else {
                        __uint128_t dvd;
                        uint64_t dhi = vfl_read_reg(v, 2, rex, sz);
                        uint64_t dlo = vfl_read_reg(v, 0, rex, sz);

                        dvd = ((__uint128_t)(dhi & vfl_mask(sz)) << (sz * 8)) | (dlo & vfl_mask(sz));
                        if (!neg) {
                            uint64_t q = (uint64_t)(dvd / src);
                            uint64_t w = (uint64_t)(dvd % src);

                            if ((q & ~vfl_mask(sz)) != 0)
                                return vfl_exit_exception(v, run, 0, 0);
                            vfl_write_reg(v, 0, rex, sz, q);
                            vfl_write_reg(v, 2, rex, sz, w);
                        } else {
                            __int128_t sdvd;
                            __int128_t ssrc;
                            __int128_t sq;
                            __int128_t sw;

                            if (sz == 2) {
                                sdvd = (int32_t)((uint32_t)dlo | ((uint32_t)dhi << 16));
                                ssrc = (int16_t)src;
                            } else if (sz == 4) {
                                sdvd = (int64_t)(dlo | (dhi << 32));
                                ssrc = (int32_t)src;
                            } else {
                                sdvd = (__int128_t)dvd;
                                ssrc = (int64_t)src;
                            }
                            sq = sdvd / ssrc;
                            sw = sdvd % ssrc;
                            if (sz == 2 && (sq > 32767 || sq < -32768))
                                return vfl_exit_exception(v, run, 0, 0);
                            if (sz == 4 && (sq > 2147483647LL || sq < -2147483648LL))
                                return vfl_exit_exception(v, run, 0, 0);
                            vfl_write_reg(v, 0, rex, sz, (uint64_t)sq);
                            vfl_write_reg(v, 2, rex, sz, (uint64_t)sw);
                        }
                    }
                    break;
                }
                default:
                    return vfl_exit_exception(v, run, 6, 0);
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x69:
        case 0x6B: {
            int64_t imm;

            sz = osz;
            if (pos >= 15) return vfl_exit_internal(v, run);
            m = buf[pos++];
            mod = (m >> 6) & 3;
            reg = ((m >> 3) & 7) | (rex_r << 3);
            rm = (m & 7) | (rex_b << 3);
            if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                return vfl_exit_internal(v, run);
            if (op == 0x6B) {
                if (pos + 1 > 15) return vfl_exit_internal(v, run);
                imm = (int8_t)buf[pos];
                pos += 1;
            } else {
                if (sz == 2) {
                    if (pos + 2 > 15) return vfl_exit_internal(v, run);
                    imm = (int16_t)vfl_get16(buf + pos);
                    pos += 2;
                } else {
                    if (pos + 4 > 15) return vfl_exit_internal(v, run);
                    imm = (int32_t)vfl_get32(buf + pos);
                    pos += 4;
                }
            }
            n = pos;
            if (!ea.is_reg && ea.rip_rel)
                ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
            rr = vfl_ea_read(vm, v, &ea, sz, rex, &a);
            if (rr == -2) return vfl_mem_fault(v, run);
            if (rr != 0) {
                if (ea.is_reg) return vfl_exit_internal(v, run);
                return vfl_memerr(v, run, rr);
            }
            {
                int64_t sa;
                __int128_t prod;
                int64_t slow;

                if (sz == 2) sa = (int16_t)(a & 0xFFFFULL);
                else if (sz == 4) sa = (int32_t)(a & 0xFFFFFFFFULL);
                else sa = (int64_t)a;
                prod = (__int128_t)sa * (__int128_t)imm;
                vfl_write_reg(v, reg, rex, sz, (uint64_t)prod);
                if (sz == 2) slow = (int16_t)(uint16_t)prod;
                else if (sz == 4) slow = (int32_t)(uint32_t)prod;
                else slow = (int64_t)(uint64_t)prod;
                if (prod != slow) v->regs.rflags |= (1ULL << 0) | (1ULL << 11);
                else v->regs.rflags &= ~((1ULL << 0) | (1ULL << 11));
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xC0:
        case 0xC1:
        case 0xD0:
        case 0xD1:
        case 0xD2:
        case 0xD3: {
            unsigned count;

            sz = (op & 1) ? osz : 1;
            if (pos >= 15) return vfl_exit_internal(v, run);
            m = buf[pos++];
            mod = (m >> 6) & 3;
            reg = (m >> 3) & 7;
            rm = (m & 7) | (rex_b << 3);
            if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                return vfl_exit_internal(v, run);
            if (op == 0xC0 || op == 0xC1) {
                if (pos + 1 > 15) return vfl_exit_internal(v, run);
                count = buf[pos++];
            } else if (op == 0xD2 || op == 0xD3) {
                count = (unsigned)(v->regs.rcx & 0xFFULL);
            } else {
                count = 1;
            }
            n = pos;
            if (!ea.is_reg && ea.rip_rel)
                ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
            if (reg > 7) return vfl_exit_exception(v, run, 6, 0);
            a = 0;
            rr = vfl_ea_read(vm, v, &ea, sz, rex, &a);
            if (rr == -2) return vfl_mem_fault(v, run);
            if (rr != 0) {
                if (ea.is_reg) return vfl_exit_internal(v, run);
                return vfl_memerr(v, run, rr);
            }
            r = vfl_do_shift(v, reg, a & vfl_mask(sz), count, sz);
            return vfl_exec_store(vm, v, run, &ea, r, sz, rex, n, ripmask);
        }

        case 0xE4:
        case 0xE5:
        case 0xE6:
        case 0xE7:
        case 0xEC:
        case 0xED:
        case 0xEE:
        case 0xEF: {
            uint16_t port;
            int is_out = op == 0xE6 || op == 0xE7 || op == 0xEE || op == 0xEF;

            sz = osz;
            if (op == 0xE4 || op == 0xE6 || op == 0xEC || op == 0xEE) sz = 1;
            else if (sz != 2) sz = 4;
            if (op == 0xE4 || op == 0xE5 || op == 0xE6 || op == 0xE7) {
                if (pos + 1 > 15) return vfl_exit_internal(v, run);
                port = buf[pos++];
            } else {
                port = (uint16_t)(v->regs.rdx & 0xFFFFULL);
            }
            n = pos;
            if (is_out) {
                a = vfl_read_reg(v, 0, rex, sz);
                vfl_adv(v, n, ripmask);
                return vfl_exit_io_out(v, run, port, sz, (uint32_t)(a & 0xFFFFFFFFULL));
            }
            return vfl_exit_io_in(v, run, port, sz, VFL_P_IO_IN_REG, 0, rex, 0, n, ripmask);
        }

        case 0x6C:
        case 0x6D:
        case 0x6E:
        case 0x6F: {
            uint16_t port = (uint16_t)(v->regs.rdx & 0xFFFFULL);
            uint64_t cnt = vfl_get_count(v, asz);
            uint64_t si = vfl_get_si(v, asz);
            uint64_t di = vfl_get_di(v, asz);
            int df = (v->regs.rflags >> 10) & 1;
            int s = (op & 1) ? osz : 1;
            int is_out = (op == 0x6E || op == 0x6F);
            uint64_t delta = df ? (uint64_t)(-(int64_t)s) : (uint64_t)s;

            n = pos;
            if (rep && cnt == 0) {
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (is_out) {
                uint64_t src = (seg >= 0 ? vfl_seg_base(v, seg) : vfl_seg_base(v, 3)) + si;
                uint64_t val = 0;

                rr = vfl_mem_read(vm, v, src, (uint64_t)s, (uint8_t *)&val);
                if (rr) return vfl_memerr(v, run, rr);
                vfl_set_si(v, asz, (si + delta) & vfl_mask(asz));
                if (rep) vfl_set_count(v, asz, (cnt - 1) & vfl_mask(asz));
                return vfl_exit_io_out(v, run, port, s, (uint32_t)(val & 0xFFFFFFFFULL));
            }
            {
                uint64_t dst = vfl_seg_base(v, 0) + di;
                uint64_t phys;

                if (vfl_translate(vm, v, dst, VFL_MMU_WRITE, &phys))
                    return vfl_mem_fault(v, run);
                if (!vfl_find_slot(vm, phys, (uint64_t)s))
                    return vfl_exit_exception(v, run, 14, 0);
                vfl_set_di(v, asz, (di + delta) & vfl_mask(asz));
                if (rep) vfl_set_count(v, asz, (cnt - 1) & vfl_mask(asz));
                return vfl_exit_io_in(v, run, port, s, VFL_P_IO_IN_MEM, 0, 0, dst,
                                      rep ? 0 : n, ripmask);
            }
        }


        case 0xA4:
        case 0xA5:
        case 0xA6:
        case 0xA7:
        case 0xAA:
        case 0xAB:
        case 0xAC:
        case 0xAD:
        case 0xAE:
        case 0xAF: {
            uint64_t cnt = vfl_get_count(v, asz);
            uint64_t si = vfl_get_si(v, asz);
            uint64_t di = vfl_get_di(v, asz);
            int df = (v->regs.rflags >> 10) & 1;
            int s = (op & 1) ? osz : 1;
            int64_t step = df ? -(int64_t)s : (int64_t)s;
            int sseg = seg >= 0 ? seg : 3;
            int repz = (rep == 3);
            int repnz = (rep == 2);

            n = pos;
            if (rep && cnt == 0) {
                vfl_adv(v, n, ripmask);
                return 0;
            }
            if (!vfl_string_chunk(vm, v, op, rep, asz, osz, seg, rex, n, ripmask))
                return 0;
            switch (op) {
                case 0xA4:
                case 0xA5: {
                    uint64_t src = vfl_seg_base(v, sseg) + si;
                    uint64_t dst = vfl_seg_base(v, 0) + di;
                    uint64_t val = 0;
                    uint8_t wb[8];
                    int w;

                    rr = vfl_mem_read(vm, v, src, (uint64_t)s, (uint8_t *)&val);
                    if (rr == -2) return vfl_mem_fault(v, run);
                    if (rr == 1) {
                        v->pending = VFL_P_MMIO_STR;
                        v->pending_op = 2;
                        v->pending_addr = dst;
                        v->pending_src = src;
                        v->pending_size = s;
                        v->pending_count = rep ? cnt : 1;
                        v->pending_port = (uint16_t)asz;
                        v->pending_adv = (uint64_t)n;
                        v->pending_mask = ripmask;
                        return vfl_str_emit(vm, v, run);
                    }
                    if (rr < 0) return vfl_exit_exception(v, run, 13, 0);
                    vfl_put64(wb, val & vfl_mask(s), s);
                    w = vfl_mem_write(vm, v, dst, (uint64_t)s, wb);
                    if (w == -2) return vfl_mem_fault(v, run);
                    if (w == 1) {
                        v->pending = VFL_P_MMIO_STR;
                        v->pending_op = 1;
                        v->pending_addr = dst;
                        v->pending_src = src;
                        v->pending_size = s;
                        v->pending_count = rep ? cnt : 1;
                        v->pending_port = (uint16_t)asz;
                        v->pending_adv = (uint64_t)n;
                        v->pending_mask = ripmask;
                        return vfl_str_emit(vm, v, run);
                    }
                    if (w < 0) return vfl_exit_exception(v, run, 13, 0);
                    vfl_set_si(v, asz, (si + (uint64_t)step) & vfl_mask(asz));
                    vfl_set_di(v, asz, (di + (uint64_t)step) & vfl_mask(asz));
                    break;
                }
                case 0xA6:
                case 0xA7: {
                    uint64_t src = vfl_seg_base(v, sseg) + si;
                    uint64_t dst = vfl_seg_base(v, 0) + di;
                    uint64_t sv = 0;
                    uint64_t dv = 0;

                    rr = vfl_mem_read(vm, v, src, (uint64_t)s, (uint8_t *)&sv);
                    if (rr) return vfl_memerr(v, run, rr);
                    rr = vfl_mem_read(vm, v, dst, (uint64_t)s, (uint8_t *)&dv);
                    if (rr) return vfl_memerr(v, run, rr);
                    vfl_do_alu(v, VFL_RMW_CMP, sv & vfl_mask(s), dv & vfl_mask(s), s);
                    vfl_set_si(v, asz, (si + (uint64_t)step) & vfl_mask(asz));
                    vfl_set_di(v, asz, (di + (uint64_t)step) & vfl_mask(asz));
                    break;
                }
                case 0xAA:
                case 0xAB: {
                    uint64_t dst = vfl_seg_base(v, 0) + di;
                    uint8_t wb[8];
                    int w;

                    a = vfl_read_reg(v, 0, rex, s);
                    vfl_put64(wb, a & vfl_mask(s), s);
                    w = vfl_mem_write(vm, v, dst, (uint64_t)s, wb);
                    if (w == -2) return vfl_mem_fault(v, run);
                    if (w == 1) {
                        v->pending = VFL_P_MMIO_STR;
                        v->pending_op = 0;
                        v->pending_addr = dst;
                        v->pending_src = a & vfl_mask(s);
                        v->pending_size = s;
                        v->pending_count = rep ? cnt : 1;
                        v->pending_port = (uint16_t)asz;
                        v->pending_adv = (uint64_t)n;
                        v->pending_mask = ripmask;
                        return vfl_str_emit(vm, v, run);
                    }
                    if (w < 0) return vfl_exit_exception(v, run, 13, 0);
                    vfl_set_di(v, asz, (di + (uint64_t)step) & vfl_mask(asz));
                    break;
                }
                case 0xAC:
                case 0xAD: {
                    uint64_t src = vfl_seg_base(v, sseg) + si;
                    uint64_t val = 0;

                    rr = vfl_mem_read(vm, v, src, (uint64_t)s, (uint8_t *)&val);
                    if (rr == -2) return vfl_mem_fault(v, run);
                    if (rr == 1)
                        return vfl_mmio_or_fault(vm, v, run, src, s, VFL_P_MMIO_REG,
                                                 0, rex, 0, 0, n, ripmask);
                    if (rr < 0)
                        return vfl_exit_exception(v, run, 13, 0);
                    vfl_write_reg(v, 0, rex, s, val & vfl_mask(s));
                    vfl_set_si(v, asz, (si + (uint64_t)step) & vfl_mask(asz));
                    break;
                }
                default: {
                    uint64_t dst = vfl_seg_base(v, 0) + di;
                    uint64_t dv = 0;

                    rr = vfl_mem_read(vm, v, dst, (uint64_t)s, (uint8_t *)&dv);
                    if (rr) return vfl_memerr(v, run, rr);
                    a = vfl_read_reg(v, 0, rex, s);
                    vfl_do_alu(v, VFL_RMW_CMP, a & vfl_mask(s), dv & vfl_mask(s), s);
                    vfl_set_di(v, asz, (di + (uint64_t)step) & vfl_mask(asz));
                    break;
                }
            }
            if (rep) {
                int zf = (v->regs.rflags >> 6) & 1;
                uint64_t left = (cnt - 1) & vfl_mask(asz);

                vfl_set_count(v, asz, left);
                if (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF) {
                    if ((repz && !zf) || (repnz && zf) || left == 0) {
                        vfl_adv(v, n, ripmask);
                        return 0;
                    }
                    return 0;
                }
                if (left == 0) {
                    vfl_adv(v, n, ripmask);
                    return 0;
                }
                return 0;
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xD7: {
            uint64_t base;
            uint64_t addr;
            uint8_t val = 0;
            int s = seg >= 0 ? seg : 3;

            if (asz == 2) base = (uint64_t)(uint16_t)v->regs.rbx;
            else if (asz == 4) base = (uint64_t)(uint32_t)v->regs.rbx;
            else base = v->regs.rbx;
            addr = vfl_seg_base(v, s) + base + (v->regs.rax & 0xFFULL);
            n = pos;
            rr = vfl_mem_read(vm, v, addr, 1, &val);
            if (rr == -2) return vfl_mem_fault(v, run);
            if (rr == 1)
                return vfl_mmio_or_fault(vm, v, run, addr, 1, VFL_P_MMIO_REG,
                                         0, rex, 0, 0, n, ripmask);
            if (rr < 0)
                return vfl_exit_exception(v, run, 13, 0);
            v->regs.rax = (v->regs.rax & ~0xFFULL) | val;
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xD6: {
            uint64_t cf = (v->regs.rflags >> 0) & 1ULL;

            v->regs.rax = (v->regs.rax & ~0xFFULL) | (cf ? 0xFFULL : 0);
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0xCC:
            n = pos;
            vfl_adv(v, n, ripmask);
            return vfl_exit_exception(v, run, 3, 0);

        case 0xCE:
            n = pos;
            vfl_adv(v, n, ripmask);
            if ((v->regs.rflags >> 11) & 1)
                return vfl_exit_exception(v, run, 4, 0);
            return 0;

        case 0xCD:
            if (pos + 1 > 15) return vfl_exit_internal(v, run);
            n = pos + 1;
            vfl_adv(v, n, ripmask);
            if (buf[pos] == 0xFF) {
                run->exit_reason = VFL_EXIT_PARAVIRT;
                run->paravirt.nr = v->regs.rax;
                run->paravirt.args[0] = v->regs.rbx;
                run->paravirt.args[1] = v->regs.rcx;
                run->paravirt.args[2] = v->regs.rdx;
                run->paravirt.args[3] = v->regs.rsi;
                v->last_exit = *run;
                return 1;
            }
            return vfl_exit_exception(v, run, buf[pos], 0);

        case 0xCF: {
            uint64_t newip;
            uint64_t newcs;
            uint64_t newfl;
            int w = (osz == 8) ? 8 : 4;

            if (osz == 2) w = 2;
            if (vfl_pop(vm, v, run, w, &newip)) return 1;
            if (vfl_pop(vm, v, run, w, &newcs)) return 1;
            if (vfl_pop(vm, v, run, w, &newfl)) return 1;
            v->sregs.cs.selector = (uint16_t)(newcs & 0xFFFFULL);
            if (!(v->sregs.cr0 & 1ULL))
                v->sregs.cs.base = (uint64_t)v->sregs.cs.selector << 4;
            v->regs.rflags = (newfl & vfl_mask(w)) | 0x2ULL;
            v->regs.rip = (v->regs.rip & ~ripmask) | (newip & ripmask);
            return 0;
        }

        case 0xF1:
            n = pos;
            vfl_adv(v, n, ripmask);
            return vfl_exit_exception(v, run, 1, 0);

        case 0xD4:
        case 0xD5: {
            uint64_t base;

            if (pos + 1 > 15) return vfl_exit_internal(v, run);
            base = buf[pos++];
            n = pos;
            if (op == 0xD4) {
                uint64_t al = v->regs.rax & 0xFFULL;

                if (base == 0) return vfl_exit_exception(v, run, 0, 0);
                v->regs.rax = (v->regs.rax & ~0xFFFFULL) |
                    ((al / base) << 8) | (al % base);
                vfl_flag_logic(v, v->regs.rax & 0xFFULL, 1);
            } else {
                uint64_t al = v->regs.rax & 0xFFULL;
                uint64_t ah = (v->regs.rax >> 8) & 0xFFULL;

                v->regs.rax = (v->regs.rax & ~0xFFFFULL) | ((ah * base + al) & 0xFFULL);
                vfl_flag_logic(v, v->regs.rax & 0xFFULL, 1);
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x27:
        case 0x2F:
        case 0x37:
        case 0x3F: {
            uint64_t al = v->regs.rax & 0xFFULL;
            uint64_t af = (v->regs.rflags >> 4) & 1;
            uint64_t oldcf = (v->regs.rflags >> 0) & 1;
            uint64_t newcf = 0;
            uint64_t newaf = 0;

            n = pos;
            if (op == 0x27) {
                if ((al & 0x0F) > 9 || af) { al += 6; newaf = 1; }
                if (al > 0x9F || oldcf) { al += 0x60; newcf = 1; }
            } else if (op == 0x2F) {
                if ((al & 0x0F) > 9 || af) { al -= 6; newaf = 1; }
                if (al > 0x99 || oldcf) { al -= 0x60; newcf = 1; }
            } else if (op == 0x37) {
                if ((al & 0x0F) > 9 || af) { al += 6; newaf = 1; newcf = 1; }
                if (al > 0x9F || oldcf) { al += 0x60; newcf = 1; }
            } else {
                if ((al & 0x0F) > 9 || af) { al -= 6; newaf = 1; newcf = 1; }
                if (al > 0x9F || oldcf) { al -= 0x60; newcf = 1; }
            }
            v->regs.rax = (v->regs.rax & ~0xFFULL) | (al & 0xFFULL);
            vfl_flag_logic(v, al & 0xFFULL, 1);
            if (newcf) v->regs.rflags |= (1ULL << 0);
            if (newaf) v->regs.rflags |= (1ULL << 4);
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x62: {
            uint8_t tb[8];

            if (pos >= 15) return vfl_exit_internal(v, run);
            m = buf[pos++];
            mod = (m >> 6) & 3;
            reg = ((m >> 3) & 7) | (rex_r << 3);
            rm = (m & 7) | (rex_b << 3);
            if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                return vfl_exit_internal(v, run);
            n = pos;
            if (!ea.is_reg && ea.rip_rel)
                ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
            {
                int64_t idx;
                int64_t lo;
                int64_t hi;

                sz = osz == 2 ? 2 : 4;
                a = vfl_read_reg(v, reg, rex, sz);
                idx = (sz == 2) ? (int16_t)(a & 0xFFFFULL) : (int32_t)(a & 0xFFFFFFFFULL);
                if (ea.is_reg) return vfl_exit_exception(v, run, 6, 0);
                rr = vfl_mem_read(vm, v, ea.addr, (uint64_t)(sz * 2), tb);
                if (rr) return vfl_memerr(v, run, rr);
                if (sz == 2) {
                    lo = (int16_t)vfl_get16(tb);
                    hi = (int16_t)vfl_get16(tb + 2);
                } else {
                    lo = (int32_t)vfl_get32(tb);
                    hi = (int32_t)vfl_get32(tb + 4);
                }
                if (idx < lo || idx > hi) return vfl_exit_exception(v, run, 5, 0);
            }
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x63:
            if (lm) {
                if (pos >= 15) return vfl_exit_internal(v, run);
                m = buf[pos++];
                mod = (m >> 6) & 3;
                reg = ((m >> 3) & 7) | (rex_r << 3);
                rm = (m & 7) | (rex_b << 3);
                if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                    return vfl_exit_internal(v, run);
                n = pos;
                if (!ea.is_reg && ea.rip_rel)
                    ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
                rr = vfl_ea_read(vm, v, &ea, 4, rex, &a);
                if (rr == -2) return vfl_mem_fault(v, run);
                if (rr == 1 && !ea.is_reg)
                    return vfl_mmio_or_fault(vm, v, run, ea.addr, 4, VFL_P_MMIO_REG,
                                             reg, rex, 0, 0, n, ripmask);
                if (rr != 0) return vfl_exit_internal(v, run);
                *vfl_reg64(v, reg) = (uint64_t)(int64_t)(int32_t)(a & 0xFFFFFFFFULL);
                vfl_adv(v, n, ripmask);
                return 0;
            }
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0xD8:
        case 0xD9:
        case 0xDA:
        case 0xDB:
        case 0xDC:
        case 0xDD:
        case 0xDE:
        case 0xDF:
            return vfl_x87_step(vm, v, run, buf, &pos, osz, asz, seg, rex, rex_r, rex_b,
                                lm, ripmask, op);

        case 0x9B:
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;

        case 0x06:
        case 0x0E:
        case 0x16:
        case 0x1E: {
            vfl_segment_t *s;

            if (op == 0x06) vfl_seg_ptr(v, 0, &s);
            else if (op == 0x0E) vfl_seg_ptr(v, 1, &s);
            else if (op == 0x16) vfl_seg_ptr(v, 2, &s);
            else vfl_seg_ptr(v, 3, &s);
            if (vfl_push(vm, v, run, s->selector, osz)) return 1;
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x07:
        case 0x17:
        case 0x1F: {
            vfl_segment_t *s;

            if (op == 0x07) vfl_seg_ptr(v, 0, &s);
            else if (op == 0x17) vfl_seg_ptr(v, 2, &s);
            else vfl_seg_ptr(v, 3, &s);
            if (vfl_pop(vm, v, run, osz, &a)) return 1;
            s->selector = (uint16_t)(a & 0xFFFFULL);
            if (!(v->sregs.cr0 & 1ULL)) s->base = (uint64_t)s->selector << 4;
            if (op == 0x17) v->int_shadow = 1;
            n = pos;
            vfl_adv(v, n, ripmask);
            return 0;
        }

        case 0x0F: {
            uint8_t op2;

            sr = vfl_simd_escape(vm, v, run, buf, &pos, osz, asz, seg, rep, p66, rex,
                                 rex_w, rex_r, rex_b, rex_x, lm, ripmask);
            if (sr != VFL_SIMD_NOTME) return sr;
            if (pos >= 15) return vfl_exit_internal(v, run);
            op2 = buf[pos++];
            switch (op2) {
                case 0x06:
                    v->sregs.cr0 &= ~8ULL;
                    n = pos;
                    vfl_adv(v, n, ripmask);
                    return 0;

                case 0x08:
                    n = pos;
                    vfl_adv(v, n, ripmask);
                    return 0;

                case 0x09:
                    n = pos;
                    vfl_adv(v, n, ripmask);
                    return vfl_exit_shutdown(v, run);

                case 0x0B:
                    return vfl_exit_exception(v, run, 6, 0);

                case 0x20:
                case 0x21:
                case 0x22:
                case 0x23: {
                    int cr;
                    int gpr;
                    int csz;

                    if (pos >= 15) return vfl_exit_internal(v, run);
                    m = buf[pos++];
                    if (((m >> 6) & 3) != 3)
                        return vfl_exit_exception(v, run, 6, 0);
                    if ((v->sregs.cs.selector & 3) != 0)
                        return vfl_exit_exception(v, run, 13, 0);
                    cr = ((m >> 3) & 7) | (rex_r << 3);
                    gpr = (m & 7) | (rex_b << 3);
                    csz = lm ? 8 : 4;
                    n = pos;
                    if (op2 == 0x21 || op2 == 0x23)
                        return vfl_exit_exception(v, run, 6, 0);
                    if (op2 == 0x20) {
                        uint64_t cv;

                        if (cr == 1 || cr == 5 || cr == 6 || cr == 7)
                            return vfl_exit_exception(v, run, 6, 0);
                        if (vfl_mov_from_cr(vm, v, cr, &cv) < 0)
                            return vfl_exit_exception(v, run, 13, 0);
                        vfl_write_reg(v, gpr, rex, csz, cv);
                    } else {
                        uint64_t cv = vfl_read_reg(v, gpr, rex, csz);

                        if (cr == 1 || cr == 5 || cr == 6 || cr == 7)
                            return vfl_exit_exception(v, run, 6, 0);
                        if (vfl_mov_to_cr(vm, v, cr, cv) < 0)
                            return vfl_exit_exception(v, run, 13, 0);
                    }
                    vfl_adv(v, n, ripmask);
                    return 0;
                }

                case 0x30: {
                    uint64_t val;

                    val = ((uint64_t)(v->regs.rax & 0xFFFFFFFFULL)) |
                          ((uint64_t)(v->regs.rdx & 0xFFFFFFFFULL) << 32);
                    if (vfl_msr_write(vm, v, (uint32_t)(v->regs.rcx & 0xFFFFFFFFULL), val) < 0)
                        return vfl_exit_exception(v, run, 13, 0);
                    vm->tlb_epoch++;
                    n = pos;
                    vfl_adv(v, n, ripmask);
                    return 0;
                }

                case 0x31: {
                    uint64_t eff = v->tsc + v->tsc_offset;

                    v->tsc += 1000;
                    v->regs.rax = (v->regs.rax & ~0xFFFFFFFFULL) | (eff & 0xFFFFFFFFULL);
                    v->regs.rdx = (v->regs.rdx & ~0xFFFFFFFFULL) | ((eff >> 32) & 0xFFFFFFFFULL);
                    n = pos;
                    vfl_adv(v, n, ripmask);
                    return 0;
                }

                case 0x33:
                    return vfl_exit_exception(v, run, 13, 0);

                case 0x32: {
                    uint64_t val = 0;

                    if (vfl_msr_read(v, (uint32_t)(v->regs.rcx & 0xFFFFFFFFULL), &val) < 0)
                        return vfl_exit_exception(v, run, 13, 0);
                    v->regs.rax = (v->regs.rax & ~0xFFFFFFFFULL) | (val & 0xFFFFFFFFULL);
                    v->regs.rdx = (v->regs.rdx & ~0xFFFFFFFFULL) | ((val >> 32) & 0xFFFFFFFFULL);
                    n = pos;
                    vfl_adv(v, n, ripmask);
                    return 0;
                }

                case 0x77:
                    n = pos;
                    vfl_adv(v, n, ripmask);
                    return 0;

                case 0xA2: {
                    uint32_t r0, r1, r2, r3;

                    a = v->regs.rax & 0xFFFFFFFFULL;
                    b = v->regs.rcx & 0xFFFFFFFFULL;
                    n = pos;
                    vfl_cpuid(v, (uint32_t)a, (uint32_t)b, &r0, &r1, &r2, &r3);
                    v->regs.rax = (v->regs.rax & ~0xFFFFFFFFULL) | r0;
                    v->regs.rbx = (v->regs.rbx & ~0xFFFFFFFFULL) | r1;
                    v->regs.rcx = (v->regs.rcx & ~0xFFFFFFFFULL) | r2;
                    v->regs.rdx = (v->regs.rdx & ~0xFFFFFFFFULL) | r3;
                    vfl_adv(v, n, ripmask);
                    return 0;
                }

                case 0xA0:
                case 0xA1:
                case 0xA8:
                case 0xA9: {
                    vfl_segment_t *s;
                    int sidx = op2 == 0xA0 ? 4 : (op2 == 0xA1 ? 5 : (op2 == 0xA8 ? 4 : 5));

                    vfl_seg_ptr(v, sidx, &s);
                    n = pos;
                    if (op2 == 0xA0 || op2 == 0xA1) {
                        if (vfl_push(vm, v, run, s->selector, osz)) return 1;
                    } else {
                        if (vfl_pop(vm, v, run, osz, &a)) return 1;
                        s->selector = (uint16_t)(a & 0xFFFFULL);
                        if (!(v->sregs.cr0 & 1ULL))
                            s->base = (uint64_t)s->selector << 4;
                    }
                    vfl_adv(v, n, ripmask);
                    return 0;
                }

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
                case 0x8F: {
                    int64_t rel;

                    if (osz == 2) {
                        if (pos + 2 > 15) return vfl_exit_internal(v, run);
                        rel = (int16_t)vfl_get16(buf + pos);
                        n = pos + 2;
                    } else {
                        if (pos + 4 > 15) return vfl_exit_internal(v, run);
                        rel = (int32_t)vfl_get32(buf + pos);
                        n = pos + 4;
                    }
                    if (vfl_cond(v, op2 & 15))
                        v->regs.rip = (v->regs.rip & ~ripmask) |
                            ((v->regs.rip + (uint64_t)n + (uint64_t)rel) & ripmask);
                    else
                        vfl_adv(v, n, ripmask);
                    return 0;
                }

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
                    if (pos >= 15) return vfl_exit_internal(v, run);
                    m = buf[pos++];
                    mod = (m >> 6) & 3;
                    rm = (m & 7) | (rex_b << 3);
                    if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                        return vfl_exit_internal(v, run);
                    n = pos;
                    if (!ea.is_reg && ea.rip_rel)
                        ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
                    b = vfl_cond(v, op2 & 15) ? 1 : 0;
                    return vfl_exec_store(vm, v, run, &ea, b, 1, rex, n, ripmask);

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
                    if (pos >= 15) return vfl_exit_internal(v, run);
                    m = buf[pos++];
                    mod = (m >> 6) & 3;
                    reg = ((m >> 3) & 7) | (rex_r << 3);
                    rm = (m & 7) | (rex_b << 3);
                    if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                        return vfl_exit_internal(v, run);
                    n = pos;
                    if (!ea.is_reg && ea.rip_rel)
                        ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
                    if (mod != 3) {
                        a = 0;
                        rr = vfl_mem_read(vm, v, ea.addr, (uint64_t)osz, (uint8_t *)&a);
                        if (rr) return vfl_memerr(v, run, rr);
                        a &= vfl_mask(osz);
                    } else {
                        a = vfl_read_reg(v, rm, rex, osz);
                    }
                    if (vfl_cond(v, op2 & 15))
                        vfl_write_reg(v, reg, rex, osz, a);
                    vfl_adv(v, n, ripmask);
                    return 0;

                case 0xA4:
                case 0xA5:
                case 0xAC:
                case 0xAD: {
                    int sh = (op2 == 0xA4 || op2 == 0xA5);
                    int immform = (op2 == 0xA4 || op2 == 0xAC);
                    uint64_t cnt;
                    uint64_t srcreg;
                    uint64_t dstold;

                    if (pos >= 15) return vfl_exit_internal(v, run);
                    m = buf[pos++];
                    mod = (m >> 6) & 3;
                    reg = ((m >> 3) & 7) | (rex_r << 3);
                    rm = (m & 7) | (rex_b << 3);
                    if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                        return vfl_exit_internal(v, run);
                    if (immform) {
                        if (pos + 1 > 15) return vfl_exit_internal(v, run);
                        cnt = buf[pos++];
                    } else {
                        cnt = (unsigned)(v->regs.rcx & 0xFFULL);
                    }
                    sz = osz;
                    if (sz != 8) cnt &= 31;
                    else cnt &= 63;
                    n = pos;
                    if (!ea.is_reg && ea.rip_rel)
                        ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
                    srcreg = vfl_read_reg(v, reg, rex, sz) & vfl_mask(sz);
                    dstold = 0;
                    rr = vfl_ea_read(vm, v, &ea, sz, rex, &dstold);
                    if (rr == -2) return vfl_mem_fault(v, run);
                    if (rr != 0) {
                        if (ea.is_reg) return vfl_exit_internal(v, run);
                        return vfl_memerr(v, run, rr);
                    }
                    dstold &= vfl_mask(sz);
                    if (cnt == 0) {
                        vfl_adv(v, n, ripmask);
                        return 0;
                    }
                    if ((int64_t)cnt >= (int64_t)(sz * 8)) {
                        r = srcreg;
                        v->regs.rflags &= ~(1ULL << 0);
                    } else if (sh) {
                        r = ((dstold << cnt) | (srcreg >> (sz * 8 - cnt))) & vfl_mask(sz);
                        v->regs.rflags = (v->regs.rflags & ~(1ULL << 0)) |
                            (((dstold >> (sz * 8 - cnt)) & 1ULL) << 0);
                    } else {
                        r = ((dstold >> cnt) | (srcreg << (sz * 8 - cnt))) & vfl_mask(sz);
                        v->regs.rflags = (v->regs.rflags & ~(1ULL << 0)) |
                            (((dstold >> (cnt - 1)) & 1ULL) << 0);
                    }
                    {
                        uint64_t cf = (v->regs.rflags >> 0) & 1ULL;

                        vfl_flag_logic(v, r, sz);
                        v->regs.rflags |= (cf << 0);
                        if (cnt == 1) {
                            uint64_t msb = (r >> (sz * 8 - 1)) & 1ULL;

                            if (sh) {
                                if (msb ^ cf) v->regs.rflags |= (1ULL << 11);
                            } else {
                                uint64_t sa = (dstold >> (sz * 8 - 1)) & 1ULL;

                                if (sa ^ msb) v->regs.rflags |= (1ULL << 11);
                                else v->regs.rflags &= ~(1ULL << 11);
                            }
                        }
                    }
                    return vfl_exec_store(vm, v, run, &ea, r, sz, rex, n, ripmask);
                }

                case 0xA3:
                case 0xAB:
                case 0xB3:
                case 0xBB: {
                    uint64_t bit;

                    if (pos >= 15) return vfl_exit_internal(v, run);
                    m = buf[pos++];
                    mod = (m >> 6) & 3;
                    reg = ((m >> 3) & 7) | (rex_r << 3);
                    rm = (m & 7) | (rex_b << 3);
                    if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                        return vfl_exit_internal(v, run);
                    n = pos;
                    if (!ea.is_reg && ea.rip_rel)
                        ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
                    bit = vfl_read_reg(v, reg, rex, osz);
                    sz = osz;
                    if (ea.is_reg) {
                        a = vfl_read_reg(v, ea.reg, rex, sz);
                        if ((a >> (bit & (uint64_t)(sz * 8 - 1))) & 1ULL)
                            v->regs.rflags |= (1ULL << 0);
                        else
                            v->regs.rflags &= ~(1ULL << 0);
                        if (op2 == 0xAB) a |= 1ULL << (bit & (uint64_t)(sz * 8 - 1));
                        else if (op2 == 0xB3) a &= ~(1ULL << (bit & (uint64_t)(sz * 8 - 1)));
                        else if (op2 == 0xBB) a ^= 1ULL << (bit & (uint64_t)(sz * 8 - 1));
                        a &= vfl_mask(sz);
                        vfl_write_reg(v, ea.reg, rex, sz, a);
                        vfl_adv(v, n, ripmask);
                        return 0;
                    }
                    {
                        int64_t off = (int64_t)(bit / (uint64_t)(sz * 8)) * sz;
                        uint64_t addr = ea.addr + (uint64_t)off;

                        a = 0;
                        rr = vfl_mem_read(vm, v, addr, (uint64_t)sz, (uint8_t *)&a);
                        if (rr) return vfl_memerr(v, run, rr);
                        a &= vfl_mask(sz);
                        if ((a >> (bit & (uint64_t)(sz * 8 - 1))) & 1ULL)
                            v->regs.rflags |= (1ULL << 0);
                        else
                            v->regs.rflags &= ~(1ULL << 0);
                        if (op2 != 0xA3) {
                            if (op2 == 0xAB) a |= 1ULL << (bit & (uint64_t)(sz * 8 - 1));
                            else if (op2 == 0xB3) a &= ~(1ULL << (bit & (uint64_t)(sz * 8 - 1)));
                            else a ^= 1ULL << (bit & (uint64_t)(sz * 8 - 1));
                            a &= vfl_mask(sz);
                            return vfl_exec_store(vm, v, run, &ea, a, sz, rex, n, ripmask);
                        }
                    }
                    vfl_adv(v, n, ripmask);
                    return 0;
                }

                case 0xBA:
                    if (pos >= 15) return vfl_exit_internal(v, run);
                    m = buf[pos++];
                    mod = (m >> 6) & 3;
                    reg = (m >> 3) & 7;
                    rm = (m & 7) | (rex_b << 3);
                    if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                        return vfl_exit_internal(v, run);
                    if (pos + 1 > 15) return vfl_exit_internal(v, run);
                    b = buf[pos++];
                    n = pos;
                    if (!ea.is_reg && ea.rip_rel)
                        ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
                    if (reg > 7) return vfl_exit_exception(v, run, 6, 0);
                    sz = osz;
                    a = 0;
                    rr = vfl_ea_read(vm, v, &ea, sz, rex, &a);
                    if (rr == -2) return vfl_mem_fault(v, run);
                    if (rr != 0) {
                        if (ea.is_reg) return vfl_exit_internal(v, run);
                        return vfl_memerr(v, run, rr);
                    }
                    a &= vfl_mask(sz);
                    if ((a >> (b & (uint64_t)(sz * 8 - 1))) & 1ULL)
                        v->regs.rflags |= (1ULL << 0);
                    else
                        v->regs.rflags &= ~(1ULL << 0);
                    if (reg == 6) a |= 1ULL << (b & (uint64_t)(sz * 8 - 1));
                    else if (reg == 2 || reg == 3) a &= ~(1ULL << (b & (uint64_t)(sz * 8 - 1)));
                    else if (reg == 7) a ^= 1ULL << (b & (uint64_t)(sz * 8 - 1));
                    else if (reg == 4 || reg == 5) {
                        vfl_adv(v, n, ripmask);
                        return 0;
                    }
                    a &= vfl_mask(sz);
                    if (reg == 0 || reg == 1) {
                        vfl_adv(v, n, ripmask);
                        return 0;
                    }
                    return vfl_exec_store(vm, v, run, &ea, a, sz, rex, n, ripmask);

                case 0xAF:
                    if (pos >= 15) return vfl_exit_internal(v, run);
                    m = buf[pos++];
                    mod = (m >> 6) & 3;
                    reg = ((m >> 3) & 7) | (rex_r << 3);
                    rm = (m & 7) | (rex_b << 3);
                    if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                        return vfl_exit_internal(v, run);
                    n = pos;
                    if (!ea.is_reg && ea.rip_rel)
                        ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
                    rr = vfl_ea_read(vm, v, &ea, osz, rex, &a);
                    if (rr == -2) return vfl_mem_fault(v, run);
                    if (rr != 0) {
                        if (ea.is_reg) return vfl_exit_internal(v, run);
                        return vfl_memerr(v, run, rr);
                    }
                    {
                        int64_t sa;
                        int64_t sb;
                        __int128_t prod;
                        int64_t slow;

                        if (osz == 2) {
                            sa = (int16_t)(vfl_read_reg(v, reg, rex, 2) & 0xFFFFULL);
                            sb = (int16_t)(a & 0xFFFFULL);
                        } else if (osz == 4) {
                            sa = (int32_t)(vfl_read_reg(v, reg, rex, 4) & 0xFFFFFFFFULL);
                            sb = (int32_t)(a & 0xFFFFFFFFULL);
                        } else {
                            sa = (int64_t)vfl_read_reg(v, reg, rex, 8);
                            sb = (int64_t)a;
                        }
                        prod = (__int128_t)sa * (__int128_t)sb;
                        vfl_write_reg(v, reg, rex, osz, (uint64_t)prod);
                        if (osz == 2) slow = (int16_t)(uint16_t)prod;
                        else if (osz == 4) slow = (int32_t)(uint32_t)prod;
                        else slow = (int64_t)(uint64_t)prod;
                        if (prod != slow) v->regs.rflags |= (1ULL << 0) | (1ULL << 11);
                        else v->regs.rflags &= ~((1ULL << 0) | (1ULL << 11));
                    }
                    vfl_adv(v, n, ripmask);
                    return 0;

                case 0xB6:
                case 0xB7:
                case 0xBE:
                case 0xBF: {
                    int ssz = (op2 == 0xB6 || op2 == 0xBE) ? 1 : 2;
                    int sx = (op2 == 0xBE || op2 == 0xBF);

                    if (pos >= 15) return vfl_exit_internal(v, run);
                    m = buf[pos++];
                    mod = (m >> 6) & 3;
                    reg = ((m >> 3) & 7) | (rex_r << 3);
                    rm = (m & 7) | (rex_b << 3);
                    if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                        return vfl_exit_internal(v, run);
                    n = pos;
                    if (!ea.is_reg && ea.rip_rel)
                        ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
                    rr = vfl_ea_read(vm, v, &ea, ssz, rex, &a);
                    if (rr == -2) return vfl_mem_fault(v, run);
                    if (rr == 1 && !ea.is_reg)
                        return vfl_mmio_or_fault(vm, v, run, ea.addr, ssz, VFL_P_MMIO_REG,
                                                 reg, rex, 0, 0, n, ripmask);
                    if (rr != 0) return vfl_exit_internal(v, run);
                    a &= vfl_mask(ssz);
                    if (sx && (a & (1ULL << (ssz * 8 - 1))))
                        a |= ~vfl_mask(ssz);
                    if (lm && rex_w) {
                        if (sx) {
                            if (ssz == 1) *vfl_reg64(v, reg) = (uint64_t)(int64_t)(int8_t)a;
                            else *vfl_reg64(v, reg) = (uint64_t)(int64_t)(int16_t)a;
                        } else {
                            vfl_write_reg(v, reg, rex, 4, a);
                        }
                    } else if (osz == 2) {
                        vfl_write_reg(v, reg, rex, 2, a);
                    } else {
                        vfl_write_reg(v, reg, rex, 4, a);
                    }
                    vfl_adv(v, n, ripmask);
                    return 0;
                }

                case 0xB0:
                case 0xB1: {
                    int csz = (op2 == 0xB0) ? 1 : osz;

                    if (pos >= 15) return vfl_exit_internal(v, run);
                    m = buf[pos++];
                    mod = (m >> 6) & 3;
                    reg = ((m >> 3) & 7) | (rex_r << 3);
                    rm = (m & 7) | (rex_b << 3);
                    if (rep == 3) {
                        uint64_t cnt = 0;
                        uint64_t val;

                        if (mod != 3) return vfl_exit_exception(v, run, 6, 0);
                        n = pos;
                        val = vfl_read_reg(v, rm, rex, csz) & vfl_mask(csz);
                        while (val) {
                            cnt += val & 1ULL;
                            val >>= 1;
                        }
                        vfl_write_reg(v, reg, rex, csz, cnt);
                        v->regs.rflags &= ~((1ULL << 0) | (1ULL << 2) | (1ULL << 4) |
                                            (1ULL << 6) | (1ULL << 7) | (1ULL << 11));
                        if (cnt == 0) v->regs.rflags |= (1ULL << 6);
                        vfl_adv(v, n, ripmask);
                        return 0;
                    }
                    if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                        return vfl_exit_internal(v, run);
                    n = pos;
                    if (!ea.is_reg && ea.rip_rel)
                        ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
                    rr = vfl_ea_read(vm, v, &ea, csz, rex, &a);
                    if (rr == -2) return vfl_mem_fault(v, run);
                    if (rr != 0) {
                        if (ea.is_reg) return vfl_exit_internal(v, run);
                        return vfl_memerr(v, run, rr);
                    }
                    a &= vfl_mask(csz);
                    b = vfl_read_reg(v, 0, rex, csz);
                    r = (a - b) & vfl_mask(csz);
                    vfl_flag_sub(v, a, b, 0, r, csz);
                    if (r == 0)
                        return vfl_exec_store(vm, v, run, &ea,
                                              vfl_read_reg(v, reg, rex, csz),
                                              csz, rex, n, ripmask);
                    vfl_write_reg(v, 0, rex, csz, a);
                    vfl_adv(v, n, ripmask);
                    return 0;
                }

                case 0xBC:
                case 0xBD: {
                    int csz = osz;

                    if (pos >= 15) return vfl_exit_internal(v, run);
                    m = buf[pos++];
                    mod = (m >> 6) & 3;
                    reg = ((m >> 3) & 7) | (rex_r << 3);
                    rm = (m & 7) | (rex_b << 3);
                    if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                        return vfl_exit_internal(v, run);
                    n = pos;
                    if (!ea.is_reg && ea.rip_rel)
                        ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
                    rr = vfl_ea_read(vm, v, &ea, csz, rex, &a);
                    if (rr == -2) return vfl_mem_fault(v, run);
                    if (rr != 0) {
                        if (ea.is_reg) return vfl_exit_internal(v, run);
                        return vfl_memerr(v, run, rr);
                    }
                    a &= vfl_mask(csz);
                    if (rep == 3 && op2 == 0xBC) {
                        int i;
                        int found = csz * 8;

                        for (i = 0; i < csz * 8; i++) {
                            if ((a >> i) & 1ULL) { found = i; break; }
                        }
                        vfl_write_reg(v, reg, rex, csz, (uint64_t)found);
                        if (a == 0) v->regs.rflags |= (1ULL << 0);
                        else v->regs.rflags &= ~(1ULL << 0);
                        if (found == 0) v->regs.rflags |= (1ULL << 6);
                        else v->regs.rflags &= ~(1ULL << 6);
                    } else if (rep == 3) {
                        int i;
                        int found = csz * 8;

                        for (i = csz * 8 - 1; i >= 0; i--) {
                            if ((a >> i) & 1ULL) { found = csz * 8 - 1 - i; break; }
                        }
                        vfl_write_reg(v, reg, rex, csz, (uint64_t)found);
                        if (a == 0) v->regs.rflags |= (1ULL << 0);
                        else v->regs.rflags &= ~(1ULL << 0);
                        if (found == 0) v->regs.rflags |= (1ULL << 6);
                        else v->regs.rflags &= ~(1ULL << 6);
                    } else if (a == 0) {
                        v->regs.rflags |= (1ULL << 6);
                    } else {
                        int i = 0;

                        v->regs.rflags &= ~(1ULL << 6);
                        if (op2 == 0xBC) {
                            while (!((a >> i) & 1ULL)) i++;
                        } else {
                            i = csz * 8 - 1;
                            while (!((a >> i) & 1ULL)) i--;
                            i = csz * 8 - 1 - i;
                        }
                        vfl_write_reg(v, reg, rex, csz, (uint64_t)i);
                    }
                    vfl_adv(v, n, ripmask);
                    return 0;
                }

                case 0xC0:
                case 0xC1: {
                    int csz = (op2 == 0xC0) ? 1 : osz;
                    uint64_t t;

                    if (pos >= 15) return vfl_exit_internal(v, run);
                    m = buf[pos++];
                    mod = (m >> 6) & 3;
                    reg = ((m >> 3) & 7) | (rex_r << 3);
                    rm = (m & 7) | (rex_b << 3);
                    if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                        return vfl_exit_internal(v, run);
                    n = pos;
                    if (!ea.is_reg && ea.rip_rel)
                        ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
                    if (mod != 3) {
                        a = 0;
                        rr = vfl_mem_read(vm, v, ea.addr, (uint64_t)csz, (uint8_t *)&a);
                        if (rr) return vfl_memerr(v, run, rr);
                        a &= vfl_mask(csz);
                        ea.is_reg = 1;
                        ea.reg = 16;
                    } else {
                        a = vfl_read_reg(v, rm, rex, csz);
                    }
                    b = vfl_read_reg(v, reg, rex, csz);
                    t = vfl_do_alu(v, VFL_RMW_ADD, a, b, csz);
                    if (ea.is_reg && ea.reg == 16) {
                        uint8_t wb[8];
                        int w;

                        vfl_put64(wb, t, csz);
                        w = vfl_mem_write(vm, v, ea.addr, (uint64_t)csz, wb);
                        if (w == -2) return vfl_mem_fault(v, run);
                        if (w) return vfl_memerr(v, run, w);
                        vfl_write_reg(v, reg, rex, csz, a);
                    } else {
                        vfl_write_reg(v, rm, rex, csz, t);
                        vfl_write_reg(v, reg, rex, csz, a);
                    }
                    vfl_adv(v, n, ripmask);
                    return 0;
                }

                case 0xC8:
                case 0xC9:
                case 0xCA:
                case 0xCB:
                case 0xCC:
                case 0xCD:
                case 0xCE:
                case 0xCF: {
                    int br = (op2 & 7) | (rex_b << 3);

                    if (osz == 2) return vfl_exit_exception(v, run, 6, 0);
                    n = pos;
                    a = vfl_read_reg(v, br, rex, osz);
                    if (osz == 4)
                        *vfl_reg64(v, br) = (uint64_t)(uint32_t)__builtin_bswap32((uint32_t)a);
                    else
                        *vfl_reg64(v, br) = __builtin_bswap64(a);
                    vfl_adv(v, n, ripmask);
                    return 0;
                }

                case 0x00: {
                    int grp;

                    if (pos >= 15) return vfl_exit_internal(v, run);
                    m = buf[pos++];
                    mod = (m >> 6) & 3;
                    grp = (m >> 3) & 7;
                    rm = (m & 7) | (rex_b << 3);
                    if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                        return vfl_exit_internal(v, run);
                    n = pos;
                    if (!ea.is_reg && ea.rip_rel)
                        ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
                    if (grp == 0) {
                        return vfl_exec_store(vm, v, run, &ea, 0, 2, rex, n, ripmask);
                    } else if (grp == 1) {
                        return vfl_exec_store(vm, v, run, &ea,
                                              (uint64_t)v->sregs.tr.selector,
                                              2, rex, n, ripmask);
                    } else if (grp == 4 || grp == 5) {
                        v->regs.rflags |= (1ULL << 6);
                    } else if (grp == 2 || grp == 3) {
                        return vfl_exit_exception(v, run, 13, 0);
                    } else {
                        return vfl_exit_exception(v, run, 6, 0);
                    }
                    vfl_adv(v, n, ripmask);
                    return 0;
                }

                case 0x01: {
                    int grp;

                    if (pos >= 15) return vfl_exit_internal(v, run);
                    m = buf[pos++];
                    mod = (m >> 6) & 3;
                    grp = (m >> 3) & 7;
                    rm = (m & 7) | (rex_b << 3);
                    if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                        return vfl_exit_internal(v, run);
                    n = pos;
                    if (!ea.is_reg && ea.rip_rel)
                        ea.addr = (v->regs.rip + (uint64_t)n + (uint64_t)(int64_t)ea.disp) & ripmask;
                    switch (grp) {
                        case 0:
                        case 1: {
                            uint64_t base = grp == 0 ? v->sregs.gdt_base : v->sregs.idt_base;
                            uint64_t lim = grp == 0 ? v->sregs.gdt_limit : v->sregs.idt_limit;

                            if (!ea.is_reg) {
                                uint8_t wb[10];
                                int bytes = lm ? 10 : 6;
                                int i;

                                wb[0] = (uint8_t)lim;
                                wb[1] = (uint8_t)(lim >> 8);
                                for (i = 0; i < bytes - 2; i++)
                                    wb[2 + i] = (uint8_t)(base >> (i * 8));
                                rr = vfl_mem_write(vm, v, ea.addr, (uint64_t)bytes, wb);
                                if (rr) return vfl_memerr(v, run, rr);
                            } else {
                                return vfl_exit_exception(v, run, 6, 0);
                            }
                            break;
                        }
                        case 2:
                        case 3: {
                            uint8_t wb[10];
                            uint64_t wbase = 0;
                            uint64_t i;
                            int bytes = lm ? 10 : 6;

                            if ((v->sregs.cs.selector & 3) != 0)
                                return vfl_exit_exception(v, run, 13, 0);
                            if (ea.is_reg)
                                return vfl_exit_exception(v, run, 6, 0);
                            rr = vfl_mem_read(vm, v, ea.addr, (uint64_t)bytes, wb);
                            if (rr == -2) return vfl_mem_fault(v, run);
                            if (rr) return vfl_memerr(v, run, rr);
                            for (i = 0; i < (uint64_t)bytes - 2; i++)
                                wbase |= (uint64_t)wb[2 + i] << (i * 8);
                            if (grp == 2) {
                                v->sregs.gdt_limit = vfl_get16(wb);
                                v->sregs.gdt_base = wbase;
                            } else {
                                v->sregs.idt_limit = vfl_get16(wb);
                                v->sregs.idt_base = wbase;
                            }
                            break;
                        }
                        case 4:
                            if (ea.is_reg) {
                                vfl_write_reg(v, ea.reg, rex, 2, v->sregs.cr0 & 0xFFFFULL);
                            } else {
                                uint8_t wb[8];

                                vfl_put64(wb, v->sregs.cr0 & 0xFFFFULL, 2);
                                rr = vfl_mem_write(vm, v, ea.addr, 2, wb);
                                if (rr) return vfl_memerr(v, run, rr);
                            }
                            break;
                        case 6:
                            a = 0;
                            rr = vfl_ea_read(vm, v, &ea, 2, rex, &a);
                            if (rr == -2) return vfl_mem_fault(v, run);
                            if (rr != 0) {
                                if (ea.is_reg) return vfl_exit_internal(v, run);
                                return vfl_memerr(v, run, rr);
                            }
                            if (!(a & 1ULL))
                                v->sregs.cr0 |= 0x1ULL;
                            v->sregs.cr0 = (v->sregs.cr0 & ~0xEULL) | (a & 0xEULL);
                            vm->tlb_epoch++;
                            break;
                        case 7:
                            if (mod != 3) {
                                n = pos;
                                vm->tlb_epoch++;
                                vfl_adv(v, n, ripmask);
                                return 0;
                            }
                            return vfl_exit_exception(v, run, 6, 0);
                        default:
                            return vfl_exit_exception(v, run, 6, 0);
                    }
                    vfl_adv(v, n, ripmask);
                    return 0;
                }

                case 0x18:
                    if (pos >= 15) return vfl_exit_internal(v, run);
                    m = buf[pos++];
                    mod = (m >> 6) & 3;
                    rm = (m & 7) | (rex_b << 3);
                    if (vfl_decode_ea(v, buf, &pos, mod, rm, asz, seg, rex_b, rex_x, &ea) < 0)
                        return vfl_exit_internal(v, run);
                    n = pos;
                    vfl_adv(v, n, ripmask);
                    return 0;

                default:
                    return vfl_exit_internal(v, run);
            }
        }

        default:
            return vfl_exit_internal(v, run);
    }
}
