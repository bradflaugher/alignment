/* ALIGNMENT - the core's one multiply (included by the files that need it: a
 * static copy each, so no call ever crosses a ROM bank). */
#ifndef AL_MUL8_H
#define AL_MUL8_H
#include <stdint.h>

/* 8 x 8 -> 16 bit multiply: SDCC would widen to a 16-bit multiply (slow on a DMG) */
#ifdef __SDCC
static uint16_t mul8(uint8_t a, uint8_t b) __naked
{
    (void)a;
    (void)b;
    __asm
        ld  hl, #0
        ld  c, a
        ld  b, #0
    00001$:
        srl e
        jr  nc, 00002$
        add hl, bc
    00002$:
        jr  z, 00003$
        sla c
        rl  b
        jr  00001$
    00003$:
        ld  b, h
        ld  c, l
        ret
    __endasm;
}
#else
static uint16_t mul8(uint8_t a, uint8_t b)
{
    return (uint16_t)(a * b);
}
#endif

#endif
