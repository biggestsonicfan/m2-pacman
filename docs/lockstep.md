# Lockstep against MAME's Pac-Man

The port is checked by running it next to MAME's own `pacman` driver on the same inputs
and comparing the two machines, frame by frame: memory, the Z80's registers, the board,
the sound chip's registers, what the SCSP is told to play, the audio, and the pictures.

## How

`tools/lockstep/`:

- `inputs.lua`: one input script for both: a coin and a start every 3000 frames, and a
  new pseudo-random stick direction every 17 frames.
- `ref.lua` runs in MAME's `pacman` driver. It replaces what the Z80 reads at IN0/IN1
  (a read tap), and after every frame records the RAM, the sprite registers and the
  board's state, read from MAME's own devices (their save-state items: the Namco WSG's
  `m_soundregs` and decoded voices, the 74LS259's `m_q`, the driver's `m_irq_mask` and
  `m_interrupt_vector`), plus every 60th picture and a WAV of the sound.
- `port.lua` runs in MAME's Model 2 driver on `sfight` with the three EPROMs. A write tap
  on the port's frame counter catches every emulated Pac-Man frame (also the two that
  share one Model 2 vblank), feeds the inputs, and records the same things from the
  port's own variables. It also logs every write the sound board makes to SCSP slots 0-2,
  and a WAV.
- **Registers are compared as each IRQ is accepted, not at the frame edge.** At a frame
  edge MAME's cycle-stepped Z80 is usually partway through an instruction, while the port
  stops between instructions. At IRQ acceptance (PC pushed, the IM 2 vector about to be
  read) both are exactly between instructions. The port snapshots its state there
  (`Z80_IRQ_HOOK` in `src/m2_z80.h`); on MAME's side a read tap on the vector word catches
  the Z80's acknowledge.
- `compare.py` compares it all; `run.sh` runs both machines headless and then compares.

```sh
MAME_PACMAN=mame-with-pacman MAME_M2=mame-with-model2 \
ROMS_PACMAN=dir-with-pacman.zip ROMS_M2=dir-with-sfight-schamp-segabill-zips \
GAME=build-dir EPROMS=roms/pacman_web LS_FRAMES=6000 LS_OUT=/tmp/lockstep \
bash tools/lockstep/run.sh
```

## Results: 6000 frames (two games), `sfight` + `roms/pacman_web`

| What | Result |
|---|---|
| **Z80 registers**, all 30 (AF BC DE HL, the alternate set, IX IY SP PC, I, R, IM, IFF1/2, HALT) | identical at all 5707 IRQs |
| **Board**: latch (IRQ enable, sound enable, flip, lamps, coin lines), IRQ mask, IM 2 vector | identical in all 6000 frames |
| **WSG**: all 32 sound registers, and the voices MAME decodes from them (frequency, volume, waveform) | identical in all 6000 frames |
| **SCSP**: every register write the WSG state asks for | 2854 of 2854 arrive, the same values in the same order, 4-64 ms (mean 8.5) after their frame starts |
| **Audio** (MAME's WSG vs the port's SCSP), 2433 frames with sound | the port's trails by 11 ms; loudness correlation 0.970; spectrum match median 1.000, >= 0.9 in 92% of frames |
| **Memory** (0x4000-0x4FFF and the sprite registers) | 5992 frames byte-exact |
| **Pictures** (every 60th frame) | 94 of 100 identical |

What the remaining differences are:

- **Memory, 8 frames:** at most 2 bytes, for one frame each: the instruction running at
  the frame edge, which MAME has only partly executed when its frame ends. None carries
  into the next frame.
- **Pictures, 6:** frames the port did not draw (the Model 2 shows 57.5 frames a second
  to Pac-Man's 60.6, and the boot self-test overruns), so it still shows an older one.
  Each is exactly MAME's picture from 1-3 frames before.
- **Audio** cannot be byte-identical: MAME synthesizes the WSG itself; the port has the
  SCSP play the same waveforms (a different chip, resampling and volume curve). The
  11 ms is the trip over the serial line to the sound board (~2 ms per register write).

What this does not check: the pictures compared are the port's own 8-bit framebuffer
(`pac_fb`, Pac-Man palette indices), not the colours a monitor shows. Those go on through
the Model 2's tile palette, colour-translation table and gamma, which this MAME models
from one guess (it reads colorxlat pen 0x40) and a real board does its own way; the same
holds for the sound board past the SCSP's registers. Both need a look at the board.

## The upload build (`PAC_RAM2`)

The image uploaded over geoserial ([upload.md](upload.md)) runs the same game code from
main RAM, with only the hottest Z80 code recompiled. `upload.py` records it: MAME's
Model 2 driver boots the geoserial kernel, the image goes up over the emulated serial line
(about 11 minutes), `port.lua` is installed through claude_mame's bridge, and only then
is pacboot launched, so the recorder sees Pac-Man's first frame. The reference is
`ref.lua`'s, as above (run it first, or reuse a `ref.*` from another run).

```sh
MAME_EXE_NAME=mame-with-model2 MAME_CWD=<scratch> M2_HLE_GEO_OFF=1 \
LS_TOOLS=tools/lockstep LS_OUT=<dir with ref.*> LS_FRAMES=6000 \
  python3 tools/lockstep/upload.py --rompath "<kernel dir>;<stock roms>"
python3 tools/lockstep/compare.py <dir> src/pacman_roms.h
```

`<kernel dir>/sfight` holds the geoserial kernel's pair (`epr-19001.15`, `epr-19002.16`)
and Pac-Man's sound EPROM (`roms/pacman/epr-19021.31`); `$LS_OUT/port.syms` comes from
the build's `game.elf`, as in `run.sh`.

Results, 6000 frames (2026-10-19), with the 1097-address partial recompile and with the
interpreter alone:

| What | partial recompile | interpreter alone |
|---|---|---|
| Z80 registers at all 5707 IRQs | identical | identical |
| Board, WSG registers and voices, 6000 frames | identical | identical |
| SCSP: 2854 register writes | all, same order, 4.7-145 ms (mean 10.1) after their frame starts | all, same order, mean 15.9 ms |
| Audio | trails by 13 ms; loudness 0.954; spectrum >= 0.9 in 91% | trails by 15 ms; loudness 0.913; >= 0.9 in 80% |
| Memory | 5992 frames byte-exact (the same 8 frame-edge instructions) | the same |
| Pictures | 95 of 100 identical | 93 of 100 |

The game is the same; the differences are the ones explained above. The faster build
gets more of its sound out in time (the SCSP writes go out while it waits for vblank).

## What it took

Matching MAME needed, in the Z80 core and the board:

- registers reset to MAME's values (0, IX/IY `FFFF`); a game that runs with
  uninitialised registers (Pac-Man's boot test does) then takes the same path
- a level-held INT line, cleared by the game's latch write, not by the acknowledge
- whole 4-cycle NOPs in HALT
- the vblank IRQ seen by an instruction that ends one cycle before the frame edge
- an idle skip that fast-forwards whole passes of the wait loop, only while the task
  queue really is empty
- **R counted exactly:** one per opcode fetch (two for prefixed ops), one per HALT NOP and
  per IRQ acknowledge, bit 7 kept apart; the recompiler adds each instruction's count and
  the idle skip 4 per skipped pass
- the latch's eight bits all kept (only two were)

It also found a sprite-restore bug (a stray pixel for a frame now and then). The core
still passes zexdoc 67/67, and the extra R counting costs nothing measurable (native MAME
runs `pacman_web` at ~280% of real time).
