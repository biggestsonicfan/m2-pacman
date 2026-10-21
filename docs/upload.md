# Running Pac-Man over geoserial, without burning it

The first run on a real Model 2B (2026-10-19) showed one grey garbage tile repeated over
the whole screen. That is what this port shows when the font and the SHARC firmware read
as garbage. Both lie above 0x80000, in the upper half of each 512 KB program EPROM, and
no homebrew had used that half before: MAME shows the same screen when each chip's upper
half repeats its lower half. Where the chips stand, and how to check them, is at the end.

This build tests the game on the board without the upper halves. Burn only m2-x11's
geoserial kernel, which fits in the lower 128 KB of the pair, and upload Pac-Man over its
RS-422 link:

| Where | What |
|---|---|
| program EPROMs | the geoserial kernel: boot, interrupt table, vblank ISR (`frameVBL`), serial manager |
| main RAM 0x580000 | Pac-Man's code, rodata and `.data`, 0x4E670 bytes: the kernel's upload slots 0-4, where it is linked to run |
| main RAM 0x540000 | pacboot (3 KB), which the kernel runs here, its app run area |
| main RAM 0x541000 | Pac-Man's working memory (`.bss`, 0x19E80 bytes), in the run area after pacboot |
| main RAM 0x560000 | the stack (geoserial's app stack, up to 0x580000) |

The kernel's own RAM (0x500000-0x53FFFF, 0x5E0000 up) is left alone.

Not RAM2 (0x200000), which MAME maps as 256 KB of RAM: on the board (2026-10-19) words
poked there over geoserial read back 0 or garbage. The first upload kept `.bss` there
and hung in its first Z80 frame.

## Build

The kernel (m2-x11, on the PC; it builds against `..\m2-sdk`, which must be on
`master`: a kernel built on an older SDK branch comes out different):

```bat
set M2_SOFTFLOAT=1
build_clang64.bat                 rem -> roms\epr-19001.15, roms\epr-19002.16
```

It fills the first 0x1FA80 bytes of the image, so both chips' upper halves stay blank.
The pair tested with this upload (2026-10-19, m2-x11 2a04b96, m2-sdk master c4082aa) has
md5s `7bbc6bf0…` (`.15`) and `e0dc11ec…` (`.16`).

Pac-Man (here, Linux or msys2):

```sh
cmake -G "Unix Makefiles" -B build-ram2 -DM2_GAME=pacman -DPAC_RAM2=ON
make -C build-ram2 -j2            # -> roms/pacman_ram2/pacman.p2r, pacboot.gsx
```

`PAC_RAM2` links with `lib/ram2.ld`, with no startup code (the kernel's stays in
charge). It sets `PAC_NO_SHARC`: it doesn't upload the SHARC firmware a second time (the
kernel booted the SHARC already; Pac-Man draws only the tile plane).

The whole recompiled Z80 code (`src/pacman_recomp.h`, 0xC6140 bytes compiled) doesn't fit
in the store, so the upload build recompiles only the hottest part,
`src/pacman_ram2_recomp.h`: the 1097 instruction addresses that take 98.5% of the Z80's
cycles. Every other address runs in the interpreter. To make it again (after a change to
the ROM or `z80recomp.py`):

```sh
cc -O2 -Isrc -o pactrace tools/pactrace.c && ./pactrace -c > cycles.txt
python3 tools/ram2/hot.py cycles.txt 98.5 > hot.txt
python3 tools/z80recomp.py src/pacman_roms.h hot.txt src/pacman_ram2_recomp.h
```

`-DPAC_RAM2_RC=none` builds the interpreter alone (0x2E2B0 bytes, a faster upload);
`-DPAC_RAM2_RC=<header>` another set. With the interpreter alone the board ran the attract
mode at 49-58% speed (2026-10-19); MAME, which doesn't model the board's memory wait
states, shows 99-101% either way.

## Run

Burn the kernel pair, keep the board's other EPROMs, power on: the geoserial manager
comes up. Then, with the RS-422 adapter on COM5:

```sh
python tools/ram2_upload.py COM5
```

From the dev container, with the adapter forwarded to WSL (usbipd) and `ftdi_sio` loaded
there, it is `/dev/ttyUSB0`; use `/usr/bin/python3`, which has pyserial:

```sh
/usr/bin/python3 tools/ram2_upload.py /dev/ttyUSB0
```

