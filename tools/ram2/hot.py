"""hot.py <cycles.txt> <percent>: the hottest Z80 instruction addresses, for a recompiled
header small enough for the upload build (lib/ram2.ld: 320 KB of upload store for all of
the code; the full src/pacman_recomp.h alone is 0xC6140 bytes compiled).

cycles.txt is tools/pactrace.c's `-c` output (address, Z80 cycles spent there). Prints the
fewest addresses, hottest first, that together took <percent> of the cycles, in address
order: a trace for tools/z80recomp.py. Every other address runs in the interpreter.

    ./pactrace -c > cycles.txt
    python3 tools/ram2/hot.py cycles.txt 98.5 > hot.txt
    python3 tools/z80recomp.py src/pacman_roms.h hot.txt src/pacman_ram2_recomp.h
"""
import sys

rows = [line.split() for line in open(sys.argv[1]) if line.strip()]
cyc = sorted(((int(c), int(a, 16)) for a, c in rows), reverse=True)
total, want = sum(c for c, _ in cyc), float(sys.argv[2]) / 100.0
picked, acc = [], 0
for c, a in cyc:
    if acc >= want * total:
        break
    picked.append(a)
    acc += c
for a in sorted(picked):
    print(f"{a:04x}")
print(f"{len(picked)} of {len(cyc)} addresses, {100.0 * acc / total:.2f}% of the cycles",
      file=sys.stderr)
