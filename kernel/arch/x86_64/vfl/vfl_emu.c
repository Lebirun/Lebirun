#include "vfl_priv.h"
#include "../syscalls/syscall_defs.h"
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <string.h>
#include <stdint.h>

int vfl_exit_exception(vfl_vcpu_t *v, vfl_run_t *run, uint32_t vec, uint32_t err) {
    run->exit_reason = VFL_EXIT_EXCEPTION;
    run->exception.vector = vec;
    run->exception.error_code = err;
    v->last_exit = *run;
    return 1;
}

int vfl_exit_shutdown(vfl_vcpu_t *v, vfl_run_t *run) {
    run->exit_reason = VFL_EXIT_SHUTDOWN;
    v->last_exit = *run;
    return 1;
}

int vfl_exit_internal(vfl_vcpu_t *v, vfl_run_t *run) {
    run->exit_reason = VFL_EXIT_INTERNAL;
    v->last_exit = *run;
    return -1;
}

int vfl_exit_io_out(vfl_vcpu_t *v, vfl_run_t *run, uint16_t port, int size, uint32_t data) {
    run->exit_reason = VFL_EXIT_IO;
    run->io.port = port;
    run->io.size = (uint8_t)size;
    run->io.direction = 1;
    run->io.data = data;
    v->last_exit = *run;
    return 1;
}

int vfl_exit_io_in(vfl_vcpu_t *v, vfl_run_t *run, uint16_t port, int size,
                   int pend, int reg, int rex, uint64_t addr,
                   int adv, uint64_t ripmask) {
    run->exit_reason = VFL_EXIT_IO;
    run->io.port = port;
    run->io.size = (uint8_t)size;
    run->io.direction = 0;
    run->io.data = 0;
    v->pending = pend;
    v->pending_reg = reg;
    v->pending_rex = rex;
    v->pending_size = size;
    v->pending_addr = addr;
    v->pending_adv = (uint64_t)adv;
    v->pending_mask = ripmask;
    v->pending_port = port;
    v->last_exit = *run;
    return 1;
}

int vfl_exit_mmio_read(vfl_vcpu_t *v, vfl_run_t *run, uint64_t addr, int size,
                       int pend, int reg, int rex, uint64_t src, uint32_t op,
                       int adv, uint64_t ripmask) {
    run->exit_reason = VFL_EXIT_MMIO;
    run->mmio.phys_addr = addr;
    run->mmio.len = (uint32_t)size;
    run->mmio.is_write = 0;
    v->pending = pend;
    if (pend != VFL_P_MMIO_STR) {
        v->pending_addr = addr;
        v->pending_len = (uint32_t)size;
        v->pending_reg = reg;
        v->pending_rex = rex;
        v->pending_size = size;
        v->pending_src = src;
        v->pending_op = op;
        v->pending_adv = (uint64_t)adv;
        v->pending_mask = ripmask;
    }
    v->last_exit = *run;
    return 1;
}

int vfl_exit_mmio_write(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint64_t addr,
                        uint64_t val, int size, int adv, uint64_t ripmask) {
    uint8_t *d = run->mmio.data;
    int i;

    if (size <= 4 && vfl_apic_mmio(vm, v, addr)) {
        uint8_t tmp[4];
        int k;

        for (k = 0; k < size; k++)
            tmp[k] = (uint8_t)(val >> (k * 8));
        if (vfl_apic_write(vm, v, addr, tmp, size) == 0) {
            v->pending = VFL_P_NONE;
            vfl_adv(v, adv, ripmask);
            return 0;
        }
    }
    run->exit_reason = VFL_EXIT_MMIO;
    run->mmio.phys_addr = addr;
    for (i = 0; i < size && i < 8; i++)
        d[i] = (uint8_t)(val >> (i * 8));
    run->mmio.len = (uint32_t)size;
    run->mmio.is_write = 1;
    v->pending = VFL_P_NONE;
    vfl_adv(v, adv, ripmask);
    v->last_exit = *run;
    return 1;
}

