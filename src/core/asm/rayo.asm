; =============================================================================
; Rayo -- a k7zx turbo loader (added by the k7zx 5.0 port; not part of k7zx 4.3)
; =============================================================================
;
; Signal: after a pilot (cycles of 8+8 samples) and a sync (a 2-sample run,
; then an 8-sample run), every *full cycle* carries two bits: its length is
; Pmin+s samples, s = 0..3 (Pmin = 3 for "2.25", 4 for "2.75"), short half
; first.  Full cycles, not half cycles, because the EAR input's threshold
; offset skews each half by tens of T on real hardware while the full cycle
; is barely affected.  The cycle is measured between edges of one kind; which
; kind is learnt from the sync, so an inverted signal loads too.  A cycle of
; Pmin+5 samples ends a segment.
;
; Timing: the classic Raudo trick.  `in l,(c) / jp (hl)` with BC = $FFFE (no
; keyboard rows selected, so a key press cannot disturb it) jumps to page H at
; offset $BF (EAR low) or $FF (EAR high).  Four stubs at those offsets in two
; pages form the state machine:
;
;     page A ($FD)          page B ($FE)
;     W_X  wait in run X     W_Y  wait in run Y
;     MID  inc h -> page B   DEC  jp dec: one cycle has ended
;
; Each poll is 16 T and adds 3 to R, so `ld a,r` at the end of a cycle is its
; length; R is reset with `ld r,a` by every handler.  The table in page B
; ($FE04..$FE7F) maps R straight to the 2-bit value (permuted per tape so the
; commonest pair gets the shortest cycle), to 1 (the sentinel produced by the
; sync cycle) or to $80 (terminator / anything invalid).
;
; Every path from one decision to the next poll is exactly 136 T, a multiple
; of 8, so on a 48K the contended ULA port locks to a no-wait phase after at
; most one delay per scanline.
;
; Bytes are assembled MSB first with a sentinel bit in D; every byte is XORed
; into C' and stored at (HL').  A segment ends on the terminator, and must end
; with the sentinel alone, C' = 0 (the last byte is the XOR of the others) and
; HL' at the expected end -- otherwise "R Tape loading error".
;
; Afterwards, optionally, the in-place LZ decompressor (dzx) expands the data.
; =============================================================================

        DEVICE NOSLOT64K

        IFNDEF PG
PG      EQU $FD
        ENDIF

PA      EQU PG*256
PB      EQU (PG+1)*256
S1      EQU PA+$BF              ; 5 bytes
S2      EQU PA+$FF              ; 5 bytes (spills into PB+0..3)
S3      EQU PB+$BF              ; 3 bytes
S4      EQU PB+$FF              ; 3 bytes (spills into PB+$100..$101)
TABLE   EQU PB                  ; entries 4..127

MINP    EQU 9                   ; pilot run, in 39 T edge-loop counts
MAXP    EQU 26

; -----------------------------------------------------------------------------
; Area 4: $FF02..$FFEF -- entry, segments, pilot/sync, exit
; -----------------------------------------------------------------------------
        ORG PB+$102
area4:
main:   di
        ld (savesp+1),sp
        ld sp,0                 ; private stack at the top of RAM
        exx
        push hl                 ; BASIC needs H'L' back
        exx
        ld bc,$fffe
        ld ix,segtab
segloop:
        ld a,(ix+4)
        inc a
        jr z,segdone            ; $FF ends the table
        exx
        ld l,(ix+0)
        ld h,(ix+1)
        ld c,0
        exx
        call sync               ; loads one segment, returns at its terminator
        ld de,5
        add ix,de
        jr segloop
segdone:
decomp: ld a,0                  ; patched: 1 = run the decompressor
        or a
        jr z,finish
dsrc:   ld hl,0                 ; patched: compressed data
ddst:   ld de,0                 ; patched: destination
        call dzx
