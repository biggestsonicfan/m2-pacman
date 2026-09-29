# The sound EPROM (`epr-19021.31`)

## Why replace it

On a Model 2B the sound board has its own Motorola 68000, a Yamaha/Sega SCSP sound chip,
and 512 KB of sound RAM. The i960 cannot touch the SCSP: the only link is a serial port
(an i8251 UART at `0x9C0000`) whose output is wired to the SCSP's MIDI input. In Sonic
the Fighters the 68000 runs the game's own sound driver, a MIDI-style synth for its music
and effects, which takes commands, not raw sound.

Pac-Man's sound is a Namco WSG: three voices, each a 32-sample 4-bit waveform played at a
programmable pitch and volume. Rather than research STF's driver and find some way to
bend it into a WSG, the port **replaces the 68000 program** with one that does no sound
work of its own: it applies whatever SCSP register and sound RAM writes the i960 sends
down the serial line. The i960 is then, in effect, the sound CPU, and all of Pac-Man's
sound logic is i960 C. So no knowledge of the original audio driver is needed.

## How it was made (no compiler)

The program is **hand-written 68000 assembly**, 132 lines:
`m2-sdk/snd/scsp_passthru.s`. It is built with the GNU assembler and linker for m68k
(Ubuntu package `binutils-m68k-linux-gnu`; binutils only, no gcc):

```sh
m68k-linux-gnu-as -m68000 -o scsp_passthru.o snd/scsp_passthru.s
m68k-linux-gnu-ld -Ttext=0x600000 --oformat binary -o snd/scsp_passthru.bin scsp_passthru.o
python3 tools/snd2rom.py snd/scsp_passthru.bin roms/      # -> roms/epr-19021.31
```

- The result, `snd/scsp_passthru.bin` (304 bytes), is committed, so an ordinary build never
  needs the m68k tools; m2-sdk's CMake has a `snd` target that re-runs the first two lines
  when they are installed. Any m68k binutils works (for example an `m68k-elf` build on
  Windows).
- `tools/snd2rom.py` makes the EPROM image: pads the program to 512 KB with `FF`, and
  swaps each 16-bit word's bytes, the way the `sfight` set stores its sound ROM (MAME
  loads it `ROM_LOAD16_WORD_SWAP` into the 68000's space at `0x600000`).

## What the 68000 program does

At reset:

1. Sets the SCSP up: master volume 15, 16-bit DAC, 512 KB RAM mode; no interrupt
   sources yet; all 32 voices keyed off and silent.
2. Writes its exception vectors into sound RAM: every autovector points at a bare `RTE`,
   level 3 at the MIDI handler.
3. Routes the SCSP's MIDI-input interrupt to level 3 (`SCILV0/1` bit 3) and enables it
   (`SCIEB` bit 3).
4. Sleeps: `STOP #0x2000`, forever.

Each byte from the i960 lands in the SCSP's MIDI input FIFO and raises the interrupt. The
handler reads bytes (`0x405`) while `SCIPD` bit 3 says one is pending, feeds them to a
packet parser, and returns to `STOP`. A sleeping 68000 costs an emulator nothing between
bytes; an earlier polling version cost MAME ~16% of its host time.

### The packets

A command byte has bit 7 set; the data bytes that follow carry 7 bits each, most
significant first. A command byte arriving mid-packet starts a new packet, so a lost byte
costs one packet, never the stream.

| Packet | Bytes | Effect |
|---|---|---|
| `0x90 a1 a0 v2 v1 v0` | 6 | SCSP register write: word at `0x100000 + (a & 0xFFE)` = v |
| `0xA0 a2 a1 a0 v2 v1 v0` | 7 | sound RAM write: word at `a & 0x7FFFE` = v |
| `0xF0` | 1 | ping: the program answers `0x5A` on MIDI out (back to the i960's UART) |

## The i960 side

`m2-sdk/src/m2_scsp.h`:

- `m2_scsp_init()` brings the UART up (async 8-N-1).
- `m2_scsp_probe(frames)` pings and waits for `0x5A`. Only if it answers does the game
  send anything else: to STF's own driver the packets would be MIDI notes. So with the
  stock sound EPROM Pac-Man simply runs silent and the panel says `NO SOUND`.
- `m2_scsp_w(reg, v)` / `m2_scsp_ram_w(addr, v)` queue packets; `m2_scsp_pump()` sends
  while the UART has room, never waiting. The UART takes a byte about every 320 us, so
  one register write (6 bytes) takes ~2 ms, and ~50 bytes fit in a frame.

## Pac-Man's sound on the SCSP

`m2-sdk/src/pacman.c` (`pac_sound_init`, `pac_sound_update`):

- **At boot:** the eight 32-sample waveforms of Pac-Man's sound PROM (`1m`) go to sound
  RAM at `0x1000`, 4-bit unsigned converted to 16-bit signed. SCSP slots 0-2 are set to
  loop 32 samples forever, attack max, no decay, silent, and keyed on. That is ~1950
  bytes, sent in the background while the game boots.
- **After every Pac-Man frame:** each voice's WSG registers are decoded as MAME's
  `sound/namco.cpp` does (a 20-bit frequency step, a 4-bit volume, a 3-bit waveform), and
  only what changed is sent:
  - pitch: the WSG steps 96 kHz x f / 2^20 through a 32-sample wave, i.e. f x 375/128
    samples/s, which as an SCSP rate per 44.1 kHz output sample is f x 1393/20 in 20-bit
    fixed point, turned into the SCSP's octave + fraction form (`m2_scsp_pitch_fx`)
  - level: volume 0-15 mapped to the nearest SCSP total-level attenuation
  - waveform: the slot's start address, `0x1000 + wave x 64`

## What it is based on

There is no public documentation for the Model 2 sound board's customs; every hardware
fact here comes from **MAME's source**:

- `src/mame/sega/model2.cpp`: the sound board memory map (SCSP registers at `0x100000`,
  the ROM at `0x600000`), how the sound ROM is loaded, and the i960's UART wired to the
  SCSP's MIDI input (`model2_scsp`)
- `src/devices/sound/scsp.cpp`: the SCSP registers, the MIDI FIFO and its interrupt
  (`CheckPendingIRQ`: held while the FIFO has bytes, dropped when a read empties it), and
  the pitch (`Step`) and level models

In MAME it is checked end to end: every one of the 2854 register writes the WSG state
asked for in 6000 frames arrived, in order, and the output sounds like MAME's own Pac-Man
(see [lockstep.md](lockstep.md)).

## On real hardware

**This program has only run in MAME.** It depends on MAME's model of the sound board in
places where MAME could be wrong:

- **Booting:** in MAME, the 68000 starts from sound RAM, and at reset MAME copies the
  ROM's first 16 bytes (the reset stack pointer and PC) there. The program relies on
  that. How the real board boots its 68000 has not been checked against hardware.
- **Waking:** the program sleeps in `STOP` and needs the SCSP's MIDI-input interrupt, at
  the level `SCILV0/1` set, to wake it.
- The SCSP register values it writes (`MVOL` at `0x400`, the interrupt registers at
  `0x41E`-`0x42A`, the slot layout) are MAME's.

If any of that is wrong on a board, the i960's ping gets no answer: the game still runs,
silently, with `NO SOUND` on the panel. The sound EPROM cannot stop the game.

A first hardware test: burn the three EPROMs, boot, and look at the panel. `SCSP SOUND`
means the 68000 booted, took the MIDI interrupt and answered the ping; then insert a
coin, and the credit sound confirms the register writes reach the voices.