int vfl_exit_mmio_write64(vfl_vcpu_t *v, vfl_run_t *run, uint64_t addr,
                          uint8_t *d64, int len, int pend, int reg, int adv,
                          uint64_t ripmask) {
    int i;

    if (len < 0 || len > 64) return vfl_exit_internal(v, run);
    run->exit_reason = VFL_EXIT_MMIO;
    run->mmio.phys_addr = addr;
    for (i = 0; i < len; i++)
        run->mmio.data[i] = d64[i];
    run->mmio.len = (uint32_t)len;
    run->mmio.is_write = 1;
    v->pending = pend;
    if (pend != VFL_P_MMIO_STR) {
        v->pending_addr = addr;
        v->pending_len = (uint32_t)len;
        v->pending_reg = reg;
        v->pending_size = len;
        v->pending_adv = (uint64_t)adv;
        v->pending_mask = ripmask;
    }
    if (pend == VFL_P_NONE) vfl_adv(v, adv, ripmask);
    v->last_exit = *run;
    return 1;
}

static void vfl_str_bump(vfl_vcpu_t *v, int movs, int asz, int s, uint64_t k) {
    int df = (v->regs.rflags >> 10) & 1;
    int64_t step = df ? -(int64_t)s : (int64_t)s;
    uint64_t si = vfl_get_si(v, asz);
    uint64_t di = vfl_get_di(v, asz);

    if (movs)
        vfl_set_si(v, asz, (si + (uint64_t)(step * (int64_t)k)) & vfl_mask(asz));
    vfl_set_di(v, asz, (di + (uint64_t)(step * (int64_t)k)) & vfl_mask(asz));
    vfl_set_count(v, asz, (vfl_get_count(v, asz) - k) & vfl_mask(asz));
}

static int vfl_str_ram(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *out, uint64_t lin,
                       int wr, uint8_t **phost, uint64_t *pphys, uint64_t bytes) {
    vfl_mem_slot_t *sl;
    uint64_t p0;
    uint64_t p1;
    int r;

    r = vfl_translate(vm, v, lin, wr ? VFL_MMU_WRITE : VFL_MMU_READ, &p0);
    if (r) {
        vfl_mem_fault(v, out);
        return 1;
    }
    r = vfl_translate(vm, v, lin + bytes - 1, wr ? VFL_MMU_WRITE : VFL_MMU_READ, &p1);
    if (r) {
        vfl_mem_fault(v, out);
        return 1;
    }
    (void)p1;
    *pphys = p0;
    sl = vfl_find_slot(vm, p0, bytes);
    if (!sl) return -1;
    if (wr && !(sl->flags & VFL_MEM_FLAG_WRITE)) {
        vfl_exit_exception(v, out, 13, 0);
        return 1;
    }
    if (!wr && !(sl->flags & VFL_MEM_FLAG_READ)) {
        vfl_exit_exception(v, out, 13, 0);
        return 1;
    }
    *pphys = p0;
    *phost = sl->host_mem + (p0 - sl->guest_phys);
    return 0;
}