It takes about 17 minutes (the interpreter alone, 10): the board's receiver needs 3 ms
between bytes. The script:

1. PINGs the kernel and deletes whatever is in its upload store (RAM; nothing is kept);
2. writes `pacman.p2r` into slots 0, 1, 2... (64 KB each, end to end from 0x580000) and
   checks each slot's sum with PROG_CRC;
3. uploads `pacboot.gsx` (a normal GSX1 geoserial app, `tools/ram2/pacboot.c`) and
   launches it. pacboot checks the image's header and sum, zeroes `.bss` (0x541000),
   flushes the instruction cache and calls Pac-Man's `main`.

Pac-Man then owns the board and no longer answers geoserial (only `PACX`, below). If the board still answers
three seconds after the launch, pacboot refused the image, and the script prints why
(the word it leaves at 0x540020).

`--no-launch` uploads without starting; `--m2x11 <path>` if m2-x11 isn't at `../m2-x11`
(the script uses its `tools/geo_serial.py`); `--pace` changes the byte gap.

While Pac-Man runs:

- **Holding TEST (half a second) returns to the geoserial manager**: Pac-Man sees it
  (`pac_board_watch`) and calls `pac_exit`, which puts back what the game overwrote (the
  colour tables, the font, the tilemaps) and sets the kernel's break-in request, so the
  manager's screen comes back readable. The kernel's own check (IN0 bank 0 bit 2, m2-sdk
  `i_handle.s`) is where MAME has TEST, not the real board. SERVICE stays Pac-Man's
  service credit. Pac-Man's RAM stays as it was, so geoserial can then PEEK it
  (0x541000 up for the game's state).
- **So does the host**, without anyone at the board:
  `/usr/bin/python3 tools/ram2_upload.py /dev/ttyUSB0 --exit` sends `PACX`; Pac-Man sees
  it on the RS-422 link (`pac_host_poll`) and calls `pac_exit` too
  (the request is `M2_BREAK` + 4, 0x5F0024), and the next vblank goes back to the manager, RAM kept. MAME's
  315-5649 needs a strobe to deliver a byte, so there poke 0x5F0024 through the bridge.
- `NO SOUND` on the panel unless Pac-Man's sound EPROM (`roms/pacman/epr-19021.31`) is
  in place of STF's; the kernel doesn't use the sound board, so it can stay in.
- The high score is saved in the backup SRAM as usual.

## Testing in MAME

The same script drives MAME through claude_mame's bridge (`bridge/bridge.lua`). Put the
kernel pair in a folder named `sfight`, before the stock set on the rompath:

```sh
MAME_EXE_NAME=/path/to/mame MAME_CWD=$PWD M2_HLE_GEO_OFF=1 \
  python3 tools/ram2_upload.py --mame --rompath "/path/kernel;/path/to/stock/roms" --seconds 60
```

The upload takes about 11 minutes there (7 for the interpreter alone). `--seconds` lets the game run on, then reads
`frameVBL` and saves a snapshot in `./snap`.

## Comparing the board with the host

Build with `-DPAC_DEFS=PAC_DUMP` and the board sends a record every frame over RS-422:
the Z80's PC and SP, a check of the ROM image and the RAM page at 0x4D00. With idle inputs
the game is deterministic, so the first frame where it parts from the host build is where
the board goes wrong:

```sh
cc -O2 -Isrc -o pachost tools/pachost.c
PACHOST_TRACE=/dev/shm/ht.bin ./pachost 30000 /dev/null
python3 tools/pacdump.py /dev/ttyUSB0 --out dumps --seconds 420 --host /dev/shm/ht.bin --rom pac.rom
```

It costs about 3 ms a frame. It found the attract freeze: the board garbles unaligned word
and halfword accesses (MAME doesn't), and GCC made them from byte pairs in the Z80 core.
The build now uses `-mstrict-alignment`.

## Checking the EPROMs

The garbage screen points at the EPROM burn, not the code. Two things to find out:

- What chips are they? The pair must be 512 KB 16-bit parts (27C4002 / 27C400 class) so
  that 0x80000-0xFFFFF exists in each. A 256 KB part, or the programmer set to one, would
  repeat or drop the upper half.
- Did the programmer read them back and verify against `roms/pacman/epr-19001.15` and
  `epr-19002.16`?
