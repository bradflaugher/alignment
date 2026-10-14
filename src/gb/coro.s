;; ALIGNMENT - a two-stack coroutine for the room generator.
;;
;; The generator is ordinary blocking C (src/core/gen.c). It runs on its own stack:
;; coro_resume() switches from the game's stack to the generator's, and the
;; generator's searches call coro_yield() between expansions to switch back. Each
;; side keeps its ROM bank (the game and the generator both live in switchable banks),
;; so the switch saves _current_bank and re-selects it on the MBC5.
;;
;; Interrupts stay enabled: SP always points at a valid stack, and an interrupt that
;; lands between the two bank writes restores the same bank we are about to select.

        .module coro

        .area   _DATA
_coro_main_sp::         .ds 2
_coro_gen_sp::          .ds 2
_coro_main_bank::       .ds 1
_coro_gen_bank::        .ds 1

        .area   _HOME

;; void coro_resume(void): run the generator until it yields
_coro_resume::
        push    bc
        push    de
        push    hl
        ld      a, (__current_bank)
        ld      (_coro_main_bank), a
        ld      (_coro_main_sp), sp
        ld      hl, #_coro_gen_sp
        ld      a, (hl+)
        ld      h, (hl)
        ld      l, a
        ld      sp, hl
        ld      a, (_coro_gen_bank)
        ld      (__current_bank), a
        ld      (0x2000), a
        pop     hl
        pop     de
        pop     bc
        ret

;; void coro_yield(void): back to the game
_coro_yield::
        push    bc
        push    de
        push    hl
        ld      a, (__current_bank)
        ld      (_coro_gen_bank), a
        ld      (_coro_gen_sp), sp
        ld      hl, #_coro_main_sp
        ld      a, (hl+)
        ld      h, (hl)
        ld      l, a
        ld      sp, hl
        ld      a, (_coro_main_bank)
        ld      (__current_bank), a
        ld      (0x2000), a
        pop     hl
        pop     de
        pop     bc
        ret