int vfl_str_emit(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *out) {
    int op = (int)v->pending_op;
    int s = v->pending_size;
    int asz = (int)v->pending_port;
    uint64_t k;
    uint64_t si;
    uint64_t di;
    int df;
    uint64_t m;
    uint64_t lin;

    if (s != 1 && s != 2 && s != 4 && s != 8) return vfl_exit_internal(v, out);
    if (op < 0 || op > 2) return vfl_exit_internal(v, out);
    if (v->pending_count == 0) {
        vfl_adv(v, (int)v->pending_adv, v->pending_mask);
        v->pending = VFL_P_NONE;
        return 0;
    }
    k = v->pending_count;
    if (k > 64 / (uint64_t)s) k = 64 / (uint64_t)s;
    if (op != 0 && v->pending_addr != v->pending_src) {
        uint64_t diff = v->pending_addr > v->pending_src ?
            v->pending_addr - v->pending_src : v->pending_src - v->pending_addr;

        if (diff / (uint64_t)s < v->pending_count) k = 1;
    }
    lin = op == 1 ? v->pending_src : v->pending_addr;
    m = 0x1000ULL - (lin & 0xFFFULL);
    if (m / (uint64_t)s < k) {
        k = m / (uint64_t)s;
        if (k == 0) k = 1;
    }
    si = vfl_get_si(v, asz);
    di = vfl_get_di(v, asz);
    df = (v->regs.rflags >> 10) & 1;
    if (asz == 2) {
        uint64_t si16 = si & 0xFFFFULL;
        uint64_t di16 = di & 0xFFFFULL;

        m = df ? si16 / (uint64_t)s + 1 : (0xFFFFULL - si16) / (uint64_t)s + 1;
        if (op != 0 && m < k) k = m;
        m = df ? di16 / (uint64_t)s + 1 : (0xFFFFULL - di16) / (uint64_t)s + 1;
        if (m < k) k = m;
        if (k == 0) k = 1;
    }
    if (op == 0) {
        uint8_t pat[8];
        uint8_t tmp[64];
        uint64_t j;
        uint64_t phys;
        uint8_t *host;
        int r;

        vfl_put64(pat, v->pending_src & vfl_mask(s), s);
        for (j = 0; j < k; j++)
            memcpy(tmp + j * (uint64_t)s, pat, (size_t)s);
        r = vfl_str_ram(vm, v, out, v->pending_addr, 1, &host, &phys, k * (uint64_t)s);
        if (r > 0) return 1;
        if (r < 0) {
            vfl_str_bump(v, 0, asz, s, k);
            v->pending_count -= k;
            v->pending_addr += (uint64_t)((df ? -(int64_t)s : (int64_t)s) * (int64_t)k);
            return vfl_exit_mmio_write64(v, out, phys, tmp, (int)(k * (uint64_t)s),
                                         VFL_P_MMIO_STR, 0, (int)v->pending_adv,
                                         v->pending_mask);
        }
        memcpy(host, tmp, (size_t)(k * (uint64_t)s));
        vfl_mark_dirty(vm, phys, k * (uint64_t)s);
        vfl_str_bump(v, 0, asz, s, k);
        v->pending_count -= k;
        v->pending_addr += (uint64_t)((df ? -(int64_t)s : (int64_t)s) * (int64_t)k);
        return 0;
    }
    if (op == 1) {
        uint64_t sph;
        uint8_t *sh;
        uint8_t tmp[64];
        int r;

        r = vfl_str_ram(vm, v, out, v->pending_src, 0, &sh, &sph, k * (uint64_t)s);
        if (r > 0) return 1;
        if (r < 0) {
            v->pending_op = 2;
            return vfl_str_emit(vm, v, out);
        }
        {
            uint64_t dph;
            uint8_t *dh;

            memcpy(tmp, sh, (size_t)(k * (uint64_t)s));
            r = vfl_str_ram(vm, v, out, v->pending_addr, 1, &dh, &dph, k * (uint64_t)s);
            if (r > 0) return 1;
            if (r < 0) {
                vfl_str_bump(v, 1, asz, s, k);
                v->pending_count -= k;
                v->pending_addr += (uint64_t)((df ? -(int64_t)s : (int64_t)s) * (int64_t)k);
                v->pending_src += (uint64_t)((df ? -(int64_t)s : (int64_t)s) * (int64_t)k);
                return vfl_exit_mmio_write64(v, out, dph, tmp, (int)(k * (uint64_t)s),
                                             VFL_P_MMIO_STR, 0, (int)v->pending_adv,
                                             v->pending_mask);
            }
            memcpy(dh, tmp, (size_t)(k * (uint64_t)s));
            vfl_mark_dirty(vm, dph, k * (uint64_t)s);
            vfl_str_bump(v, 1, asz, s, k);
            v->pending_count -= k;
            v->pending_addr += (uint64_t)((df ? -(int64_t)s : (int64_t)s) * (int64_t)k);
            v->pending_src += (uint64_t)((df ? -(int64_t)s : (int64_t)s) * (int64_t)k);
            return 0;
        }
        vfl_str_bump(v, 1, asz, s, k);
        v->pending_count -= k;
        v->pending_addr += (uint64_t)((df ? -(int64_t)s : (int64_t)s) * (int64_t)k);
        v->pending_src += (uint64_t)((df ? -(int64_t)s : (int64_t)s) * (int64_t)k);
        return vfl_exit_mmio_read(v, out, sph, (int)(k * (uint64_t)s),
                                  VFL_P_MMIO_STR, 0, 0, 0, 1,
                                  (int)v->pending_adv, v->pending_mask);
    }
    {
        uint64_t sph;
        uint8_t *sh;
        int r;

        r = vfl_str_ram(vm, v, out, v->pending_src, 0, &sh, &sph, k * (uint64_t)s);
        if (r > 0) return 1;
        if (r == 0) {
            uint64_t dph;
            uint8_t *dh;
            int r2;

            r2 = vfl_str_ram(vm, v, out, v->pending_addr, 1, &dh, &dph, k * (uint64_t)s);
            if (r2 > 0) return 1;
            if (r2 == 0) {
                memcpy(dh, sh, (size_t)(k * (uint64_t)s));
                vfl_mark_dirty(vm, dph, k * (uint64_t)s);
                vfl_str_bump(v, 1, asz, s, k);
                v->pending_count -= k;
                v->pending_addr += (uint64_t)((df ? -(int64_t)s : (int64_t)s) * (int64_t)k);
                v->pending_src += (uint64_t)((df ? -(int64_t)s : (int64_t)s) * (int64_t)k);
                return 0;
            }
            v->pending_op = 1;
            return vfl_str_emit(vm, v, out);
        }
        {
            uint64_t sph2;
            int r2;

            r2 = vfl_translate(vm, v, v->pending_src, VFL_MMU_READ, &sph2);
            if (r2) return vfl_mem_fault(v, out);
            return vfl_exit_mmio_read(v, out, sph2, (int)(k * (uint64_t)s),
                                      VFL_P_MMIO_STR, 0, 0, 0, 2,
                                      (int)v->pending_adv, v->pending_mask);
        }
    }
}

