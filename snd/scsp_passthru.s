| scsp_passthru.s — Model 2 sound-board program: the i960 drives the SCSP directly.
|
| Replaces the game's 68000 sound program (the :audiocpu ROM at 0x600000; tools/m2_load.lua
| can load it over a stock set). It does nothing on its own: it reads the bytes the i960
| sends down the sound UART (they arrive in the SCSP's MIDI input FIFO) and applies them.
| The i960 side is src/m2_scsp.h. Packets, each a command byte (bit 7 set) then 7-bit bytes,
| most significant first:
|
|   0x90 a1 a0 v2 v1 v0      SCSP register write:  word at 0x100000 + (a & 0xFFE) = v
|   0xA0 a2 a1 a0 v2 v1 v0   sound RAM write:      word at (a & 0x7FFFE)          = v
|   0xF0                     ping: answers 0x5A on MIDI out (back to the i960's UART),
|                            so the i960 knows this program, not a game's, is listening
|
| A command byte in the middle of a packet starts a new one, so a lost byte costs one packet.
| The 68000 sleeps in STOP; a byte in the MIDI FIFO raises the SCSP's MIDI interrupt (SCIEB
| bit 3, level 3 through SCILV0/1), whose handler drains the FIFO and goes back to sleep. A
| stopped 68000 costs an emulator nothing, where a polling one costs it every instruction
| (MAME sound/scsp.cpp CheckPendingIRQ: the line is held while the FIFO has bytes and drops
| when a read empties it).
|
|   m68k-linux-gnu-as -m68000 -o scsp_passthru.o scsp_passthru.s
|   m68k-linux-gnu-ld -Ttext=0x600000 --oformat binary -o scsp_passthru.bin scsp_passthru.o

        .text
        .globl  _start
vectors:
        .long   0x00080000              | reset SSP: top of the 512 KB sound RAM
        .long   _start                  | reset PC (MAME copies these 8 bytes + 8 more to RAM 0)
        .long   _start, _start          | bus / address error: restart

_start:
        move.w  #0x2700, %sr            | interrupts off while setting up
        lea     0x80000, %sp            | a restart from an error vector drops its frame
        move.b  #0x1F, 0x400001         | sound board control: bit 4 on, LEDs (bits 0-3) off.
                                        | STF's program keeps bit 4 set while it runs and
                                        | clears it only in its error-blink loop: the
                                        | board's audio output. MAME: sample banking only.
        move.w  #0x1500, %d7            | let the board settle before touching the SCSP:
0:      ror.l   #8, %d1                 | STF spins the same ~30 ms first (0x60122A)
        ror.l   #8, %d1
        dbra    %d7, 0b

        lea     0x100000, %a5           | SCSP registers
        move.w  #0x0300, 0x400(%a5)     | MEM4MB (512 KB of 4 Mbit DRAM), DAC18B, MVOL 0
                                        | until every slot is silent (STF: 3 to 0x400)
        clr.w   0x41E(%a5)              | SCIEB: no SCSP interrupt sources yet
        clr.w   0x42A(%a5)              | MCIEB: none to the main CPU
        move.w  #0x07FF, 0x422(%a5)     | SCIRE: clear anything pending

        | All 32 slots silent, every register set. MAME clears them at reset; a board's
        | power-up values are unknown, and a stray MDL (0x0E) or LFO depth (0x12) left in
        | a slot the i960 later keys on would modulate it. STF sets TL 0xFF, RR 0x1F, key
        | off (0x6013B6).
        move.l  %a5, %a0
        moveq   #31, %d7
1:      moveq   #15, %d6
2:      clr.w   (%a0)+                  | the slot's 16 registers (0x16 too: no direct or
        dbra    %d6, 2b                 | DSP output)
        move.w  #0x001F, 0x0A-0x20(%a0) | RR max
        move.w  #0x00FF, 0x0C-0x20(%a0) | TL 0xFF: silent
        dbra    %d7, 1b
        move.w  #0x1000, (%a5)          | KYONEX: apply (all off)

        | The effect DSP: its program RAM (MPRO) comes up holding garbage, and a garbage
        | step with MWT set writes the ring buffer in sound RAM: from RBP x 8 KB, i.e.
        | over these vectors and the i960's waveforms at 0x1000 with RBP 0. Zero
        | COEF/MADRS/MPRO (0x700-0xBFF; a zero step writes nothing), and put the 8K-word
        | ring buffer at 0x70000-0x73FFF, clear of everything (STF loads its own effects
        | program here and puts its ring at 0x70000, 0x6013B0).
        lea     0x700(%a5), %a0
        move.w  #(0xC00-0x700)/4-1, %d7
