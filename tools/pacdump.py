#!/usr/bin/env python3
"""pacdump.py: read the per-frame records a running upload build sends over RS-422
(src/pacman.c pac_dump_step; build with -DPAC_DEFS=PAC_DUMP) and compare them with the host build.

  /usr/bin/python3 tools/pacdump.py /dev/ttyUSB0 --out DIR [--seconds 60] [--host TRACE]

Each record is the frame count, the Z80's PC and SP, a 1 KB slice sum of the ROM image
and its bytes at 0x340B (checked against --rom), and the 256 bytes of RAM at 0x4D00.
Saves them all to DIR/pacf.bin (frame u32, pc, sp, 256 bytes). With --host (the file
PACHOST_TRACE=TRACE ./pachost N /dev/null writes: 256 bytes per frame), prints the bytes
that differ from the host at the same frame, each time that set changes: the board and the
host start from the same reset and see the same idle inputs.
"""
import argparse, os, struct, sys, time

LEN = 25 + 0x100 + 2


def deinterleave(buf):
    out, i = bytearray(), 0
    while i + 1 < len(buf):
        if buf[i] == 0x07:
            out.append(buf[i + 1]); i += 2
        else:
            i += 1
    return bytes(out), buf[i:]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("port")
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--host", default="")
    ap.add_argument("--rom", default="", help="the 16 KB program ROM the build carries")
    a = ap.parse_args()
    import serial
    os.makedirs(a.out, exist_ok=True)
    host = open(a.host, "rb").read() if a.host else b""
    rom = open(a.rom, "rb").read() if a.rom else b""
    rombad = set()
    log = open(os.path.join(a.out, "pacf.bin"), "ab")
    ser = serial.Serial(a.port, 2_000_000, timeout=0.05)
    buf, rest, end, n, bad, shown = bytearray(), b"", time.time() + a.seconds, 0, 0, None
    while time.time() < end:
        raw = ser.read(16384)
        if not raw:
            continue
        data, rest = deinterleave(rest + raw)
        buf += data
        while True:
            k = buf.find(b"PACF")
            if k < 0:
                del buf[:-3]
                break
            if len(buf) - k < LEN:
                del buf[:k]
                break
            d = bytes(buf[k:k + LEN])
            if (sum(d[:-2]) & 0xFFFF) != struct.unpack_from("<H", d, LEN - 2)[0]:
                del buf[:k + 4]
                bad += 1
                continue
            del buf[:k + LEN]
            frame, pc, sp = struct.unpack_from("<IHH", d, 4)
            ram = d[25:25 + 0x100]
            sl, rsum = struct.unpack_from("<BI", d, 12)
            if rom:
                ok = (sum(rom[sl * 1024:sl * 1024 + 1024]) & 0xFFFFFFFF) == rsum
                if ok == (sl in rombad):
                    print(f"frame {frame}: ROM {sl * 0x400:04X}-{sl * 0x400 + 0x3FF:04X} {'restored' if ok else 'CHANGED'}")
                    (rombad.discard if ok else rombad.add)(sl)
                if d[17:25] != rom[0x340B:0x3413] and "340B" not in rombad:
                    rombad.add("340B")
                    print(f"frame {frame}: ROM 340B reads {d[17:25].hex()}")
            log.write(d[4:12] + ram)
            n += 1
            if host and (frame + 1) * 256 <= len(host):
                ref = host[frame * 256:(frame + 1) * 256]
                diff = tuple(i for i in range(256) if ram[i] != ref[i])
                if diff != shown:
                    shown = diff
                    s = " ".join(f"{0x4D00 + i:04X}:{ram[i]:02X}/{ref[i]:02X}" for i in diff[:12])
                    print(f"frame {frame} PC={pc:04X} SP={sp:04X}: {len(diff)} differ {s}")
            elif n % 600 == 1:
                print(f"frame {frame} PC={pc:04X} SP={sp:04X} 4D46: {ram[0x46:0x4e].hex()}")
    ser.close()
    log.close()
    print(f"{n} records, {bad} with a bad sum")
    return 0 if n else 1


if __name__ == "__main__":
    sys.exit(main())
