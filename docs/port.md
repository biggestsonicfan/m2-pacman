# The port in detail

MAME's `pacman` driver, ported to the i960: the Z80 is interpreted by `src/m2_z80.h`, and
`src/pacman_hw.h` is the rest of the board (memory map, IM 2 vblank IRQ, inputs, the
tilemap + 8 sprites, both PROMs). The 224x288 portrait screen sits cell-aligned on the
System 24 tile plane (m2-sdk's `m2_tilefb.h`), and each frame only the changed cells are
copied to char RAM. Like any sfight-based program it boots the SHARC with Sonic the
Fighters' firmware (`cpres1.h`/`cpres2.h` in m2-sdk's `src/`, see
[The SHARC firmware](#the-sharc-firmware)) and commits an empty GEO frame each frame, so
the real geometrizer (`M2_HLE_GEO_OFF`) runs alongside it; Pac-Man itself draws no
polygons. `-DPAC_NO_SHARC` (or missing firmware headers) leaves the SHARC out. It has no
cocktail flip.

This was the Pac-Man section of m2-sdk's README until the port moved here.

```sh
python3 tools/pacrom.py path/to/pacman.zip    # -> src/pacman_roms.h  (Namco data: git-ignored)
python3 tools/pacrom.py path/to/puckman.zip   # -> src/puckman_roms.h (for src/puckman.c)
# optional, recommended: statically recompile the game's code (see "Static recompilation")
cc -O2 -Isrc -o pactrace tools/pactrace.c && ./pactrace > trace.txt
python3 tools/z80recomp.py src/pacman_roms.h trace.txt src/pacman_recomp.h
cmake -G "Unix Makefiles" -B build -DM2_GAME=pacman   # or puckman, pacman_web
make -C build -j2
```

(For Puck Man: build pactrace with `-DPAC_ROMS='"puckman_roms.h"'` and write
`src/puckman_recomp.h`.)

Without the ROM header it builds a homebrew board test instead (`src/pactest_rom.h`,
made by `tools/pactest.py` from m2-sdk's font, all original data). Inputs: P1/P2 sticks,
COIN1/2, START1/2, SERVICE = credit.

- **Speed (MAME, real ROMs):** attract and gameplay hold 100% of real Pac-Man speed
  (uncapped, ~410% with the SHARC and sound running, recompiled). Boot, from program
  start to the attract loop, takes 9.8 s against 9.0 s on the real board (0.6 s of it is
  SDK/SHARC setup; the sound waveforms upload in the background); interpreted it is 14.5 s. The game spends most of each frame in
  a wait-for-vblank loop (`ld hl,(nn) / ld a,(hl) / and a / jp m`, 0x238D), and the
  core's `Z80_JP_TAKEN` hook ends the Z80's slice there (idle skip, found by byte pattern
  at reset). When a frame overruns its vblank the next one skips drawing (at most three
  in a row; gameplay never overruns). The core keeps B-L/A/F out of a memory array
  (`z80_getr`/`z80_setr` for the few run-time-indexed ops), so GCC can hold them in i960
  registers. The panel on the left shows the rate; `-DPAC_BENCH` removes the vblank cap.
  Pac-Man runs at 60.61 Hz and the Model 2 at 57.52 Hz, so about every 19th vblank runs
  two Pac-Man frames (drawing only the second) and game time matches the real board.
  Not tried on silicon.
- **Static recompilation:** `tools/pactrace.c` runs the board on the host (boot, attract,
  coins, a random stick) and lists every instruction address the Z80 executes (about 4200).
  `tools/z80recomp.py` turns each into C with its operands and PC as constants, using the
  core's own inline helpers and cycle counts, in one switch in address order: a case falls
  through to the next when the PC lands there, anything else goes back to the switch, and
  an address outside the trace, or a form it does not translate (DAA, EI, HALT, EX, block
  moves, IXH/IXL: 36 of ~4200), runs through the interpreter. So the trace decides speed,
  not behaviour: 20000 frames of pseudo-random play (`pac_frame` + the Z80 registers, RAM
  and sound registers hashed every frame) match the interpreter exactly, for both sets.
  The generated `src/<set>_recomp.h` is derived from the ROM, so it is git-ignored; the
  build uses it when present (`-DPAC_NO_RECOMP` to leave it out). It is big: the program
  ROM goes from ~240 KB to ~750 KB of the 1 MB (bus writes other than work RAM are
  one out-of-line `pac_wr_slow`, not inlined at every site), and cc1 needs ~650 MB for ~100 s.
- **Colours:** a tile-palette write goes through the colour-translation table (row = the
  5-bit channel, pen 0x40, MAME `palette_w`). The STF table `m2_init` builds saturates
  that column, so `pacman.c` rewrites it as a linear ramp before loading its palette.
  Without that, mid-tone colours (the blue maze) come out white.
- **Sound:** the sound board's 68000 normally runs the game's own driver, a MIDI synth
  for STF's music and effects that takes nothing raw. So `snd/scsp_passthru.s` replaces
  it: a 304-byte 68000 program that applies SCSP register and sound RAM writes the i960
  sends down the sound UART (`src/m2_scsp.h`; a ping first confirms it is there, so a
  game's driver never gets them as notes). It sleeps in `STOP` and wakes on the SCSP's
  MIDI interrupt, so an emulator spends nothing on it between bytes (polling cost MAME
  ~16% of its host time). Pac-Man uploads the eight 32-sample waveforms
  from the `1m` PROM, loops them on SCSP slots 0-2 and sets each voice's pitch, level and
  waveform from the WSG registers every frame. Checked in MAME against a host synthesis
  of the same register stream (MAME `namco.cpp`'s model): the siren sweeps 392-914 Hz
  against 400-913 Hz with the same 0.417 s period, and the intro tune's note content
  correlates 0.996. Without the passthrough program the game runs silent ("NO SOUND").
- **Lockstep with MAME's own Pac-Man:** `tools/lockstep/run.sh` plays one input script
  (a coin, a start and a pseudo-random stick) into MAME's `pacman` driver and into the i960
  port under MAME's Model 2 driver (`EPROMS=roms/pacman_web`: the three EPROMs over a stock
  `sfight` set, as a board or Pinboard runs it), and `compare.py` checks everything, frame by
  frame. Over 6000 frames (two games):
  - **Memory** (0x4000-0x4FFF and the sprite registers): 5992 frames byte-exact; the other 8
    differ in one or two bytes for one frame (the instruction astride the frame edge:
    MAME's Z80 is cycle-stepped, so at its frame end that one is part done).
  - **The Z80's registers**, all of them (AF BC DE HL, the alternate set, IX IY SP PC, I, R,
    IM, IFF1/2, HALT), taken as each IRQ is accepted (an instruction boundary on both: the
    port's `Z80_IRQ_HOOK`, a tap on MAME's vector read): identical at all 5707 IRQs.
  - **The board**: the 74LS259 latch (IRQ enable, sound enable, flip, lamps, coin lines),
    the IRQ mask and IM 2 vector, and all 32 WSG registers: identical in all 6000 frames,
    and MAME's decoded voices (frequency, volume, waveform) equal the port's decode.
  - **What the SCSP plays**: every register write the sound board makes to slots 0-2 is
    logged; the 2854 pitch/level/waveform writes the WSG state asks for all arrive, the
    same values in the same order, 4-64 ms (mean 8.5) after their frame starts (they cross
    the sound UART at ~2 ms per write).
  - **The audio**: MAME's WSG against the port's SCSP, per frame with sound (2433): the
    port's trails by 11 ms; loudness correlates 0.970, the spectra match (median cosine
    1.000, >= 0.9 in 92% of frames).
  - **Pictures** (every 60th): 94 of 100 identical; the other 6 are frames the port did not
    draw (57.5 Hz display, frameskip in the boot test), each exactly MAME's picture from 1-3
    frames before.

  Getting there took matching MAME in the core and board: registers reset to 0 (IX/IY
  FFFF), a level-held INT line cleared by the game's latch (`Z80_EXT_IRQ`), whole 4-cycle
  NOPs in HALT, the vblank IRQ seen by an instruction that ends one cycle before the frame
  edge, an idle skip that fast-forwards whole passes of the wait loop only while the task
  queue really is empty (it used to be able to hold a task over a frame), and R counted
  exactly (one per M1: two for prefixed ops, one per HALT NOP and IRQ acknowledge, bit 7
  kept apart; the recompiler adds each op's count). It also found a sprite-restore bug (a
  stray pixel for a frame now and then).
- **Tests:** `tools/z80test/z80test.c` runs zexdoc (67/67 pass) against the core on the
  host, and `tools/pachost.c` runs the whole board on the host and writes a PPM.
- **In the browser:** `src/pacman_web.c` is Pac-Man without the SHARC boot. A booted SHARC
  keeps MAME emulating its firmware's loop every frame, and Pac-Man gives it no work, so
  leaving it out makes MAME itself ~2.7x faster (native MAME: 60% -> 160% of real time).
  It also waits for vblank on `sinr` instead of a tight poll (`PAC_IDLE_SINR`): MAME's
  i960 core charges it 406 cycles for one host `sin()`, so the idle i960 costs MAME ~100x
  less (native MAME +20%); it needs the i960 FPU, so not for m2emulator. It runs at full
  speed in a web (Emscripten) MAME built with the Model 2 driver (`emmake make
  SUBTARGET=m2 SOURCES=src/mame/sega/model2.cpp`), with `m2_load.lua`, `game.bin` and
  `scsp_passthru.bin` in `/files` and sfight/schamp/segabill in `/roms`. `m2_load.lua`
  prints MAME's own speed to the log every ~5 s: below 100% the host can't keep up and
  the sound breaks up. Web MAME's audio backend opened the browser's audio at the
  device rate (44.1 kHz here) while MAME sends 48 kHz, which kept its buffer full
  (~0.45 s behind) and dropping samples; the fix is in the MAME fork
  (`biggestsonicfan/mame` branch `web-audio-latency`, `src/osd/modules/sound/js_sound.js`).
- **Standalone (EPROMs):** a Pac-Man build also writes the sound program EPROM
  `roms/<game>/epr-19021.31` (`tools/snd2rom.py`: `snd/scsp_passthru.bin` in a 512 KB image, words
  low byte first like the original dump, the rest 0xFF). So sfight with three chips replaced,
  `epr-19001.15` + `epr-19002.16` (program) and `epr-19021.31` (sound), runs Pac-Man with
  sound and no loader; checked in MAME (m2sharc, HLE geometry and `M2_HLE_GEO_OFF`) against
  the WSG reference (siren 399-915 Hz vs 400-913 Hz, intro note correlation 0.997). With
  only the two program EPROMs swapped it runs silent ("NO SOUND"): the i960 has no path to
  the sound board but the MIDI bytes into its 68000 program. Burn the `pacman` build (not
  `pacman_web`, whose `sinr` idle needs the i960 FPU: fine on hardware, not on m2emulator).
  Not tried on a real board.
- **Run on a stock sfight romset:** the build also writes `roms/<game>/game.bin`, and
  `tools/m2_load.lua` (an `-autoboot_script`) copies it over the program ROM region, and
  `snd/scsp_passthru.bin` over the sound program (`$M2_SOUND_BIN`, or
  `/files/scsp_passthru.bin`), then resets. That's how Pinboard's web MAME launches it:
  `M2_GAME_BIN=roms/pacman/game.bin M2_SOUND_BIN=snd/scsp_passthru.bin mame sfight
  -autoboot_script tools/m2_load.lua`.
- **Why interpret?** The Model 2 has an i960, and its sound board a 68000 and the SCSP.
  The SCSP passthrough above is the first piece of handing a guest's work to a matching
  Model 2 part; a guest CPU matching the i960 or 68000 is not built yet. Pac-Man's Z80 has
  no match here, so it goes through the interpreter; the machine layer (`pacman_hw.h`)
  only sees the bus hooks, not how the CPU runs.

## The SHARC firmware

`src/pacman.c` boots the SHARC with Sonic the Fighters' own programs, `cpres1.h`
(`cpres_data`) and `cpres2.h` (`cpres_data2`) in m2-sdk's `src/` (git-ignored there: Sega
data). m2-sdk's `docs/firmware-extraction.md` extracts them from the ROM. Or assemble them
from source: [stf-sharc](https://github.com/biggestsonicfan/stf-sharc) is the annotated
disassembly of both images, and it reassembles byte for byte (stf-tools' `test-cpres.mjs`
checks the linked `.exe` against the ROM). `tools/sharc2h.py` turns its linked
`cpres1.exe` / `cpres2.exe` straight into the headers. It reads the `seg_pmco` section and
byte-reverses each 48-bit word into ROM order:

```sh
python3 tools/sharc2h.py ../stf-sharc/cpres1.exe ../m2-sdk/src/cpres1.h cpres_data
python3 tools/sharc2h.py ../stf-sharc/cpres2.exe ../m2-sdk/src/cpres2.h cpres_data2
```

The arrays match the ROM extract value for value (14862 and 9351 halfwords); only the
comment line differs.
