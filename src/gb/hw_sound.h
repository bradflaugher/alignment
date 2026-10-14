/* hw_sound.h - sound register access shim.
 *
 * All driver writes go through SND_W(reg, value) where reg is the low byte of
 * the I/O address (0x10 = NR10 ... 0x26 = NR52, 0x30..0x3F = wave RAM).
 *
 *  - Game Boy (SDCC / GBDK):  a direct store to 0xFF00+reg (constant reg
 *    compiles to a single `ld (0xFFxx),a`); <gb/hardware.h> provides the
 *    NRxx_REG names for anyone who prefers them.
 *  - Host unit tests (-DHOST_TEST):  every write lands in host_snd_regs[reg]
 *    (index = address - 0xFF00, so host_snd_regs[0x12] is NR12) and is also
 *    passed to host_snd_hook(reg, value) if set.  host_snd_src tells the hook
 *    who is writing: SND_SRC_MUSIC (driver / music) or SND_SRC_SFX.
 *  - SND_BANK_PUSH() / SND_BANK_POP(): map the (autobanked) music data ROM
 *    bank in / restore the caller's bank.  No-ops on the host.
 */
#ifndef HW_SOUND_H
#define HW_SOUND_H

#include <stdint.h>

#define SND_NR10 0x10
#define SND_NR11 0x11
#define SND_NR12 0x12
#define SND_NR13 0x13
#define SND_NR14 0x14
#define SND_NR21 0x16
#define SND_NR22 0x17
#define SND_NR23 0x18
#define SND_NR24 0x19
#define SND_NR30 0x1A
#define SND_NR31 0x1B
#define SND_NR32 0x1C
#define SND_NR33 0x1D
#define SND_NR34 0x1E
#define SND_NR41 0x20
#define SND_NR42 0x21
#define SND_NR43 0x22
#define SND_NR44 0x23
#define SND_NR50 0x24
#define SND_NR51 0x25
#define SND_NR52 0x26
#define SND_WAVE 0x30   /* 0x30..0x3F wave pattern RAM */

#define SND_SRC_MUSIC 0
#define SND_SRC_SFX   1

#ifdef HOST_TEST

extern uint8_t host_snd_regs[0x40];
typedef void (*host_snd_hook_t)(uint8_t reg, uint8_t val);
extern host_snd_hook_t host_snd_hook;
extern uint8_t host_snd_src;
void host_snd_write(uint8_t reg, uint8_t val);

#define SND_W(r, v)  host_snd_write((uint8_t)(r), (uint8_t)(v))
#define SND_SRC(s)   (host_snd_src = (uint8_t)(s))
#define SND_BANK_PUSH()
#define SND_BANK_POP()

#else /* Game Boy */

#include <gb/gb.h>
#include <gb/hardware.h>
#define SND_W(r, v)  (*(volatile uint8_t *)(0xFF00u + (uint8_t)(r)) = (uint8_t)(v))
#define SND_SRC(s)   ((void)0)

/* music_data.c is autobanked (#pragma bank 255).  sound.c stays in bank 0;
 * sound_tick() maps the data bank in once on entry and restores the
 * caller's bank on exit (safe from the VBL ISR).  SND_BANK_PUSH() declares
 * a local, so it must come first in the function body. */
BANKREF_EXTERN(music_data)
#define SND_BANK_PUSH() uint8_t snd_saved_bank = CURRENT_BANK; SWITCH_ROM(BANK(music_data))
#define SND_BANK_POP()  SWITCH_ROM(snd_saved_bank)

#endif

#endif
