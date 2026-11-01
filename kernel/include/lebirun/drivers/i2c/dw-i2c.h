#ifndef DW_I2C_H
#define DW_I2C_H

#include <stdint.h>

typedef struct {
    uint64_t phys;
    uint64_t base;
    uint64_t t;
    const uint8_t *w;
    uint8_t *r;
    uint32_t wn;
    uint32_t ws;
    uint32_t rn;
    uint32_t ri;
    uint32_t rg;
    uint16_t txd;
    uint16_t rxd;
    uint16_t tar;
    uint8_t fast;
    uint8_t on;
} dw_i2c_t;

extern dw_i2c_t *dw_i2c_buses;
extern uint32_t dw_i2c_count;

uint32_t dw_i2c_init(void);
void dw_i2c_release(void);
int dw_i2c_xfer(dw_i2c_t *c, uint16_t addr, uint8_t fast, const uint8_t *w,
                uint32_t wn, uint8_t *r, uint32_t rn);
int dw_i2c_begin(dw_i2c_t *c, uint16_t addr, uint8_t fast, const uint8_t *w,
                 uint32_t wn, uint8_t *r, uint32_t rn);
int dw_i2c_pump(dw_i2c_t *c);
void dw_i2c_abort(dw_i2c_t *c);
void dw_i2c_delay(uint64_t ms);

#endif
