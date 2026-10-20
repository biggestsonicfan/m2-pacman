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

The program is **hand-written 68000 assembly**, 169 lines:
`snd/scsp_passthru.s`. It is built with the GNU assembler and linker for m68k
(Ubuntu package `binutils-m68k-linux-gnu`; binutils only, no gcc):

```sh
m68k-linux-gnu-as -m68000 -o scsp_passthru.o snd/scsp_passthru.s
m68k-linux-gnu-ld -Ttext=0x600000 --oformat binary -o snd/scsp_passthru.bin scsp_passthru.o
python3 tools/snd2rom.py snd/scsp_passthru.bin roms/      # -> roms/epr-19021.31
```

- The result, `snd/scsp_passthru.bin` (384 bytes), is committed, so an ordinary build never
  needs the m68k tools; the CMake build has a `snd` target that re-runs the first two lines
  when they are installed. Any m68k binutils works (for example an `m68k-elf` build on
  Windows).
- `tools/snd2rom.py` makes the EPROM image: pads the program to 512 KB with `FF`, and
  swaps each 16-bit word's bytes, the way the `sfight` set stores its sound ROM (MAME
  loads it `ROM_LOAD16_WORD_SWAP` into the 68000's space at `0x600000`).

## What the 68000 program does

At reset:

1. Sets the sound board's control latch (`0x400001` = `0x1F`, as STF does), then spins
   ~30 ms, as STF does, before touching the SCSP.
2. Sets the SCSP: 18-bit DAC, 4 Mbit RAM, master volume 0 for now; no interrupt sources.
3. Writes every register of all 32 slots: zero, except release rate max and total level
   0xFF (silent), and keys them all off.
4. Zeroes the effect DSP's program, coefficients and addresses (`0x700`-`0xBFF`) and puts
   its ring buffer at `0x70000`-`0x73FFF`; then master volume 15.
5. Writes all 64 exception vectors into sound RAM: the errors and traps restart the
   program, the autovectors point at a bare `RTE`, level 3 at the MIDI handler.
6. Empties the MIDI input FIFO (4 reads), routes the MIDI-input interrupt to level 3
   (`SCILV0/1` bit 3), clears it and enables it (`SCIEB` bit 3).
7. Sleeps: `STOP #0x2000`, forever.

Steps 1, 3, 4, 5 and the FIFO flush are for a real board, whose SCSP registers, DSP
program RAM and sound RAM come up holding whatever they hold. MAME clears them, so it
needs none of that (see "Checked against STF's program" below).

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

`src/m2_scsp.h`:

- `m2_scsp_init()` brings the UART up (async 8-N-1).
- `m2_scsp_probe(frames)` pings and waits for `0x5A`. Only if it answers does the game
  send anything else: to STF's own driver the packets would be MIDI notes. So with the
  stock sound EPROM Pac-Man simply runs silent and the panel says `NO SOUND`.
- `m2_scsp_w(reg, v)` / `m2_scsp_ram_w(addr, v)` queue packets; `m2_scsp_pump()` sends
  while the UART has room, never waiting. The UART takes a byte about every 320 us, so
  one register write (6 bytes) takes ~2 ms, and ~50 bytes fit in a frame.

## Pac-Man's sound on the SCSP

`src/pacman.c` (`pac_sound_init`, `pac_sound_update`):

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
- The SCSP register values it writes (the interrupt registers at `0x41E`-`0x42A`, the
  slot layout) are MAME's.

The first board run (2026-10-19) answered the ping (`SCSP SOUND`) but stayed silent with
a coin in. Sonic the Fighters' own sound program (`epr-19021.31`, reset at `0x601200`)
does two things at boot that MAME doesn't need, and this program now does both:

- It writes the sound board's control latch, a byte at `0x400001`: `0x1F` while it runs
  (bits 0-3 are LEDs, active-low; bit 4 set). It clears bit 4 only in its error-blink
  loop, so bit 4 is most likely the audio output's mute. MAME uses this latch only for
  sample-ROM banking (bit 5) and plays sound either way.
- It writes `3` to `0x400` (`MEM4MB`: 512 KB of 4 Mbit DRAM; `DAC18B`: the 18-bit DAC
  format), then `15` to `0x401` (`MVOL`). The old value, `0x000F`, set 1 Mbit DRAM and a
  16-bit DAC. MAME ignores `MEM4MB`, and gives `DAC18B` the same loudness.

### Checked against STF's program

STF's own sound program (`epr-19021.31`, word-swapped, loaded at `0x600000`) was taken
apart in Ghidra (its 68000 processor module, language `68000:BE:32:MC68020`, a superset
of the 68000; Ghidra ships no plain-68000 variant and there is no maintained one to
install) and compared with this program. What STF does that this program did not, and
now does:

| STF (address) | What it does | Why it matters on a board |
|---|---|---|
| `0x60122A` | spins ~30 ms before touching the SCSP | the board settles first |
| `0x6013B6` | every slot: TL `0xFF`, RR `0x1F`, key off | slot registers power up unknown; this program wrote only 3 of each slot's 16, and the i960 never writes `0x0E` (modulation) or `0x12` (LFO) of its 3 |
| `0x601390` | loads its own DSP program into `0x700`-`0xBFF`, ring buffer at `0x70000` (`0x402` = `0x138`) | a garbage DSP program writes its ring buffer, at sound RAM 0 when `RBP` is 0: over the vectors and the waveforms at `0x1000` |
| `0x60133A` | copies a full 64-entry vector table into sound RAM | an error or trap through an unwritten vector jumps to garbage |
| `0x601404` | reads the MIDI FIFO 4 times before enabling its interrupt | stale bytes |

What STF does the same way, confirming this program's assumptions: `3` to `0x400`
(`MEM4MB` + `DAC18B`; STF also checks that `0x40000` doesn't mirror `0`), `15` to `MVOL`
last, MIDI input at interrupt level 3 (`SCILV0` and `SCILV1` bit 3), and a handler that
reads `0x405` with a byte read and never clears `SCIPD` bit 3 through `SCIRE` (STF's reads
one byte per interrupt and returns: the interrupt is held while the FIFO has bytes, as
MAME models it). STF's handler also restarts its program on a `0xFF` byte (MIDI reset);
the i960 never sends one.

MAME test (`sfight`, 40 s, coin at 10 s, start at 11.7 s, `-wavwrite`): the new program's
output is sample-identical to the old one's. With pseudo-random words written into
every slot register, the DSP program RAM and sound RAM, and the 68000 restarted, the
old program's coin sound came out at a fifth of its level and every later level was off;
the new program's output was sample-identical to a clean restart.

If any of that is wrong on a board, the i960's ping gets no answer: the game still runs,
silently, with `NO SOUND` on the panel. The sound EPROM cannot stop the game.

A first hardware test: burn the three EPROMs, boot, and look at the panel. `SCSP SOUND`
means the 68000 booted, took the MIDI interrupt and answered the ping; then insert a
coin, and the credit sound confirms the register writes reach the voices.
