# m2-pacman

Namco **Pac-Man** running on a **Sega Model 2B** board: MAME's `pacman` driver ported to
the board's Intel i960KB, with the Z80 emulated in software and the sound played by the
Model 2's own SCSP. It runs as a replacement for Sonic the Fighters (`sfight`): three EPROMs
are swapped, everything else on the board, or in the romset, is stock.

This repository documents the port. The code lives in
[m2-sdk](https://github.com/biggestsonicfan/m2-sdk) (the Model 2 homebrew SDK it is
built with); paths below are in that repo, as of branch `pacman-launch-eproms`
(commit `57de444`, PR #10).

- [docs/sound.md](docs/sound.md): the sound EPROM, how it was made without a 68000
  compiler, what it does, and what it is based on
- [docs/lockstep.md](docs/lockstep.md): how the port is checked against MAME's own
  Pac-Man, and the results (6000 frames: registers, board, sound, memory, pictures)

## The three EPROMs

A board, MAME or m2emulator runs the stock `sfight` set with these three files replaced:

| File | What | Made from |
|---|---|---|
| `epr-19001.15` | i960 program, first half | `src/pacman.c` (or `src/pacman_web.c`) |
| `epr-19002.16` | i960 program, second half | the same |
| `epr-19021.31` | sound board 68000 program | `snd/scsp_passthru.s` (see [docs/sound.md](docs/sound.md)) |

The build writes two sets (in `m2-sdk/roms/`):

- **`roms/pacman/`**: boots the real SHARC coprocessor and geometrizer like any sfight-based
  game (Sonic the Fighters' own SHARC programs), and runs its frame loop alongside the game.
  The most accurate Model 2; use it for **m2emulator and real hardware**.
- **`roms/pacman_web/`**: the same game without the SHARC boot, and idling on the i960's
  `sinr` instruction, so MAME has far less to emulate (in a browser it is the difference
  between ~17% and full speed). Use it for **MAME** and the Pinboard web launch. The `sinr`
  idle needs the i960's FPU, so it is not meant for m2emulator.

The sound EPROM is the same file in both. The Namco ROM data is compiled into the program
EPROMs; none of it is in either repository (`tools/pacrom.py` reads your `pacman.zip`).

## Building

Needs the i960-elf GCC toolchain (see m2-sdk's README) and Python 3.

```sh
cd m2-sdk
python3 tools/pacrom.py path/to/pacman.zip          # -> src/pacman_roms.h (Namco data, git-ignored)
# optional, recommended: the statically recompiled Z80 code
cc -O2 -Isrc -o pactrace tools/pactrace.c && ./pactrace > trace.txt
python3 tools/z80recomp.py src/pacman_roms.h trace.txt src/pacman_recomp.h
cmake -G "Unix Makefiles" -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-i960-elf.cmake -DM2_GAME=pacman
make -C build -j2                                    # -> roms/pacman/ (M2_GAME=pacman_web -> roms/pacman_web/)
```

`src/puckman.c` builds the Namco Puck Man set the same way (`puckman.zip`, `roms/puckman/`).
The sound EPROM needs no 68000 toolchain: its 304-byte program is committed
(`snd/scsp_passthru.bin`) and the build pads and word-swaps it (`tools/snd2rom.py`).

## Running

MAME (a build with the Model 2 driver; `m2sharc` for the real SHARC path): put the three
files in a folder named `sfight` and list it before the folder with the stock
`sfight.zip`, `schamp.zip` and `segabill.zip`:

```sh
M2_HLE_GEO_OFF=1 mame sfight -rompath "/path/with/sfight-folder;/path/to/stock/roms" -window
```

MAME reports WRONG CHECKSUMS for exactly those three files; that is expected. Leave out
`M2_HLE_GEO_OFF=1` to use MAME's quicker built-in geometrizer.

The panel on the left of the screen shows the emulation's speed (100% = Pac-Man's
60.61 Hz) and `SCSP SOUND` when the sound EPROM answered the i960 (`NO SOUND` with the
stock one; the game then runs silent).

Controls (Model 2 -> Pac-Man): P1 stick, P2 stick, COIN 1/2 = coins, START 1/2, SERVICE =
a credit. Pac-Man is silent until a coin goes in, as on the real machine. No cocktail flip.

## How it works, briefly

- **Z80:** `src/m2_z80.h`, an interpreter that passes zexdoc (67/67), plus
  `tools/z80recomp.py`, which statically recompiles the ~4200 instruction addresses the
  game uses into C (the interpreter handles anything else). An idle skip ends the Z80's
  time slice in the game's wait-for-vblank loop.
- **Board:** `src/pacman_hw.h`: memory map, the 74LS259 latch, the level-held vblank
  IRQ and IM 2 vector as MAME's driver has them, inputs, and tile + sprite video.
- **Video:** Pac-Man's 224x288 portrait screen sits in the Model 2's System 24 tile plane
  (`src/m2_tilefb.h`); each frame only the changed 8x8 cells are copied.
- **Timing:** Pac-Man runs at 60.61 Hz, the Model 2 refreshes at 57.52 Hz, so about every
  19th vblank runs two Pac-Man frames (drawing only the second): game time matches the
  real board.
- **Sound:** the i960 plays the Namco WSG on the SCSP, through a small relay program
  on the sound board; see [docs/sound.md](docs/sound.md).

## Status

- Byte-for-byte in lockstep with MAME's own `pacman` driver over 6000 frames, down to
  the Z80's R register; see [docs/lockstep.md](docs/lockstep.md).
- Runs in MAME (native and web). **Not yet tried on real hardware or in m2emulator.**
  The sound EPROM in particular rests on MAME's model of the sound board (see
  [docs/sound.md](docs/sound.md#on-real-hardware)).
