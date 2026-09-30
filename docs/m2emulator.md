# Running in m2emulator

The `pacman` EPROM set runs in m2emulator (ElSemi's Model 2 emulator, 1.1a): it boots to
the attract mode and plays at full speed. It is silent there (see [Sound](#sound)).

It needs two fixes in m2-sdk, [PR #12](https://github.com/biggestsonicfan/m2-sdk/pull/12)
(branch `m2emulator-fixes`). Both work around bugs in m2emulator, not in the port: with
or without them the set runs the same in MAME, and a real board should too.

## Running it

Build `M2_GAME=pacman` (not `pacman_web`, whose `sinr` idle needs an i960 FPU that
m2emulator does not have) with m2-sdk at PR #12 or later, and copy the three files over
the stock set in m2emulator's `roms\sfight` folder:

```
roms/pacman/epr-19001.15  epr-19002.16  epr-19021.31   ->   m2emulator\roms\sfight\
EMULATOR.EXE sfight
```

m2emulator does not check ROM checksums, so nothing else changes. The panel on the left
says `NO SOUND`; the controls are the same as in MAME (see the README).

## What was wrong

### 1. A hang before the first frame: the sound UART

With the stock m2-sdk build, m2emulator showed nothing of Pac-Man, though the vblank
counter kept ticking (so the i960 was alive and taking interrupts).
Markers written to RAM at each step of `main()` (read back with a Lua script, below) put
it inside `pac_sound_init()`. The first thing `m2_scsp_probe()` does is empty the sound
UART's receive buffer:

```c
while (M2_SND_CTL & 0x02u) (void)M2_SND_DATA;            /* drop stale RX bytes */
```

m2emulator's i8251 at `0x9C0000` reads status `0x07` and data `0xFF` whatever is done
to it: RxRDY never clears, and the loop never ends. A real i8251 holds one received
byte, so the fix bounds the loop to 16 reads (`src/m2_scsp.h`).

### 2. Wrong picture: shift counts above 31

With the hang fixed, the game ran, but parts of the screen were missing: no maze, "HIGH
SC" for "HIGH SCORE", "-SHAD" for "-SHADOW", wrong colours. The copy to the tile plane
was right (char RAM matched the port's frame buffer cell for cell), and so was the frame
buffer's drawing of video RAM: **Pac-Man's own video RAM** was wrong, so the Z80 had run
differently.

To find where, the port on m2emulator was compared with the same code on the PC
(`tools/pachost.c`), first frame by frame and then write by write: each Z80 write to
video/colour RAM or I/O logged to RAM on both sides and diffed. The first difference came
in the power-on clear of video RAM (ROM `0x2322`):

```
2322  ld (hl),a
      inc l
      jr nz,2322
```

On m2emulator the loop left after 32 bytes, when `L` became `0x20`: `inc l` had set the
Z flag. The Z80 core takes Z from a table, `z80_sz[]`, and dumping it from m2emulator's
memory showed Z set for `0x20`, `0x40`, `0x60`, ... as well as `0x00`. The table is
built by

```c
z80_sz[i] = (i & (SF | XF | YF)) | (i ? 0 : ZF);
```

and GCC's i960 backend (`i960.md`, `*equals_zero_insn0`) compiles every `x == 0` as

```
shro  x,1,y        ; y = 1 >> x
```

That is right on an i960, where a shift by 32 or more gives 0 (and in MAME, which is why
the lockstep never saw it). **m2emulator masks the shift count to its low 5 bits**, as an
x86 does, so `1 >> 32` is `1 >> 0` = 1: every non-zero multiple of 32 tests as zero. The
build has 156 of these `x == 0` tests, most of them in the Z80 core's flag code.

GCC has no option to avoid the pattern, so m2-sdk now rewrites it after compiling:
`tools/emu_shro.py` runs as the C compiler launcher (CMake option `M2_EMU_SHRO`, on by
default), compiles each file to assembly, and replaces each `shro reg,1,reg` with a
sequence that gives the same result on all three targets:

| Original | Replacement |
|---|---|
| `shro a,1,b` | `subo a,0,b` / `or a,b,b` / `shro 31,b,b` / `xor 1,b,b` (`b` = 1 if `a` is 0) |
| `shro a,1,a` | `scanbit a,a` / `shro 31,a,a` (`scanbit` of 0 gives `0xFFFFFFFF`) |

`scanbit` also sets the condition codes, which the original `shro` leaves alone, so the
script checks that nothing reads them before they are set again and stops the build if
something does. It never has, for this build.

## Checked in MAME too

The lockstep against MAME's own `pacman` driver (see [lockstep.md](lockstep.md)), 3000
frames, `EPROMS=roms/pacman`, with the rewrite:

- all 30 Z80 registers equal at all 2707 IRQs; latch, board and WSG registers identical
  in every frame; the SCSP gets the same 2161 register writes in the same order
- memory byte-exact in 2992 frames (the 8 others: the instruction running at the frame
  edge, one frame each); 45 of 50 pictures identical (the other 5: frames the port did
  not draw)

The same run with `M2_EMU_SHRO=OFF` gives exactly these results: in MAME the rewrite
changes nothing measurable.

## Sound

The port plays Pac-Man's sound on the SCSP by sending raw register writes down the sound
UART to its own 68000 program ([sound.md](sound.md)). At boot it pings that program and
waits ~1.5 s for an answer; the panel says `SCSP SOUND` if one comes, `NO SOUND` if not.
In m2emulator none comes, with sound on or off in its settings: its UART always reads
`0x07`/`0xFF`, and nothing the i960 writes to it appears to reach the sound board. So the
game runs silent there. m2emulator presumably gives stock games their sound some other
way; any sound for the port in m2emulator would need a different path from the i960 to
the SCSP, and a real Model 2B has none.

## Looking inside m2emulator

m2emulator runs `scripts\<romset>.lua` if it exists (`sfight.lua` here), calling its
`Frame()` after every frame; `I960_ReadByte/Word/DWord(addr)` read the i960's memory, and
`io.open` works, so a script can log to a file. Things worth knowing:

- **Its Lua numbers are single precision.** `I960_ReadDWord` of a value with high bits
  set loses the low ones (`0x005050C0` can come back as `0x00505100`). Read bytes with
  `I960_ReadByte` and put words together in the log, not in Lua.
- A log is only complete once the file is closed or flushed; stopping the emulator from
  outside loses what is buffered.
- The symbols' addresses are in `game.map` / `i960-elf-nm game.elf`; they move whenever
  the build changes, so look them up again after adding anything.
- For progress through boot, write stage numbers to a fixed free address from the game
  (m2-sdk's `m2_post.h` uses `0x5F0000`), and for "which of two runs is wrong", record
  per frame or per write in spare RAM on the i960 and compare with the PC build.
