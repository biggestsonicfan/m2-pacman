"""ram2_upload.py: run Pac-Man on a Model 2B with the geoserial kernel burned, without
burning Pac-Man: the code in main RAM, its working memory in RAM2
(build: cmake -B build-ram2 -DPAC_RAM2=ON; make -C build-ram2).

    python tools/ram2_upload.py COM5                 real board (pyserial), 3 ms/byte
    python3 tools/ram2_upload.py --mame              shared MAME + claude_mame's bridge

It uses m2-x11's host driver (tools/geo_serial.py; --m2x11 if it isn't at ../m2-x11):
  1. PING, and delete whatever is in the kernel's upload store (it's RAM: nothing kept).
  2. Store roms/pacman_ram2/pacman.p2r in slots 0, 1, 2..., 64 KB each, which lie end to
     end from 0x580000, where it is linked to run; check each slot's pair-sum (PROG_CRC).
  3. Upload pacboot.gsx (a normal GSX1 app) and launch it. It checks the image, zeroes
     .bss in RAM2 (0x200000) and starts Pac-Man, which doesn't answer geoserial.
     If the board still answers, pacboot refused the image: its reason is at 0x540020.

The first write of each image slot is 16 bytes long: the kernel checks a GSX1 header in
any first write of 32 bytes or more, and these slots hold plain data.
"""
import argparse, asyncio, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SLOT = 0x10000                     # GS_SLOT_SIZE
CHUNK = 120                        # PROG_WRITE payload after the slot+offset
STATUS_ADDR = 0x540020             # pacboot's result word
REASONS = {0x50320000: "checks passed, Pac-Man started (and came back?)",
           0x50320001: "no P2R1 image in the store",
           0x50320002: "image size, entry or .bss outside the map",
           0x50320003: "image pair-sum mismatch"}


def load_geo_serial(m2x11):
    sys.path.insert(0, os.path.join(m2x11, "tools"))
    import geo_serial
    return geo_serial


async def write_slot(gs, g, slot, blob, progress):
    """PROG_WRITE blob into slot: 16 bytes first (no GSX1 check), then 120-byte frames."""
    off = 0
    while off < len(blob):
        n = 16 if off == 0 else CHUNK
        piece = blob[off:off + n]
        payload = bytes([slot]) + off.to_bytes(4, "little") + piece
        for _ in range(5):
            rep = await gs.command(g.CMD_PROG_WRITE, payload)
            if rep and rep[0] == g.GS_OK:
                break
        else:
            print(f"\n   WRITE failed: slot {slot} offset {off:#x} -> {rep}")
            return False
        off += len(piece)
        progress(len(piece))
    return True


async def run(gs, g, image, boot, launch=True):
    if not await gs.ping():
        ok, attempts, heard, statuses = await gs.wait_ping(25)
        if not ok:
            print(f"no PING reply ({attempts} tries, {heard} bytes heard, {statuses})")
            return False
    print("board: geoserial answers (GS01)")
    for slot, name, size in await gs.prog_list():
        rep = await gs.prog_delete(slot)
        print(f"   deleted slot {slot} '{name}' ({size} bytes): "
              f"{'ok' if rep and rep[0] == g.GS_OK else rep}")

    chunks = [image[i:i + SLOT] for i in range(0, len(image), SLOT)]
    t0, done = time.time(), [0]

    def progress(n):
        done[0] += n
        el = time.time() - t0
        eta = el / done[0] * (len(image) - done[0])
        print(f"\r   {done[0]:7d}/{len(image)} bytes  {el:5.0f}s  ~{eta:4.0f}s left",
              end="", flush=True)

    print(f"image: {len(image):#x} bytes in {len(chunks)} slots")
    for i, c in enumerate(chunks):
        slot = await gs.prog_alloc(f"pac{i}", len(c))
        if slot != i:
            print(f"   ALLOC gave slot {slot}, wanted {i}: the image must sit in slots 0..")
            return False
        if not await write_slot(gs, g, slot, c, progress):
            return False
    print()
    for i, c in enumerate(chunks):
        want = g.gsx_pair_sum(c)
        got = await gs.prog_crc(i, want)
        if got != want:
            print(f"   slot {i}: pair-sum board={got} host={want:08X}: corrupt upload")
            return False
    print(f"   all {len(chunks)} slots verified ({time.time() - t0:.0f}s)")

    slot = await gs.upload(boot, "pacboot")
    if slot is None:
        return False
    if not launch:
        print(f"pacboot is in slot {slot}; not launched (--no-launch)")
        return True
    rep = await gs.prog_launch(slot)
    if not rep or rep[0] != g.GS_OK:
        print(f"LAUNCH refused: {rep}")
        return False
    print("launched pacboot")
    await asyncio.sleep(3.0)
    if await gs.ping():
        v = await gs.peek(STATUS_ADDR)
        print(f"the board came back to the manager: pacboot says "
              f"{REASONS.get(v, hex(v) if v is not None else 'nothing')}")
        return False
    print("Pac-Man is running (geoserial no longer answers; "
          "power-cycle, or press SERVICE, for the manager)")
    return True