int vfl_str_resume(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *in, vfl_run_t *out) {
    int op = (int)v->pending_op;
    int s = v->pending_size;
    uint64_t k = v->pending_count;
    uint64_t B;
    uint64_t dph;
    uint8_t *dh;
    int r;

    (void)vm;
    if (s != 1 && s != 2 && s != 4 && s != 8) return vfl_exit_internal(v, out);
    if (op != 2) return vfl_str_emit(vm, v, out);
    if (k == 0) {
        vfl_adv(v, (int)v->pending_adv, v->pending_mask);
        v->pending = VFL_P_NONE;
        return 0;
    }
    if (k > 64 / (uint64_t)s) k = 64 / (uint64_t)s;
    B = k * (uint64_t)s;
    r = vfl_str_ram(vm, v, out, v->pending_addr, 1, &dh, &dph, B);
    if (r > 0) return 1;
    if (r < 0) return vfl_exit_exception(v, out, 14, 0);
    memcpy(dh, in->mmio.data, (size_t)B);
    vfl_mark_dirty(vm, dph, B);
    {
        int asz = (int)v->pending_port;
        int df = (v->regs.rflags >> 10) & 1;
        int64_t step = df ? -(int64_t)s : (int64_t)s;

        vfl_str_bump(v, 1, asz, s, k);
        v->pending_count -= k;
        v->pending_addr += (uint64_t)(step * (int64_t)k);
        v->pending_src += (uint64_t)(step * (int64_t)k);
    }
    return vfl_str_emit(vm, v, out);
}


int vfl_mmio_fill_exit(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint64_t linear,
                       int bytes) {
    uint64_t phys;

    if (bytes <= 0 || bytes > 64) return vfl_exit_exception(v, run, 14, 0);
    if (vfl_translate(vm, v, linear, VFL_MMU_READ, &phys))
        return vfl_mem_fault(v, run);
    if (phys != linear) return vfl_exit_exception(v, run, 14, 0);
    return vfl_exit_mmio_read(v, run, phys, bytes, VFL_P_MMIO_FILL, 0, 0,
                              linear, 0, 0, 0);
}

int vfl_mmio_or_fault(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint64_t addr,
                      int size, int pend, int reg, int rex, uint64_t src,
                      uint32_t op, int adv, uint64_t ripmask) {
    uint64_t phys;
    int r;

    if (vfl_translate(vm, v, addr, VFL_MMU_READ, &phys))
        return vfl_mem_fault(v, run);
    if (vfl_find_slot(vm, phys, (uint64_t)size))
        return vfl_exit_exception(v, run, 13, 0);
    r = vfl_exit_mmio_read(v, run, addr, size, pend, reg, rex, src, op, adv, ripmask);
    run->mmio.phys_addr = phys;
    return r;
}