finish: ld a,$08                ; border black (MIC bit kept set)
        out ($fe),a
        exx
        pop hl
        exx
savesp: ld sp,0
eiop:   ei                      ; patched to NOP for snapshots
jpop:   jp 0                    ; patched: JP usr, or RET
errx:   exx
error:  ld sp,(savesp+1)
        exx
        ld hl,(PB+$100-2)       ; H'L' saved at $FFFE
        exx
        ei
        rst 8
        db $1a                  ; R Tape loading error

; --- one segment: find the pilot and the sync, then run the state machine ---
sync:   ld h,$0a                ; border colour for the pilot stripes (+MIC)
        in a,(c)
        ld l,a
.p0:    ld d,24                 ; 24 consecutive pilot runs
.p1:    call edge
        ld a,e
        cp MINP
        jr c,.p0
        cp MAXP
        jr nc,.p0
        dec d
        jr nz,.p1
.p2:    call edge               ; pilot until a short run: the sync
        ld a,e
        cp MAXP
        jr nc,.p0
        cp MINP
        jr nc,.p2
        ; The short run has just ended; L = level of the long sync run.  The
        ; cycles are measured between edges *into* the short run's level.
        ld a,l
        inc a
        jp z,sync_inv           ; long run high: inverted signal
        ld hl,$68ed             ; normal: X = high, Y = low
        ld (S2),hl              ;   W_X at (A,$FF)
        ld (S3),hl              ;   W_Y at (B,$BF)
        ld (S1+2),hl            ;   MID at (A,$BF)
        ld hl,$0024
        ld (S1),hl
        ld a,$e9
        ld (S2+2),a
        ld (S3+2),a
        ld (S1+4),a
        ld a,$c3                ;   DEC at (B,$FF)
        ld (S4),a
        ld hl,dec
        ld (S4+1),hl
        jp sync_go

; --- segment table (patched): dest, end, flags ($FF terminates) -------------
segtab: dw 0,0
        db 0
        dw 0,0
        db $ff
        ds 10,$ff
area4end:
        ASSERT area4end <= PB+$1f0, area 4 overflow

; -----------------------------------------------------------------------------
; Area 3: $FEC2..$FEFE -- the per-cycle handlers (both 136 T to the next poll)
; -----------------------------------------------------------------------------
        ORG PB+$c2
area3:
dec:    ld a,r                  ; cycle length
        ld l,a
        ld e,(hl)               ; -> 0..3, 1 (sentinel) or $80
        ld a,d
        add a,a
        add a,a
        jr c,full               ; the sentinel fell out: 4th pair of a byte
        or e
        jp m,term
        ld d,a
        ld a,e                  ; loading stripes from the pair's value
        or 8
        out ($fe),a
        ld e,0                  ; padding to 136 T
        nop
presetn: ld a,0                 ; patched: R preset
        ld r,a
        dec h
        in l,(c)
        jp (hl)
full:   or e
        exx
        ld (hl),a
        inc hl
        xor c
        ld c,a
        inc de                  ; padding to 136 T
        exx
        ld d,1
presetf: ld a,0                 ; patched: R preset (same value)
        ld r,a
        dec h
        in l,(c)
        jp (hl)
area3end:
        ASSERT area3end <= PB+$ff, area 3 overflow

; -----------------------------------------------------------------------------
; Area 2: $FE80..$FEBE -- the inverted-polarity stubs, and the common entry
; into the state machine
; -----------------------------------------------------------------------------
        ORG PB+$80
area2:
sync_inv:   ld hl,$68ed             ; inverted: X = low, Y = high
        ld (S1),hl              ;   W_X at (A,$BF)
        ld (S4),hl              ;   W_Y at (B,$FF)
        ld (S2+2),hl            ;   MID at (A,$FF)
        ld hl,$0024
        ld (S2),hl
        ld a,$e9
        ld (S1+2),a
        ld (S4+2),a
        ld (S2+4),a
        ld a,$c3                ;   DEC at (B,$BF)
        ld (S3),a
        ld hl,dec
        ld (S3+1),hl