async def main_serial(g, port, baud, pace, image, boot, launch):
    link = g.SerialLink(port, baud, send_pace=pace)
    try:
        return await run(g.GeoSerial(link), g, image, boot, launch)
    finally:
        link.close()


async def main_mame(g, image, boot, launch, rompath, seconds):
    """MAME: claude_mame's bridge drives the emulated 315-5649. MAME_EXE_NAME (an absolute
    path works) picks the build; rompath must put the geoserial kernel's EPROM pair
    before a stock sfight set."""
    b = g._MameBridge()()
    await b.launch_mame("sfight", ["-rompath", rompath, "-debugger", "none",
                                   "-video", "none", "-sound", "none", "-nothrottle",
                                   "-snapshot_directory", os.path.join(os.getcwd(), "snap"),
                                   "-snapview", "native"])
    try:
        await b.call("continue")
        await asyncio.sleep(4.0)
        gs = g.GeoSerial(g.MameLink(b))
        await gs.drain_boot_log()
        ok = await run(gs, g, image, boot, launch)
        if ok and seconds:
            await asyncio.sleep(seconds)
            v = await b.call("read_mem", {"cpu": ":maincpu", "addr": 0x5F00F0, "width": 4,
                                          "count": 1, "space": "program"})
            print(f"frameVBL after {seconds}s: {v[0]}")
            await b.call("snapshot")
            await asyncio.sleep(0.5)
        return ok
    finally:
        await b.shutdown_mame()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("port", nargs="?", help="serial port of the RS-422 adapter (COM5)")
    ap.add_argument("--baud", type=int, default=2_000_000)
    ap.add_argument("--pace", type=float, default=0.003,
                    help="seconds between bytes sent (the 315-5649 needs ~3 ms)")
    ap.add_argument("--image", default=os.path.join(ROOT, "roms", "pacman_ram2", "pacman.p2r"))
    ap.add_argument("--boot", default=os.path.join(ROOT, "roms", "pacman_ram2", "pacboot.gsx"))
    ap.add_argument("--m2x11", default=os.path.join(ROOT, "..", "m2-x11"))
    ap.add_argument("--no-launch", action="store_true", help="upload only")
    ap.add_argument("--mame", action="store_true", help="test on MAME instead of a board")
    ap.add_argument("--rompath", default="", help="MAME: rompath (kernel pair first)")
    ap.add_argument("--seconds", type=float, default=0, help="MAME: run on, then read frameVBL")
    a = ap.parse_args()
    if not a.mame and not a.port:
        ap.error("give the serial port (COM5), or --mame")
    image = open(a.image, "rb").read()
    g = load_geo_serial(a.m2x11)
    est = (len(image) // CHUNK + 1) * (CHUNK + 9) * a.pace
    if not a.mame:
        print(f"upload ~{est / 60:.0f} min at {a.pace * 1000:.0f} ms/byte")
        ok = asyncio.run(main_serial(g, a.port, a.baud, a.pace, image, a.boot, not a.no_launch))
    else:
        ok = asyncio.run(main_mame(g, image, a.boot, not a.no_launch, a.rompath, a.seconds))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
