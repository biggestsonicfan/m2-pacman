"""ram2_stamp.py: finish the upload build (PAC_RAM2=ON): stamp the STF pair-sums into the
Pac-Man image and its pacboot app, check both headers, write them out.

    python3 tools/ram2_stamp.py game.bin pacboot.bin <out.p2r> <out.gsx>

game.bin    objcopy of the lib/ram2.ld build: header 'P2R1' {entry, size, bss, bss_size,
            sum@20}, run at 0x580000 (the upload store), .bss in the slots after it
pacboot.bin objcopy of tools/ram2/pacboot.ld: a GSX1 geoserial app {sum@28}

The pair-sum is m2-x11's (tools/gsx_stamp.py gsx_pair_sum, the kernel's gs_romsum), which
the kernel's PROG_CRC also computes over each upload slot.
"""
import struct, sys

STORE_BASE = 0x580000                     # geoserial's upload store, GS_STORE_BASE
P2R_MAGIC, GSX_MAGIC = 0x31523250, 0x47535831
SLOT = 0x10000                            # geoserial upload slot (GS_SLOT_SIZE)
SLOTS_FOR_IMAGE = 5                       # 6 slots; the last holds pacboot


def pair_sum(blob, skip):
    sa = sb = 0
    for off in range(0, len(blob) & ~3, 4):
        if off == skip:
            continue
        sa += blob[off] + blob[off + 1]
        sb += blob[off + 2] + blob[off + 3]
    return (sa & 0xFFFF) | ((sb & 0xFFFF) << 16)


def fail(msg):
    sys.exit(f"ram2_stamp: {msg}")


def main(game, boot, out_img, out_gsx):
    img = bytearray(open(game, "rb").read())
    magic, entry, size, bss, bss_size = struct.unpack_from("<5I", img, 0)
    if magic != P2R_MAGIC:
        fail(f"{game}: no P2R1 header (magic {magic:08X})")
    if size != len(img) or size & 3:
        fail(f"{game}: header size {size:#x} != file size {len(img):#x}")
    if size > SLOTS_FOR_IMAGE * SLOT:
        fail(f"{game}: {size:#x} bytes needs more than {SLOTS_FOR_IMAGE} upload slots")
    if not (STORE_BASE + size <= bss and bss + bss_size <= STORE_BASE + SLOTS_FOR_IMAGE * SLOT):
        fail(f"{game}: .bss {bss:#x}+{bss_size:#x} not in the store after the image")
    if not (STORE_BASE <= entry < STORE_BASE + size):
        fail(f"{game}: entry {entry:#x} outside the image")
    struct.pack_into("<I", img, 20, pair_sum(img, 20))

    gsx = bytearray(open(boot, "rb").read())
    magic, api, entry_off, load, gbss, stack, flags = struct.unpack_from("<7I", gsx, 0)
    if magic != GSX_MAGIC or load != len(gsx) or gbss or entry_off >= load or len(gsx) & 3:
        fail(f"{boot}: bad GSX1 header (magic {magic:08X} load {load:#x} file {len(gsx):#x})")
    struct.pack_into("<I", gsx, 28, pair_sum(gsx, 28))

    open(out_img, "wb").write(img)
    open(out_gsx, "wb").write(gsx)
    print(f"image   {out_img}: {size:#x} bytes at {STORE_BASE:#x}, entry {entry:#x}, "
          f".bss {bss:#x}+{bss_size:#x}, {-(-size // SLOT)} upload slots")
    print(f"pacboot {out_gsx}: {load} bytes")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(*sys.argv[1:])
