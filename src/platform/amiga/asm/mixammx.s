; AMMX kernels for the sound mixer (Apollo 68080). Assembled with vasmm68k_mot -m68080.
;
; void magVolAMMX(short *frames, long groups, long factor)
;   frames: stereo 16-bit samples L,R,L,R...; groups: number of 8-byte groups (2 frames each)
;   factor: left volume << 16 | right volume, each 0..256 (8.8 fixed point, 256 = unchanged)
;   Each sample becomes (sample * volume) >> 8: PMUL88 keeps bits 23..8 of the 16x16 product,
;   four samples per instruction.
        section text,code
        xdef    _magVolAMMX
_magVolAMMX:
        movem.l d2-d3,-(sp)
        move.l  12(sp),a0               ; frames
        move.l  16(sp),d0               ; groups
        move.l  20(sp),d1               ; factor (L.w : R.w)
        move.l  d1,-(sp)
        move.l  d1,-(sp)
        load    (sp),d2                 ; d2 = L R L R
        addq.l  #8,sp
        bra.s   .start
.loop:  load    (a0),d3
        pmul88  d2,d3,d3
        store   d3,(a0)+
.start: dbra.l  d0,.loop
        movem.l (sp)+,d2-d3
        rts
