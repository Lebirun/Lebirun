#ifndef VFL_SIMD_H
#define VFL_SIMD_H

#include "vfl_priv.h"
#include "../syscalls/syscall_defs.h"
#include <lebirun/common.h>
#include <lebirun/mem_map.h>
#include <string.h>
#include <stdint.h>
#define vfl_u32f(x) (__extension__ ({ float _v = 0; memcpy(&_v, &(x), 4); _v; }))
#define vfl_u64d(x) (__extension__ ({ double _v = 0; memcpy(&_v, &(x), 8); _v; }))
#define vfl_f32_add(a, b) ((a) + (b))
#define vfl_f32_sub(a, b) ((a) - (b))
#define vfl_f32_mul(a, b) ((a) * (b))
#define vfl_f32_div(a, b) ((a) / (b))
#define vfl_f64_add(a, b) ((a) + (b))
#define vfl_f64_sub(a, b) ((a) - (b))
#define vfl_f64_mul(a, b) ((a) * (b))
#define vfl_f64_div(a, b) ((a) / (b))
#define vfl_f32_sqrt(x) (__extension__ ({ float _v = (x); __asm__ volatile ("fsqrt" : "+t"(_v)); _v; }))
#define vfl_f64_sqrt(x) (__extension__ ({ double _v = (x); __asm__ volatile ("fsqrt" : "+t"(_v)); _v; }))

int sse_movmsk(vfl_vcpu_t *v, int dst, int src, int rex, int el, int n, uint64_t ripmask);
int escape_modrm(vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf, int *pos,
                        int rex_r, int rex_b, int *mod, int *reg, int *rm);
int escape_ea(vfl_vcpu_t *v, uint8_t *buf, int *pos, int mod,
                     int rm, int asz, int seg, int rex_b, int rex_x, vfl_ea_t *ea,
                     uint64_t rip, uint64_t ripmask, int tailsz);
uint32_t fu32(float f);
uint64_t du64(double f);
uint64_t f32_to_f64(uint32_t x);
int64_t f32_rint_i64(uint32_t x, int *ov);
uint32_t f64_to_f32(uint64_t x);
uint32_t f32_cmp(float a, float b, int pred);
uint32_t f32_min(float a, float b);
uint32_t f32_max(float a, float b);
uint64_t f64_min(double a, double b);
uint64_t f64_max(double a, double b);
int sse_load_vec(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                        int lanes, uint32_t *d);
int sse_mem_fill(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint64_t addr,
                        uint8_t *tmp, int bytes);
int sse_store_vec(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                         int lanes, uint32_t *s, int n, uint64_t ripmask, int align);
int sse_move_load(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                         int reg, int lanes, int n, uint64_t ripmask, int align,
                         int zero_hi);
int sse_bitop(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                     int reg, int lanes, int which, int n, uint64_t ripmask);
int sse_arith_f32(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                         int reg, int lanes, int which, int n, uint64_t ripmask);
int sse_arith_f64(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                         int reg, int lanes, int which, int n, uint64_t ripmask);
int sse_cmp_f32(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                       int reg, int lanes, int pred, int n, uint64_t ripmask);
int sse_intop(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                     int reg, int lanes, int el, int which, int n, uint64_t ripmask);
int sse_shift_cnt(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                         int reg, int lanes, int el, int which, int n, uint64_t ripmask);
int sse_shufps(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                      int reg, int lanes, int imm, int pd, int n, uint64_t ripmask);
int sse_unpck(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                     int reg, int lanes, int el, int hi, int n, uint64_t ripmask);
int sse_pack(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                    int reg, int lanes, int which, int n, uint64_t ripmask);
int sse_hadd(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                    int reg, int lanes, int which, int isd, int n, uint64_t ripmask);
int media_pre(vfl_vcpu_t *v, uint8_t *buf, int *pos, int asz, int seg,
                     int rex_r, int rex_b, int rex_x, uint64_t ripmask, int tailsz,
                     int *mod, int *reg, vfl_ea_t *ea);
int sse_scalar_f32(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                          int reg, int which, int n, uint64_t ripmask);
int sse_scalar_f64(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                          int reg, int which, int n, uint64_t ripmask);
int vfl_simd_media(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                          int *pos, uint8_t op2, int pp, int osz, int asz, int seg,
                          int rep, int rex, int rex_w, int rex_r, int rex_b,
                          int rex_x, int lm, uint64_t ripmask);
int sse_scalar_store(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, vfl_ea_t *ea,
                            uint8_t *data, int size, int n, uint64_t ripmask);
int media_arith(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                       int *pos, uint8_t op2, int pp, int asz, int seg,
                       int rex, int rex_r, int rex_b, int rex_x,
                       int lm, uint64_t ripmask);
int media_int(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                     int *pos, uint8_t op2, int pp, int asz, int seg,
                     int rex, int rex_r, int rex_b, int rex_x,
                     int lm, uint64_t ripmask);
int simd_38(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                   int *pos, int pp, int asz, int seg, int rex, int rex_r,
                   int rex_b, int rex_x, int lm, uint64_t ripmask);
int simd_3a(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                   int *pos, int pp, int asz, int seg, int rex, int rex_r,
                   int rex_b, int rex_x, int lm, uint64_t ripmask);
void vfl_round_f32(float *r, float x, int mode);
void vfl_round_f64(double *r, double x, int mode);
int vfl_vex_more2(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                         int *pos, int op, int pp, int W, int L, int lanes, int s1,
                         int reg, vfl_ea_t *ea, int n, uint64_t ripmask);
int vfl_vex38_3a(vfl_vm_t *vm, vfl_vcpu_t *v, vfl_run_t *run, uint8_t *buf,
                        int *pos, int mmmmm, uint8_t op3, int vvvv, int L, int pp,
                        int W, int asz, int seg, int rxb, int rxr, int rxx,
                        uint64_t ripmask);

static __attribute__((always_inline)) inline long double f80_round(long double x) {
    long double r = x;

    __asm__ volatile ("frndint" : "+t"(r));
    return r;
}

#endif