3:      clr.l   (%a0)+
        dbra    %d7, 3b
        move.w  #0x0038, 0x402(%a5)     | RBL 0 (8K words), RBP 0x38

        move.b  #15, 0x401(%a5)         | MVOL 15 (STF: 15 to 0x401)

        | exception vectors live in sound RAM, which comes up holding garbage: every
        | vector restarts the program, except the autovectors (a bare RTE) and level 3
        | (the MIDI handler). STF copies a whole table there (0x60133A).
        lea     0x08, %a0               | bus error .. vector 63
        moveq   #64-2-1, %d7
4:      move.l  #_start, (%a0)+
        dbra    %d7, 4b
        lea     0x60, %a0               | spurious, autovectors 1-7 (0x60-0x7C)
        moveq   #7, %d7
5:      move.l  #spurious, (%a0)+
        dbra    %d7, 5b
        move.l  #midi_irq, 0x6C         | level 3

        moveq   #0, %d5                 | d5 = command (0 = waiting for one)
        moveq   #0, %d6                 | d6 = data bytes still expected
        moveq   #0, %d2                 | d2 = accumulator
        moveq   #0, %d4                 | d4 = address

        moveq   #3, %d7                 | empty the 4-byte MIDI input FIFO of anything that
6:      tst.b   0x405(%a5)              | came before us (STF reads it 4 times, 0x601404)
        dbra    %d7, 6b
        move.w  #0x0008, 0x424(%a5)     | SCILV0 bit 3 \  MIDI interrupt -> level 3
        move.w  #0x0008, 0x426(%a5)     | SCILV1 bit 3 /   (STF: the same level, 0x6013DA)
        clr.w   0x428(%a5)              | SCILV2
        move.w  #0x0008, 0x422(%a5)     | SCIRE: drop a MIDI interrupt the flush left
        move.w  #0x0008, 0x41E(%a5)     | SCIEB: MIDI input interrupt on

idle:   stop    #0x2000                 | sleep, interrupts enabled
        bra.s   idle

spurious:
        rte

| ---- level 3: drain the MIDI FIFO through the packet parser (state lives in d2/d4-d6)
midi_irq:
poll:   move.w  0x420(%a5), %d0         | SCIPD
        btst    #3, %d0                 | MIDI input pending?
        beq     done
        moveq   #0, %d0
        move.b  0x405(%a5), %d0         | pop one byte
        bpl.s   data
        | a command byte
        moveq   #0, %d2
        moveq   #0, %d6
        move.b  %d0, %d5
        cmp.b   #0x90, %d0
        bne.s   1f
        moveq   #5, %d6
        bra     poll
1:      cmp.b   #0xA0, %d0
        bne.s   2f
        moveq   #6, %d6
        bra     poll
2:      moveq   #0, %d5
        cmp.b   #0xF0, %d0
        bne     poll                    | unknown command: ignore until the next one
        move.w  #0x005A, 0x406(%a5)     | ping -> MOBUF
        bra     poll

data:   tst.b   %d6
        beq     poll                    | stray data byte
        lsl.l   #7, %d2
        or.b    %d0, %d2
        subq.b  #1, %d6
        cmp.b   #0x90, %d5
        bne.s   ram
        cmp.b   #3, %d6                 | 2 address bytes in
        bne.s   1f
        move.l  %d2, %d4
        moveq   #0, %d2
        bra     poll
1:      tst.b   %d6
        bne     poll
        and.w   #0x0FFE, %d4
        move.w  %d2, 0(%a5, %d4.w)      | SCSP register write
        moveq   #0, %d5
        bra     poll

ram:    cmp.b   #3, %d6                 | 3 address bytes in
        bne.s   1f
        move.l  %d2, %d4
        moveq   #0, %d2
        bra     poll
1:      tst.b   %d6
        bne     poll
        and.l   #0x7FFFE, %d4
        move.l  %d4, %a0
        move.w  %d2, (%a0)              | sound RAM write
        moveq   #0, %d5
        bra     poll

done:   rte
