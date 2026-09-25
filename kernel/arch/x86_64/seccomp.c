#include <lebirun/seccomp.h>
#include <lebirun/task.h>
#include <lebirun/mem_map.h>
#include <lebirun/uaccess.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef struct {
    uint16_t code;
    uint8_t jt;
    uint8_t jf;
    uint32_t k;
} bpf_insn_t;
typedef struct {
    uint16_t len;
    uint64_t filter;
} bpf_fprog_t;
#define BPF_LD 0x00
#define BPF_LDX 0x01
#define BPF_ST 0x02
#define BPF_STX 0x03
#define BPF_ALU 0x04
#define BPF_JMP 0x05
#define BPF_RET 0x06
#define BPF_MISC 0x07
#define BPF_W 0x00
#define BPF_H 0x08
#define BPF_B 0x10
#define BPF_IMM 0x00
#define BPF_ABS 0x20
#define BPF_IND 0x40
#define BPF_MEM 0x60
#define BPF_LEN 0x80
#define BPF_MSH 0xa0
#define BPF_ADD 0x00
#define BPF_SUB 0x10
#define BPF_MUL 0x20
#define BPF_DIV 0x30
#define BPF_OR 0x40
#define BPF_AND 0x50
#define BPF_LSH 0x60
#define BPF_RSH 0x70
#define BPF_NEG 0x80
#define BPF_MOD 0x90
#define BPF_XOR 0xa0
#define BPF_JA 0x00
#define BPF_JEQ 0x10
#define BPF_JGT 0x20
#define BPF_JGE 0x30
#define BPF_JSET 0x40
#define BPF_K 0x00
#define BPF_X 0x08
#define BPF_A 0x10
#define BPF_TAX 0x00
#define BPF_TXA 0x80
#define SECCOMP_RET_ALLOW 0x7fff0000u
#define SECCOMP_RET_KILL 0x00000000u
#define SECCOMP_RET_TRAP 0x00030000u
#define SECCOMP_RET_ERRNO 0x00050000u
#define SECCOMP_RET_MASK 0x0000ffffu
#define AUDIT_ARCH_X86_64 0xc000003eu
#define SECCOMP_BPF_MAX_LEN 512
#define SECCOMP_BPF_MAX_STEPS 4096
static uint32_t bpf_load_word(uint32_t nr, uint32_t arch, uint64_t ip,
                              uint64_t *args, uint32_t k) {
    if (k == 0) return (uint32_t)nr;
    if (k == 4) return arch;
    if (k == 8) return (uint32_t)(ip & 0xffffffffu);
    if (k == 12) return (uint32_t)(ip >> 32);
    if (k >= 16 && k <= 56 && ((k - 16) & 7) == 0) {
        uint64_t v = args[(k - 16) / 8];
        return (uint32_t)(v & 0xffffffffu);
    }
    return 0;
}
static int bpf_check_program(bpf_insn_t *prog, uint64_t len) {
    uint64_t pc;
    uint16_t code;
    uint16_t cls;
    uint32_t target;
    if (!prog || len == 0 || len > SECCOMP_BPF_MAX_LEN) return -1;
    for (pc = 0; pc < len; pc++) {
        code = prog[pc].code;
        cls = code & 0x07;
        if (cls == BPF_LD) {
            if (code == (BPF_LD | BPF_W | BPF_ABS)) {
                uint32_t k = prog[pc].k;
                if (k != 0 && k != 4 && k != 8 && k != 12 &&
                    (k < 16 || k > 56 || ((k - 16) & 7) != 0)) return -1;
            } else if (code == (BPF_LD | BPF_W | BPF_IMM)) {
            } else if (code == (BPF_LD | BPF_W | BPF_LEN)) {
            } else if (code == (BPF_LD | BPF_MEM)) {
                if ((prog[pc].k & ~0xfu) != 0) return -1;
            } else {
                return -1;
            }
        } else if (cls == BPF_LDX) {
            if (code != (BPF_LDX | BPF_W | BPF_IMM) &&
                code != (BPF_LDX | BPF_W | BPF_LEN) &&
                code != (BPF_LDX | BPF_MEM)) return -1;
            if (code == (BPF_LDX | BPF_MEM) &&
                (prog[pc].k & ~0xfu) != 0) return -1;
        } else if (cls == BPF_ST) {
            if (code != (BPF_ST | BPF_MEM)) return -1;
            if ((prog[pc].k & ~0xfu) != 0) return -1;
        } else if (cls == BPF_STX) {
            if (code != (BPF_STX | BPF_MEM)) return -1;
            if ((prog[pc].k & ~0xfu) != 0) return -1;
        } else if (cls == BPF_ALU) {
            uint16_t op = code & 0xf0;
            uint16_t src = code & 0x08;
            if (op != BPF_ADD && op != BPF_SUB && op != BPF_MUL &&
                op != BPF_DIV && op != BPF_MOD && op != BPF_OR &&
                op != BPF_AND && op != BPF_XOR && op != BPF_LSH &&
                op != BPF_RSH && op != BPF_NEG) return -1;
            if (src != BPF_K && src != BPF_X) return -1;
            if (op == BPF_NEG && src != BPF_X) return -1;
        } else if (cls == BPF_JMP) {
            uint16_t op = code & 0xf0;
            if (op == BPF_JA) {
                if (prog[pc].k == 0) return -1;
                target = (uint32_t)(pc + 1 + prog[pc].k);
                if (target >= len || target <= pc) return -1;
            } else if (op == BPF_JEQ || op == BPF_JGT || op == BPF_JGE ||
                       op == BPF_JSET) {
                uint16_t src = code & 0x08;
                if (src != BPF_K && src != BPF_X) return -1;
                target = (uint32_t)(pc + 1 + prog[pc].jt);
                if (target >= len || target <= pc) return -1;
                target = (uint32_t)(pc + 1 + prog[pc].jf);
                if (target >= len || target <= pc) return -1;
            } else {
                return -1;
            }
        } else if (cls == BPF_RET) {
            if (code != (BPF_RET | BPF_K) && code != (BPF_RET | BPF_X) &&
                code != (BPF_RET | BPF_A)) return -1;
        } else if (cls == BPF_MISC) {
            if (code != (BPF_MISC | BPF_TAX) &&
                code != (BPF_MISC | BPF_TXA)) return -1;
        } else {
            return -1;
        }
    }
    code = prog[len - 1].code & 0x07;
    if (code != BPF_RET) return -1;
    return 0;
}
static uint32_t bpf_run(bpf_insn_t *prog, uint64_t len, uint32_t nr,
                        uint32_t arch, uint64_t ip, uint64_t *args) {
    uint32_t mem[16];
    uint32_t a = 0;
    uint32_t x = 0;
    uint64_t pc = 0;
    uint64_t steps = 0;
    uint64_t i;
    for (i = 0; i < 16; i++) mem[i] = 0;
    while (pc < len) {
        uint16_t code;
        if (++steps > SECCOMP_BPF_MAX_STEPS) return SECCOMP_RET_KILL;
        code = prog[pc].code;
        switch (code & 0x07) {
        case BPF_LD:
            if (code == (BPF_LD | BPF_W | BPF_ABS))
                a = bpf_load_word(nr, arch, ip, args, prog[pc].k);
            else if (code == (BPF_LD | BPF_W | BPF_IMM))
                a = prog[pc].k;
            else if (code == (BPF_LD | BPF_MEM))
                a = mem[prog[pc].k & 0xf];
            else
                a = (uint32_t)len;
            pc++;
            break;
        case BPF_LDX:
            if (code == (BPF_LDX | BPF_W | BPF_IMM))
                x = prog[pc].k;
            else if (code == (BPF_LDX | BPF_MEM))
                x = mem[prog[pc].k & 0xf];
            else
                x = (uint32_t)len;
            pc++;
            break;
        case BPF_ST:
            mem[prog[pc].k & 0xf] = a;
            pc++;
            break;
        case BPF_STX:
            mem[prog[pc].k & 0xf] = x;
            pc++;
            break;
        case BPF_MISC:
            if (code == (BPF_MISC | BPF_TAX)) x = a;
            else a = x;
            pc++;
            break;
        case BPF_ALU: {
            uint32_t v = (code & 0x08) ? x : prog[pc].k;
            switch (code & 0xf0) {
            case BPF_ADD: a += v; break;
            case BPF_SUB: a -= v; break;
            case BPF_MUL: a *= v; break;
            case BPF_DIV: a = v ? a / v : 0; break;
            case BPF_MOD: a = v ? a % v : 0; break;
            case BPF_OR: a |= v; break;
            case BPF_AND: a &= v; break;
            case BPF_XOR: a ^= v; break;
            case BPF_LSH: a <<= (v & 31); break;
            case BPF_RSH: a >>= (v & 31); break;
            case BPF_NEG: a = (uint32_t)-(int32_t)a; break;
            }
            pc++;
            break;
        }
        case BPF_JMP: {
            uint16_t op = code & 0xf0;
            uint32_t v;
            int take;
            if (op == BPF_JA) {
                pc = pc + 1 + prog[pc].k;
                break;
            }
            v = (code & 0x08) ? x : prog[pc].k;
            take = 0;
            if (op == BPF_JEQ) take = (a == v);
            else if (op == BPF_JGT) take = (a > v);
            else if (op == BPF_JGE) take = (a >= v);
            else take = ((a & v) != 0);
            pc = pc + 1 + (take ? prog[pc].jt : prog[pc].jf);
            break;
        }
        case BPF_RET:
            if (code == (BPF_RET | BPF_X)) return x;
            if (code == (BPF_RET | BPF_A)) return a;
            return prog[pc].k;
        default:
            return SECCOMP_RET_KILL;
        }
    }
    return SECCOMP_RET_KILL;
}
int seccomp_install(task_t *task, const void *uprog) {
    bpf_fprog_t fprog;
    bpf_insn_t *insns;
    task_ext_t *e;
    if (!task || !uprog) return -22;
    if ((uint64_t)(uintptr_t)uprog < 0x1000 ||
        (uint64_t)(uintptr_t)uprog >= KERNEL_VMA) return -14;
    if (copy_from_user(&fprog, uprog, sizeof(fprog)) != 0) return -14;
    if (fprog.len == 0 || fprog.len > SECCOMP_BPF_MAX_LEN) return -22;
    if (fprog.filter == 0) return -14;
    insns = (bpf_insn_t *)kmalloc((uint64_t)fprog.len * sizeof(bpf_insn_t));
    if (!insns) return -12;
    if (copy_from_user(insns, (const void *)(uintptr_t)fprog.filter,
                       (uint64_t)fprog.len * sizeof(bpf_insn_t)) != 0) {
        kfree(insns);
        return -14;
    }
    if (bpf_check_program(insns, fprog.len) != 0) {
        kfree(insns);
        return -22;
    }
    e = task_ext_get(task, 1);
    if (!e) {
        kfree(insns);
        return -12;
    }
    if (e->seccomp_filter) kfree(e->seccomp_filter);
    e->seccomp_filter = insns;
    e->seccomp_filter_len = fprog.len;
    return 0;
}
int64_t seccomp_check(task_t *task, int nr, uint64_t ip, uint64_t a0,
                          uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
    task_ext_t *e;
    bpf_insn_t *prog;
    uint64_t len;
    uint64_t args[6];
    uint32_t ret;
    if (!task) return 0;
    e = task_ext_get(task, 0);
    if (!e || !e->seccomp_filter || !e->seccomp_filter_len) return 0;
    prog = (bpf_insn_t *)e->seccomp_filter;
    len = e->seccomp_filter_len;
    args[0] = a0;
    args[1] = a1;
    args[2] = a2;
    args[3] = a3;
    args[4] = a4;
    args[5] = a5;
    ret = bpf_run(prog, len, (uint32_t)nr, AUDIT_ARCH_X86_64, ip, args);
    if ((ret & 0x7fff0000u) == SECCOMP_RET_ALLOW) return 0;
    if ((ret & 0x7fff0000u) == SECCOMP_RET_ERRNO) {
        int err = (int)(ret & SECCOMP_RET_MASK);
        if (err <= 0 || err > 4095) err = 1;
        return -(int64_t)err;
    }
    deliver_signal_to_task(task, 31);
    return -38;
}
int seccomp_fork(task_t *parent, task_t *child) {
    task_ext_t *pe;
    task_ext_t *ce;
    bpf_insn_t *copy;
    if (!parent || !child) return -22;
    pe = task_ext_get(parent, 0);
    if (!pe || !pe->seccomp_filter || !pe->seccomp_filter_len) return 0;
    ce = task_ext_get(child, 1);
    if (!ce) return -12;
    copy = (bpf_insn_t *)kmalloc(pe->seccomp_filter_len * sizeof(bpf_insn_t));
    if (!copy) return -12;
    memcpy(copy, pe->seccomp_filter,
           pe->seccomp_filter_len * sizeof(bpf_insn_t));
    ce->seccomp_filter = copy;
    ce->seccomp_filter_len = pe->seccomp_filter_len;
    return 0;
}
void seccomp_release_task(task_t *task) {
    task_ext_t *e;
    if (!task) return;
    e = task_ext_get(task, 0);
    if (!e) return;
    if (e->seccomp_filter) {
        kfree(e->seccomp_filter);
        e->seccomp_filter = NULL;
        e->seccomp_filter_len = 0;
    }
}