int vfl_ea_read(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_ea_t *ea, int size, int rex, uint64_t *out) {
    *out = 0;
    if (ea->is_reg) {
        *out = vfl_read_reg(v, ea->reg, rex, size);
        return 0;
    }
    return vfl_mem_read(vm, v, ea->addr, (uint64_t)size, (uint8_t *)out);
}

int vfl_exec_rmw(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                 int op, uint64_t src, int size, int rex, int n, uint64_t ripmask) {
    uint64_t dst = 0;
    uint64_t res;
    int r;

    if (ea->is_reg) {
        dst = vfl_read_reg(v, ea->reg, rex, size);
        res = vfl_do_alu(v, op, dst, src, size);
        if (op != VFL_RMW_CMP) vfl_write_reg(v, ea->reg, rex, size, res);
        vfl_adv(v, n, ripmask);
        return 0;
    }
    r = vfl_mem_read(vm, v, ea->addr, (uint64_t)size, (uint8_t *)&dst);
    if (r == -2)
        return vfl_mem_fault(v, run);
    if (r == 1)
        return vfl_mmio_or_fault(vm, v, run, ea->addr, size, VFL_P_MMIO_RMW,
                                 0, 0, src & vfl_mask(size), (uint32_t)op, n, ripmask);
    if (r < 0)
        return vfl_exit_exception(v, run, 13, 0);
    dst &= vfl_mask(size);
    res = vfl_do_alu(v, op, dst, src, size);
    if (op == VFL_RMW_CMP) {
        vfl_adv(v, n, ripmask);
        return 0;
    }
    {
        uint8_t b[8];
        int w;

        vfl_put64(b, res, size);
        w = vfl_mem_write(vm, v, ea->addr, (uint64_t)size, b);
        if (w == -2)
            return vfl_mem_fault(v, run);
        if (w == 1) {
            uint64_t phys;

            if (vfl_translate(vm, v, ea->addr, VFL_MMU_WRITE, &phys))
                return vfl_mem_fault(v, run);
            return vfl_exit_mmio_write(vm, v, run, phys, res, size, n, ripmask);
        }
        if (w < 0)
            return vfl_exit_exception(v, run, 13, 0);
    }
    vfl_adv(v, n, ripmask);
    return 0;
}

int vfl_exec_store(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                   uint64_t val, int size, int rex, int n, uint64_t ripmask) {
    uint8_t b[8];
    int r;

    if (ea->is_reg) {
        vfl_write_reg(v, ea->reg, rex, size, val);
        vfl_adv(v, n, ripmask);
        return 0;
    }
    vfl_put64(b, val, size);
    r = vfl_mem_write(vm, v, ea->addr, (uint64_t)size, b);
    if (r == -2)
        return vfl_mem_fault(v, run);
    if (r == 1) {
        uint64_t phys;

        if (vfl_translate(vm, v, ea->addr, VFL_MMU_WRITE, &phys))
            return vfl_mem_fault(v, run);
        return vfl_exit_mmio_write(vm, v, run, phys, val, size, n, ripmask);
    }
    if (r < 0)
        return vfl_exit_exception(v, run, 13, 0);
    vfl_adv(v, n, ripmask);
    return 0;
}

int vfl_exec_load_reg(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                      int reg, int size, int rex, int n, uint64_t ripmask) {
    uint64_t val = 0;
    int r;

    if (ea->is_reg) {
        vfl_write_reg(v, reg, rex, size, vfl_read_reg(v, ea->reg, rex, size));
        vfl_adv(v, n, ripmask);
        return 0;
    }
    r = vfl_mem_read(vm, v, ea->addr, (uint64_t)size, (uint8_t *)&val);
    if (r == -2)
        return vfl_mem_fault(v, run);
    if (r == 1)
        return vfl_mmio_or_fault(vm, v, run, ea->addr, size, VFL_P_MMIO_REG,
                                 reg, rex, 0, 0, n, ripmask);
    if (r < 0)
        return vfl_exit_exception(v, run, 13, 0);
    vfl_write_reg(v, reg, rex, size, val & vfl_mask(size));
    vfl_adv(v, n, ripmask);
    return 0;
}

int vfl_memerr(vfl_vcpu_t *v, vfl_run_t *run, int rr) {
    if (rr == -2) return vfl_mem_fault(v, run);
    if (rr == 1) return vfl_exit_exception(v, run, 14, 0);
    return vfl_exit_exception(v, run, 13, 0);
}
