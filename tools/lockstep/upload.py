"""lockstep/upload.py: record the geoserial upload build (PAC_RAM2) for compare.py, as
port.lua records the EPROM build: MAME's Model 2 driver boots the geoserial kernel, the
image goes up over the emulated serial link (tools/ram2_upload.py, 7-11 minutes), port.lua
is installed through claude_mame's bridge, and only then is pacboot launched, so the
recorder sees Pac-Man's first frame. MAME exits after LS_FRAMES frames (port.lua).

    MAME_EXE_NAME=mame MAME_CWD=<scratch> LS_TOOLS=tools/lockstep LS_OUT=<dir> LS_FRAMES=6000 \\
      python3 tools/lockstep/upload.py --rompath "<kernel dir>;<stock roms>" \\
        --image roms/pacman_ram2/pacman.p2r --boot roms/pacman_ram2/pacboot.gsx

<kernel dir>/sfight holds the kernel pair (epr-19001.15/.16) and, for the SCSP to play,
the port's sound EPROM (roms/pacman/epr-19021.31). $LS_OUT/port.syms must be there
(run.sh writes it from game.elf).
"""
import argparse, asyncio, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


async def main(a):
    sys.path.insert(0, os.path.join(a.m2x11, "tools"))
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import geo_serial as g
    import ram2_upload
    out = os.environ["LS_OUT"]
    b = g._MameBridge()()
    await b.launch_mame("sfight", ["-rompath", a.rompath, "-debugger", "none",
                                   "-video", "none", "-sound", "none", "-nothrottle",
                                   "-wavwrite", os.path.join(out, "port.wav"),
                                   "-cfg_directory", os.path.join(out, "cfg"),
                                   "-nvram_directory", os.path.join(out, "nvram")])
    try:
        await b.call("continue")
        await asyncio.sleep(4.0)
        gs = g.GeoSerial(g.MameLink(b))
        await gs.drain_boot_log()
        if not await ram2_upload.run(gs, g, open(a.image, "rb").read(), a.boot, False):
            return 1
        slot = next((s for s, name, _ in await gs.prog_list() if name == "pacboot"), None)
        if slot is None:
            print("pacboot is not in the store")
            return 1
        tool = os.path.join(os.environ["LS_TOOLS"], "port.lua").replace("\\", "/")
        r = await b.call("eval", {"expr": f'dofile("{tool}") or true'})
        print(f"port.lua: {r}")
        await gs.prog_launch(slot)
        frames = int(os.environ.get("LS_FRAMES", "3000"))
        print(f"launched pacboot; recording {frames} frames")
        # port.lua closes its files and asks MAME to exit after the last frame's picture;
        # with the bridge attached MAME may not go, so that picture is the end signal too
        last = os.path.join(out, f"port_{frames - frames % 60:05d}.fb")
        t0 = time.time()
        while (b._proc.poll() is None and not os.path.exists(last)
               and time.time() - t0 < a.timeout):
            await asyncio.sleep(2.0)
        done = b._proc.poll() is not None or os.path.exists(last)
        await asyncio.sleep(2.0)
        print(f"{'recorded' if done else 'timed out'} after {time.time() - t0:.0f}s")
        return 0 if done else 1
    finally:
        try:   # Python 3.12's Server.wait_closed() can wait forever once MAME has gone
            await asyncio.wait_for(b.shutdown_mame(), 15)
        except asyncio.TimeoutError:
            if b._proc is not None and b._proc.poll() is None:
                b._proc.kill()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--rompath", required=True)
    ap.add_argument("--image", default=os.path.join(ROOT, "roms", "pacman_ram2", "pacman.p2r"))
    ap.add_argument("--boot", default=os.path.join(ROOT, "roms", "pacman_ram2", "pacboot.gsx"))
    ap.add_argument("--m2x11", default=os.path.join(ROOT, "..", "m2-x11"))
    ap.add_argument("--timeout", type=float, default=1800)
    sys.exit(asyncio.run(main(ap.parse_args())))
