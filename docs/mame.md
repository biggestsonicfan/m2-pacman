# The MAME in `mame/`

`mame/` is a stripped-down copy of the MAME used for this port: every run, lockstep and web
launch in these docs. It holds only the files needed to build the Model 2 and Pac-Man
drivers (native and web): 3573 of the commit's 31561 files, 134 MB.

## Where it comes from

[biggestsonicfan/mame](https://github.com/biggestsonicfan/mame), branch `web-audio-latency`,
commit `1be23f21bff`: upstream MAME 0.289 (mamedev `4562ed86e71`, 2026-09-04) plus five
commits, 13 files:

| Commit | What |
|---|---|
| `86ddf414ac2` | sega/model2: real-cpres2 strip-termination fix + cpres2 instrumentation |
| `dff808ddd63` | sharc: fix ADSP-2106x circular-buffer wrap at B+L |
| `85f2442b8b3` | SHARC IOP message registers, 315-5649 RS-422 transport, model2 GEO state |
| `b81352decb4` | sega/model2: add a `geoserial` set for GEO serial testing (the `m2sharc` branch ends here) |
| `1be23f21bff` | osd/js_sound (web audio): open the context at MAME's rate, bound the latency, fade underruns |

The files are exactly as in that commit: nothing is edited, only left out.

## What was kept, and how that was decided

What the builds really used, not a guess:

- **Every source and header the three builds compiled** (native Model 2, native Pac-Man,
  web Model 2), from the compiler's dependency files.
- **Identical `#pragma once` stubs** beside those headers: GCC treats content-identical
  `#pragma once` files as one and records only the first in its dependency list, so the
  others (e.g. bgfx's `wsl/stubs/oaidl.h`, identical to `rpc.h`) had to be added.
- **The build system:** `makefile`, `scripts/` (genie project scripts, build helpers, the
  Emscripten post-js), genie's own source (`3rdparty/genie`), `regtests/regtests.mak`
  (the makefile includes it).
- **Inputs of generated files:** the layouts (`.lay`), the UI font, the Z80 opcode list
  (`z80.lst` + `z80make.py`), the driver list (`src/mame/mame.lst`), `COPYING`.
- **Licences** of the third-party libraries kept (`3rdparty/*/LICENSE*`, `COPYING*`).
- **What the web build embeds** in `m2.wasm`: bgfx's shader chains, effects and ESSL
  shaders (`bgfx/chains`, `bgfx/effects`, `bgfx/shaders/essl`), `artwork/bgfx` and two
  artwork PNGs (2.2 MB).

Left out: every other driver and device, the other CPU cores, hash files, artwork,
plugins, docs, tests, and the unused third-party libraries.

Checked by building it from scratch:

- native, both drivers in one executable, and re-running the lockstep with it on both
  sides: identical results to the original MAME, number for number;
- web: `m2.wasm` has the same code as the one Pinboard runs; the only difference is the
  line endings of the embedded shader/config JSON (the original tree had CRLFs, this one
  has git's LFs), 8940 bytes.

## Building

Native (Linux; needs SDL2 and X headers: `libsdl2-dev libsdl2-ttf-dev libfontconfig-dev
libpulse-dev libasound2-dev libxinerama-dev libxi-dev libxrandr-dev`):

```sh
cd mame
make SUBTARGET=m2pac SOURCES=src/mame/sega/model2.cpp,src/mame/pacman/pacman.cpp \
     NOWERROR=1 USE_QTDEBUG=0 REGENIE=1 -j2      # -> ./m2pac: runs both sfight and pacman
```

`SUBTARGET=m2 SOURCES=src/mame/sega/model2.cpp` or `SUBTARGET=pacman
SOURCES=src/mame/pacman/pacman.cpp` build one driver each, as the original builds were.

Web (Emscripten 6.0.10, as Pinboard's player runs it):

```sh
source ~/emsdk/emsdk_env.sh
emmake make SUBTARGET=m2 SOURCES=src/mame/sega/model2.cpp NOWERROR=1 REGENIE=1 -j2
                                                  # -> m2.js + m2.wasm
```

A full build takes 30-60 minutes at `-j2`.

MAME's `plugins/` folder and the native bgfx shaders are not included: run natively with
`-video soft` or `-video none` (both checked), not `-video bgfx`; the web build embeds what
it needs (`-video accel`).

## Licence

MAME is GPL-2.0-or-later as a whole (`mame/COPYING`); its files carry their own licence
lines, and the third-party libraries theirs, in their folders.
