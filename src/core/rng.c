#include "rng.h"

/* Marsaglia xorshift on two 16-bit words, triple (5,3,1): 32 bits of state, period
 * 2^32-1 (checked exhaustively on the host), still only 16-bit shifts. The old 16-bit
 * state made sector streams run into each other: about 20% of boards were exact
 * repeats of some other (seed, sector), often an earlier sector of the same run. */
uint16_t rng_state = 1;   /* y: the last output */
static uint16_t rng_x;

void rng_seed(uint16_t seed)
{
    rng_x = 0;
    rng_state = seed ? seed : 0xACE1u;
}

uint16_t rng_next(void)
{
    uint16_t t = (uint16_t)(rng_x ^ (uint16_t)(rng_x << 5));
    rng_x = rng_state;
    rng_state = (uint16_t)(rng_state ^ (uint16_t)(rng_state >> 1) ^ t ^ (uint16_t)(t >> 3));
    return rng_state;
}

uint8_t rng_range(uint8_t n)
{
    /* multiply-high on the top byte: avoids division, bias is negligible for gameplay */
    uint16_t r = (uint16_t)(rng_next() >> 8);
    return (uint8_t)((r * (uint16_t)n) >> 8);
}

static uint16_t feistel_f(uint16_t v)
{
    v = (uint16_t)(v * 0x6F4Du + 0x3A9Bu);
    return (uint16_t)(v ^ (uint16_t)(v >> 7));
}

static void feistel(uint16_t a, uint16_t b)
{
    uint16_t t;
    uint8_t i;
    for (i = 0; i < 4; i++) {
        t = (uint16_t)(a ^ feistel_f((uint16_t)(b + i)));
        a = b;
        b = t;
    }
    rng_x = a;
    rng_state = b;
}

/* Seed the full 32-bit state from two 16-bit values through a 4-round Feistel network.
 * That is a bijection, so every (a, b) pair - every (run seed, sector) - gets a state
 * of its own, well mixed. The one pair that maps to all-zero (a state xorshift can't
 * leave) takes the state of (a, 0) instead: sector 0 is never played, so no pair the
 * game uses shares it. */
void rng_seed2(uint16_t a, uint16_t b)
{
    feistel(a, b);
    if (!rng_x && !rng_state) feistel(a, 0);
}
