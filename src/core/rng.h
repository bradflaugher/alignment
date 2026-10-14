#ifndef IB_RNG_H
#define IB_RNG_H
#include <stdint.h>
/* 32-bit xorshift over two 16-bit words. Deterministic across SDCC and gcc. */
extern uint16_t rng_state;
void rng_seed(uint16_t seed);
/* seed from two 16-bit values; every pair with b != 0 gets its own state (per-sector streams) */
void rng_seed2(uint16_t a, uint16_t b);
uint16_t rng_next(void);
/* uniform-ish value in [0, n) for n >= 1 (n <= 255) */
uint8_t rng_range(uint8_t n);
#endif