sync_go:    ld d,0                  ; the sync cycle decodes to the sentinel
presets: ld a,0                 ; patched: R preset for the sync cycle
        ld r,a
        ld h,PG+1               ; we are in the long run Y: wait in W_Y
        in l,(c)
        jp (hl)

area2end:
        ASSERT area2end <= PB+$bf, area 2 overflow

; -----------------------------------------------------------------------------
; Area 1: $FDC4..$FDFE -- end of a segment
; -----------------------------------------------------------------------------
        ORG PA+$c4
area1:
term:   cp $84                  ; terminator on a byte boundary?
        jp nz,error
        exx
        ld a,c                  ; XOR of everything, checksum included
        or a
        jp nz,errx
        ld a,l
        cp (ix+2)
        jp nz,errx
        ld a,h
        cp (ix+3)
        jp nz,errx
        exx
        ret
area1end:
        ASSERT area1end <= PA+$ff, area 1 overflow

; -----------------------------------------------------------------------------
; Area 0: $FD40..$FDBE -- the edge timer (pilot search), then the in-place
; LZ decompressor (sent only when the tape is compressed).  dzx: HL = source,
; DE = destination; the format is described in src/core/lz.h.  The timer comes
; first: the compressed stream may overhang the end of its destination by a
; few bytes, and those may land on it -- it is not needed once the data starts.
; -----------------------------------------------------------------------------
        ORG PA+$40
area0:
; --- wait for an edge; E = length in 39 T steps, L = new level --------------
edge:   ld e,0
.e1:    inc e
        jr z,.to
        in a,(c)
        cp l
        jr z,.e1
        ld l,a
        ld a,h                  ; pilot stripes
        xor 7
        ld h,a
        out ($fe),a
        ret
.to:    dec e
        ret

dzxstart:
dzx:    ld a,$80
.lit:   call .gam               ; literal run
        ldir
        call .bit
        jr c,.new
        call .gam               ; repeat the last offset
.copy:  push hl
.off:   ld hl,0                 ; -offset (written by .new)
        add hl,de
        ldir
        pop hl
        call .bit
        jr nc,.lit
.new:   call .gam               ; offset high byte + 1
        inc b
        dec b
        ret nz                  ; 256: end of stream
        ex af,af'
        ld a,c
        neg                     ; -(hi+1) = ~hi
        ld (.off+2),a
        ld a,(hl)
        inc hl
        cpl
        ld (.off+1),a
        ex af,af'
        call .gam               ; length - 1
        inc bc
        jr .copy
.gam:   ld bc,1                 ; interlaced Elias gamma
.g1:    call .bit
        ret c
        call .bit
        rl c
        rl b
        jr .g1
.bit:   add a,a
        ret nz
        ld a,(hl)
        inc hl
        rla
        ret
area0end:
        ASSERT area0end <= PA+$bf, area 0 overflow

; -----------------------------------------------------------------------------
; Boot code: runs inside the BASIC line at 23781.  Copies the areas into place,
; builds the table from run lengths, and jumps to main.  The data that follows
; it (chunks, runs) is appended by the converter.
; -----------------------------------------------------------------------------
        ORG 23781
boot: ld hl,bootdata
.c:     ld e,(hl)
        inc hl
        ld d,(hl)
        inc hl
        ld a,d
        or e
        jr z,.t
        ld c,(hl)
        inc hl
        ld b,0
        ldir
        jr .c
.t:     ld de,TABLE+4
.r:     ld b,(hl)
        inc hl
        inc b
        dec b
        jp z,main
        ld a,(hl)
        inc hl
.f:     ld (de),a
        inc e
        djnz .f
        jr .r
bootdata:

        SAVEBIN "rayo_top.bin", PA, $300
        SAVEBIN "rayo_boot.bin", boot, bootdata-boot
