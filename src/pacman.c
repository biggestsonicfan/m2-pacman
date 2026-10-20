/*
 * pacman.c — Namco Pac-Man on the Model 2B: the MAME pacman driver ported to the i960.
 *
 * The board's Z80 is interpreted (src/m2_z80.h); the rest of the board — memory map,
 * IRQ, inputs, tile + sprite video — is src/pacman_hw.h. Pac-Man's screen (224x288,
 * portrait) sits cell-aligned in the middle of the 496x384 System 24 tile plane
 * (m2_tilefb.h: one char block per cell), and each frame only the cells that changed
 * are copied to char RAM. Pac-Man's 16 colours go straight into the tile palette.
 * The panel on the left shows how fast the emulation runs (100% = Pac-Man's 60.61 Hz);
 * when the Z80 is flat out, every other frame may skip drawing to keep the game speed.
 *
 * ROMs: `python3 tools/pacrom.py pacman.zip` writes src/pacman_roms.h (gitignored, Namco
 * data). Without it this builds the homebrew board test from src/pactest_rom.h.
 * src/puckman.c is the same program on the Namco set (src/puckman_roms.h).
 *
 * Controls (Model 2 -> Pac-Man): P1 stick, P2 stick, COIN1/COIN2 = coins, START1/2,
 * SERVICE = credit. No cocktail flip. The high score is kept in the board's backup SRAM.
 *
 * Sound needs snd/scsp_passthru.bin as the sound board's 68000 program (tools/m2_load.lua
 * loads it over a stock set); without it the game runs silent ("NO SOUND" on the panel).
 *
 * Build:  cmake -B build -DM2_GAME=pacman
 */
#include "m2.h"
#ifdef PAC_GEO_SPRITES
#define TFB_PRIO 0                     /* tile plane below the polygons: the sprites go on top */
#endif
#include "m2_tilefb.h"
#include "m2_scsp.h"

/* Boot the SHARC like any sfight-based game: the COP (cpres1) and GEO (cpres2) firmware,
 * i.e. Sonic the Fighters' SHARC programs (src/cpres*.h: tools/sharc2h.py from the
 * stf-sharc reassembly, or a ROM extract — docs/firmware-extraction.md). Pac-Man draws
 * nothing through it; each frame just commits an empty GEO display list, so the real
 * geometrizer (MAME M2_HLE_GEO_OFF, silicon) runs its frame loop alongside the game.
 * -DPAC_NO_SHARC, or no firmware headers, builds without it. */
#if !defined(PAC_NO_SHARC) && __has_include("cpres1.h") && __has_include("cpres2.h")
#define PAC_SHARC 1
#include "m2_obj.h"                    /* m2_silicon_boot, m2_frame_begin/commit/end */
#endif
#ifdef PAC_GEO_SPRITES
#ifndef PAC_SHARC
#error "PAC_GEO_SPRITES draws through the GEO: it needs the SHARC firmware (src/cpres1.h, cpres2.h)"
#endif
#define M2_SPR_COP                     /* quads through the COP: m2emulator draws no DIRECT data */
#include "m2_sprite.h"
#endif

#ifndef PAC_ROMS                       /* src/puckman.c picks the other set */
#define PAC_ROMS   "pacman_roms.h"
#ifndef PAC_RC_HDR                     /* -DPAC_RC_HDR: another recompiled header (PAC_RAM2_RC) */
#define PAC_RC_HDR "pacman_recomp.h"
#endif
#define PAC_NAME   "PAC-MAN"
#endif
#if __has_include(PAC_ROMS)
#include PAC_ROMS
#define PAC_TITLE PAC_NAME
/* the ROM's traced code statically recompiled (tools/z80recomp.py), if generated */
#if defined(PAC_RC_HDR) && !defined(PAC_NO_RECOMP) && __has_include(PAC_RC_HDR)
#define PAC_RECOMP PAC_RC_HDR
#endif
#else
#include "pactest_rom.h"
#define PAC_TITLE "TEST ROM"
#endif
#include "pacman_hw.h"

#define PAC_HZ   60606u                /* Pac-Man frame rate, mHz (MAME pacman: 18.432 MHz/3/384/264) */
#define M2_HZ    57524u                /* Model 2 vblank rate, mHz (16 MHz / 656 / 424) */

#define PAC_MAX_SKIP 3                 /* frameskip: draw at least every 4th vblank */

#define PAC_COL0 17                    /* Pac-Man screen origin on the tile plane, in cells */
#define PAC_ROW0 6

#ifdef PAC_RAM2
static u8 pac_raw_sys = 0xff, pac_raw_p1 = 0xff;   /* this frame's IN0 and IN1, for pac_board_watch */
#endif

/* Model 2 inputs -> Pac-Man IN0/IN1 (all active-low on both boards) */
static void pac_read_inputs(void) {
    u8 sys, p1, p2, in0 = 0xff, in1 = 0xff;
    M2_IO.bank = 0;
    sys = M2_IO.in0; p1 = M2_IO.in1; p2 = M2_IO.in2;
    if (!(p1 & M2_INP_UP))    in0 &= ~0x01;
    if (!(p1 & M2_INP_LEFT))  in0 &= ~0x02;
    if (!(p1 & M2_INP_RIGHT)) in0 &= ~0x04;
    if (!(p1 & M2_INP_DOWN))  in0 &= ~0x08;
    if (!(sys & M2_IN0_COIN1))   in0 &= ~0x20;
    if (!(sys & M2_IN0_COIN2))   in0 &= ~0x40;
    if (!(sys & M2_IN0_SERVICE)) in0 &= ~0x80;
    if (!(p2 & M2_INP_UP))    in1 &= ~0x01;
    if (!(p2 & M2_INP_LEFT))  in1 &= ~0x02;
    if (!(p2 & M2_INP_RIGHT)) in1 &= ~0x04;
    if (!(p2 & M2_INP_DOWN))  in1 &= ~0x08;
    if (!(sys & M2_IN0_START1)) in1 &= ~0x20;
    if (!(sys & M2_IN0_START2)) in1 &= ~0x40;
    pac_in0 = in0;
    pac_in1 = in1;                     /* bit 4 test off, bit 7 = upright cabinet */
#ifdef PAC_RAM2
    pac_raw_sys = sys; pac_raw_p1 = p1;
#endif
}

/* A tile-palette write goes through the colour-translation table: each 5-bit channel c
 * reads colorxlat row c. MAME reads pen 0x40 (sega/model2.cpp palette_w); STF fills pens
 * 0x40-0x7F of rows 0-31 with one value per row (m2-sdk m2__fill_scr_colorxlat: its
 * screen-brightness ramp, 22..224), so the board reads one of those, picked by a
 * brightness we don't know. Make pens 0x30-0xFF of rows 0-31 a straight 0..255 ramp:
 * STF's ramp starts at 22 and its polygon table saturates at 4/31, both of which wash
 * Pac-Man's blue maze out. Pens 0-0x2F are left to the polygon table (Pac-Man draws no
 * polygons). Must run BEFORE the palette writes (MAME converts a colour when written). */
static void pac_linear_tilepal(void) {
    volatile u16 *R = (volatile u16 *)0x01810000u;
    volatile u16 *G = (volatile u16 *)0x01814000u;
    volatile u16 *B = (volatile u16 *)0x01818000u;
    u32 c, e;
    for (c = 0; c < 32u; c++) {
        u16 v = (u16)((c * 255u + 15u) / 31u);
        for (e = 0x30u; e < 0x100u; e++) {
            R[c * 0x100u + e] = v; G[c * 0x100u + e] = v; B[c * 0x100u + e] = v;
        }
    }
}

#ifdef PAC_RAM2
/* Leave for the geoserial manager when the host sends "PACX" over the RS-422 link
 * (tools/ram2_upload.py --exit): set the kernel's break-in request (m2-sdk m2_fault.h
 * M2_BREAK @0x5F0020, +4), and its vblank ISR aborts us at the next vblank. RAM stays as
 * it was, so geoserial can PEEK the game's state. (The ISR's own button check is IN0
 * bit 2, TEST, not SERVICE.) Silicon only: MAME's 315-5649 needs a strobe to deliver a
 * byte; there, poke the request through the bridge. */
static u8 pac_exit_seen;
static void pac_host_poll(void) {
    static const char key[4] = { 'P', 'A', 'C', 'X' };
    int i;
    for (i = 0; i < 8; i++) {
        u8 f = M2_IO.flag, b;
        if (f & 0x04u) b = M2_IO.rxd1;
        else if (f & 0x08u) b = M2_IO.rxd2;
        else return;
        pac_exit_seen = b == (u8)key[pac_exit_seen] ? (u8)(pac_exit_seen + 1u)
                      : b == (u8)key[0] ? 1u : 0u;
        if (pac_exit_seen == 4u) {
            ((volatile u32 *)0x005F0020u)[1] = 1u;
            pac_exit_seen = 0;
        }
    }
}
#define PAC_HOST_POLL() pac_host_poll()
#else
#define PAC_HOST_POLL() ((void)0)
#endif

/* ---- sound: the Namco WSG on the SCSP -------------------------------------------------
 * With snd/scsp_passthru.s on the sound board (tools/m2_load.lua loads it), the i960 plays
 * the WSG itself: the eight 32-sample waveforms (1m PROM) go to sound RAM once, and SCSP
 * slots 0-2 loop them forever; each frame only a voice's pitch, level and waveform change.
 * WSG (MAME sound/namco.cpp pacman_sound_w): 96 kHz, a 20-bit phase step `f` per clock,
 * top 5 bits index the waveform, so a slot plays f * 96000 * 32 / 2^20 = f * 375/128
 * samples/s; as a step per 44.1 kHz sample in 20-bit fixed point that is f * 69.66
 * (f * 1393 / 20, which stays inside 32 bits). Volume 0-15 is amplitude, mapped to SCSP TL
 * (0.4/0.8/1.5/3/6/12/24/48 dB bits, MAME sound/scsp.cpp) by the nearest attenuation. */
#define PAC_SND_WAVES 0x1000u            /* sound RAM: 8 x 32 16-bit samples */
static const u8 pac_snd_tl[16] = { 255, 63, 47, 37, 30, 25, 21, 18, 14, 12, 9, 7, 5, 3, 1, 0 };
static u16 pac_snd_sent[3][3];           /* per voice: pitch, TL, SA last sent */
static int pac_snd_ok;                   /* passthrough answered the probe */

static void pac_sound_init(void) {
    int v, i;
    m2_scsp_init();
    pac_snd_ok = m2_scsp_probe(90);      /* ~1.5 s: the sound board boots alongside us */
    if (!pac_snd_ok) return;             /* a game's sound program: send it nothing */
    for (i = 0; i < 256; i++)            /* 4-bit unsigned -> 16-bit signed */
        m2_scsp_ram_w(PAC_SND_WAVES + (u32)i * 2u, (u16)(((int)(pac_prom_snd[i] & 15) - 8) * 384));
    for (v = 0; v < 3; v++) {
        u32 sl = M2_SCSP_SLOT(v);
        m2_scsp_w(sl + 0x02, (u16)PAC_SND_WAVES);   /* SA (low 16 bits)              */
        m2_scsp_w(sl + 0x04, 0);                    /* LSA                           */
        m2_scsp_w(sl + 0x06, 32);                   /* LEA: a 32-sample loop         */
        m2_scsp_w(sl + 0x08, 0x001F);               /* AR max, no decay              */
        m2_scsp_w(sl + 0x0A, 0x3C1F);               /* KRS off, DL 0, RR max         */
        m2_scsp_w(sl + 0x0C, 255);                  /* TL: silent until the game sets it */
        m2_scsp_w(sl + 0x10, 0);                    /* pitch                         */
        m2_scsp_w(sl + 0x16, 0xE000);               /* DISDL 7 (0 dB), centre        */
        m2_scsp_w(sl + 0x00, 0x0820);               /* KYONB, LPCTL normal loop, 16-bit */
        pac_snd_sent[v][0] = 0; pac_snd_sent[v][1] = 255; pac_snd_sent[v][2] = (u16)PAC_SND_WAVES;
    }
    m2_scsp_w(M2_SCSP_SLOT(0) + 0x00, 0x1820);      /* KYONEX: key the three slots on */
    m2_scsp_pump();   /* ~1950 bytes queued: the vblank waits send them (~0.6 s) while the
                       * game boots; its first sound comes much later, after a coin */
}

static void pac_sound_update(void) {
    int v;
    if (!pac_snd_ok) return;
    for (v = 0; v < 3; v++) {
        const u8 *r = pac_snd;
        u32 f = (u32)r[v * 5 + 0x11] << 4 | (u32)r[v * 5 + 0x12] << 8
              | (u32)r[v * 5 + 0x13] << 12 | (u32)r[v * 5 + 0x14] << 16;
        u32 vol = pac_snd_on ? r[v * 5 + 0x15] : 0;
        u16 pitch, tl, sa;
        if (v == 0) f |= r[0x10];
        if (!f) vol = 0;
        pitch = m2_scsp_pitch_fx(f * 1393u / 20u);
        tl = pac_snd_tl[vol & 15];
        sa = (u16)(PAC_SND_WAVES + (r[v * 5 + 0x05] & 7u) * 64u);
        if (vol && pitch != pac_snd_sent[v][0]) { m2_scsp_w(M2_SCSP_SLOT(v) + 0x10, pitch); pac_snd_sent[v][0] = pitch; }
        if (sa != pac_snd_sent[v][2])           { m2_scsp_w(M2_SCSP_SLOT(v) + 0x02, sa); pac_snd_sent[v][2] = sa; }
        if (tl != pac_snd_sent[v][1])           { m2_scsp_w(M2_SCSP_SLOT(v) + 0x0C, tl); pac_snd_sent[v][1] = tl; }
    }
    m2_scsp_pump();
}

/* ---- the high score, kept in the Model 2's backup SRAM --------------------------------
 * Pac-Man has no NVRAM: the ROM clears work RAM at power-on, so the high score dies with the
 * power. The Model 2B has 16 KB of battery-backed SRAM at 0x01d00000 (MAME "backup1", kept
 * as sfight.nv in -nvram_directory). STF keeps its settings, rankings and bookkeeping in
 * 0x3000-0x3fff and checks only its header: a word at 0x3300 and "SONICTHEFIGHTER" (in
 * swapped words) at 0x3308; with either broken it initializes the whole SRAM at boot, and
 * otherwise it keeps all of it, other bytes and all. So the record goes over that header:
 * 0x3300 "PACH", 0x3304 the score as the ROM keeps it at 0x4e88-0x4e8a (3 BCD bytes, low
 * first) with a checksum in the top byte, 0x3308 the ID "PAC-MAN M2 PORT ". Once Pac-Man
 * has saved a score, a reinstalled STF finds no header of its own and starts from clean
 * defaults; Pac-Man in turn ignores STF's header. A record from the first version, at
 * 0x3ff0 where STF never looks, is moved there at boot, so that STF rebuilds too (the
 * higher one wins when both hold a record).
 * Saved whenever the high score changes while the game runs (0x4e00 = 1 attract, 2 ready,
 * 3 playing; never from the power-on self-test, ROM 0x3000-0x33ff, which scribbles over RAM).
 * Restored once, at the attract's first frame (0x4e00 becomes 1 with the high score cleared):
 * the three bytes go into RAM, and the digits into the top row's cells 0x43ed-0x43f2 the way
 * the ROM prints a score (6 digits, leading zeros blank, "00" at least), because the ROM
 * only draws the high score when a player's score passes it (0x2a8c). -DPAC_NO_HISCORE
 * leaves all this out (tools/lockstep clears its nvram directory instead). */
#ifndef PAC_NO_HISCORE
#define PAC_NV_MAGIC 0x48434150u       /* "PACH" */
static volatile u32 *const pac_nv = (volatile u32 *)(0x01d00000u + 0x3300u);
static volatile u32 *const pac_nv_old = (volatile u32 *)(0x01d00000u + 0x3ff0u);
static const char pac_nv_id[16] = "PAC-MAN M2 PORT ";
static u32 pac_hi_saved;               /* the record's score; 0 = no record */
static int pac_hi_pending;             /* a record waits for the attract to restore it into */

static u32 pac_hi_sum(u32 s) { return (0x5au - s - (s >> 8) - (s >> 16)) & 0xffu; }

/* the score of a { "PACH", score|sum } record, 0 if there is none */
static u32 pac_hi_read(volatile u32 *r) {
    u32 v = r[1], s = v & 0xffffffu;
    return (r[0] == PAC_NV_MAGIC && (v >> 24) == pac_hi_sum(s)) ? s : 0;
}

/* the record over STF's header; the old one goes */
static void pac_hi_write(u32 s) {
    volatile u8 *id = (volatile u8 *)&pac_nv[2];
    int k;
    for (k = 0; k < 16; k++) id[k] = (u8)pac_nv_id[k];
    pac_nv[1] = s | pac_hi_sum(s) << 24;
    pac_nv[0] = PAC_NV_MAGIC;
}

static void pac_hiscore_init(void) {
    volatile const u8 *id = (volatile const u8 *)&pac_nv[2];
    u32 s = pac_hi_read(pac_nv);
    int k;
    u32 o = pac_hi_read(pac_nv_old);
    for (k = 0; k < 16; k++) if (id[k] != (u8)pac_nv_id[k]) s = 0;
    if (o) {                           /* BCD, top digit highest: compares as a number */
        if (o > s) pac_hi_write(s = o);
        for (k = 0; k < 4; k++) pac_nv_old[k] = 0;
    }
    pac_hi_saved = s; pac_hi_pending = s != 0;
}

/* after each Pac-Man frame */
static void pac_hiscore_update(void) {
    u32 s = (u32)pac_ram[0xe88] | (u32)pac_ram[0xe89] << 8 | (u32)pac_ram[0xe8a] << 16;
    u8 state = pac_ram[0xe00];
    int k;
    if (z80.pc >= 0x3000) return;      /* the self-test */
    if (pac_hi_pending) {
        if (state != 1 || s) return;
        s = pac_hi_saved;
        pac_ram[0xe88] = (u8)s; pac_ram[0xe89] = (u8)(s >> 8); pac_ram[0xe8a] = (u8)(s >> 16);
        for (k = 0; k < 6; k++)        /* through pac_wr: the cells get redrawn */
            pac_wr((u16)(0x43ed + k), (u8)((k >= 2 && !(s >> (4 * k))) ? 0x40 : (s >> (4 * k)) & 15));
        pac_hi_pending = 0;
        return;
    }
    if (s && s != pac_hi_saved && state >= 1 && state <= 3) {
        pac_hi_write(s);
        pac_hi_saved = s;
    }
}
#else
#define pac_hiscore_init()   ((void)0)
#define pac_hiscore_update() ((void)0)
#endif

#ifdef PAC_IDLE_SINR
/* Idle cheaply under MAME: its i960 core emulates each instruction at about the same host
 * cost, but charges `sinr` 406 cycles (MAME cpu/i960/i960.cpp) while computing it with one
 * host sin(). Waiting for vblank on sinr instead of a tight poll cuts the idle i960 work
 * MAME does ~100x, which is most of what a browser build spends. Needs the i960KB FPU:
 * fine on MAME and a real Model 2B, an invalid opcode on m2emulator (no FPU). */
static inline void pac_idle(void) {
    u32 x = 0;                         /* sin(+0.0) = +0.0: the value never changes */
    __asm__ volatile ("sinr %0,%0" : "+r"(x));
}
#endif

#ifdef PAC_GEO_SPRITES
/* Sprites as GEO polygons (m2-sdk m2_sprite.h) instead of pixels plotted into the tile plane:
 * the 64 patterns go to texture RAM once, turned to portrait, one mask per pen; each frame a
 * sprite is one textured quad per visible pen. The char RAM then only changes when a tile
 * does, and an emulator that draws the GEO on a GPU (m2-hle2's Dreamcast build) gets them as
 * polygons. Pac-Man colour i is colorbase PAC_SPR_CB + i. */
#define PAC_SPR_CB 16
static int pac_spr_id[64];

static void pac_geo_sprites_init(void) {
    static u8 pix[16 * 16];
    int c, r, k;
    m2_spr_flat_colors();              /* exact colorbase colours, as the tile palette's */
    for (c = 0; c < 16; c++) {
        u8 rr, g, b;
        pac_palette_rgb(c, &rr, &g, &b);
        m2_spr_color((u32)(PAC_SPR_CB + c), M2_RGB(rr >> 3, g >> 3, b >> 3));
    }
    /* portrait, as pac_draw_sprite: screen row r = native column, screen column k = 15 - native row */
    for (c = 0; c < 64; c++) {
        for (r = 0; r < 16; r++)
            for (k = 0; k < 16; k++) pix[r * 16 + k] = pac_sprpat[c][15 - k][r];
        pac_spr_id[c] = m2_spr_load(pix, 16, 16, 16);
    }
}

/* this frame's sprites, 7..0 so sprite 0 ends up in front (MAME order), clipped like
 * pac_draw_sprite to portrait rows 16..271; called between m2_frame_begin and commit */
static void pac_geo_sprites(void) {
    int n, p, x0 = PAC_COL0 * 8, y0 = PAC_ROW0 * 8;
    u16 pens[16] = { 0 };
    m2_spr_frame_setup();
    m2_spr_clip(x0, y0 + 16, x0 + PAC_W, y0 + PAC_H - 16);
    for (n = 7; n >= 0; n--) {
        u8 attr = pac_ram[0xff0 + 2 * n];
        const u8 *col = pac_colour[pac_ram[0xff1 + 2 * n] & 0x1f];
        u32 flags = ((attr & 2) ? M2_SPR_FLIPX : 0u) | ((attr & 1) ? M2_SPR_FLIPY : 0u);
        int sx, sy, x;
        pac_sprite_pos(n, &sx, &sy);
        for (p = 1; p < 4; p++) pens[p] = col[p] ? (u16)(PAC_SPR_CB + col[p]) : 0;   /* index 0 is clear */
        x = x0 + PAC_W - 16 - sy;
        m2_spr_draw(pac_spr_id[attr >> 2], x, y0 + sx, flags, pens, 0);
        m2_spr_draw(pac_spr_id[attr >> 2], x, y0 + sx - 256, flags, pens, 0);   /* wraparound */
    }
}
#endif

/* copy the changed cells of pac_fb into their char blocks */
static void pac_blit(void) {
    int i, y;
    for (i = 0; i < pac_cn; i++) {
        int c = pac_clist[i], tx = c % PAC_TW, ty = c / PAC_TW;
        volatile u32 *d = (volatile u32 *)(M2_CHARGFX + ((PAC_ROW0 + ty) * TFB_CELLS_W + PAC_COL0 + tx) * 32);
        for (y = 0; y < 8; y++) d[y] = pac_fb[c][y];
    }
    pac_copy_done();
}

static void pac_num(char *s, u32 v, int w) {
    int i;
    for (i = w - 1; i >= 0; i--) { s[i] = (char)(v ? '0' + v % 10 : (i == w - 1 ? '0' : ' ')); v /= 10; }
    s[w] = 0;
}

/* The panel on the left. -DPAC_NO_PANEL leaves it out: the game alone (the Dreamcast disc). */
#ifdef PAC_NO_PANEL
#define pac_panel(y, s)     ((void)0)
#define pac_panel_clear(y)  ((void)0)
#else
#define pac_panel(y, s)     tfb_text(8, y, s, (u8)ink, -1)
#define pac_panel_clear(y)  tfb_fillrect(8, y, 5 * 8, 8, 0)
#endif

/* -DPAC_DIAG: the panel shows how far the game got, for a board with no other window into
 * it: "S" the last step reached (1-8 the boot, 10-17 the main loop), then frameVBL, frames
 * run and the Z80's PC, redrawn at every step. A hang leaves the step it hung in. */
#ifdef PAC_DIAG
static u8 pac_dg_ink = 1;
static void pac_dg(int y, char tag, u32 v) {
    char s[11];
    int i;
    s[0] = tag; s[1] = ' ';
    for (i = 0; i < 8; i++) s[2 + i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 15];
    s[10] = 0;
    tfb_fillrect(8, y, 10 * 8, 8, 0);
    tfb_text(8, y, s, pac_dg_ink, -1);
}
static void pac_dg_step(u32 step, u32 frames) {
    pac_dg(200, 'S', step);
    pac_dg(216, 'V', frameVBL);
    pac_dg(232, 'F', frames);
    pac_dg(248, 'P', z80.pc);
}
#define PAC_DG(step, frames) pac_dg_step(step, frames)
/* whether RAM2 keeps what is written: a word every 32 KB of 0x200000-0x23FFFF, then read
 * them back. "M" 01234567 if all 256 KB are RAM (MAME); the 2B tested read garbage. "B":
 * bytes 11 22 33 44 stored one at a time at 0x200100, read back as a word (44332211) */
static void pac_dg_ram2(void) {
    volatile u32 *w = (volatile u32 *)0x00200000;
    volatile u8 *b = (volatile u8 *)0x00200100;
    u32 m = 0;
    int k;
    for (k = 0; k < 8; k++) w[k * 0x2000] = 0xA5A5A500u | (u32)k;
    for (k = 0; k < 8; k++) m = (m << 4) | (w[k * 0x2000] & 15u);
    b[0] = 0x11; b[1] = 0x22; b[2] = 0x33; b[3] = 0x44;
    pac_dg(168, 'M', m);
    pac_dg(184, 'B', *(volatile u32 *)b);
}
#else
#define PAC_DG(step, frames) ((void)0)
#endif

/* -DPAC_PROF: where the time goes, on the board. Board timer 3 (0x00F0000C) counts down at
 * 25 MHz from the value written (MAME sega/model2.cpp timers_r/w; its IRQ stays off: the
 * kernel enables only vblank). Each PAC_PF(k) adds the ticks since the last one to bucket k
 * and restarts the timer; every 60 vblanks the panel shows each bucket's average per vblank
 * in thousands of ticks (one Model 2 vblank = 435K): I inputs, Z the Z80 (pac_frame),
 * S sound, H high score, B the tile copy, W waiting for vblank, O the rest of the loop. */
#ifdef PAC_PROF
#define PAC_TIMER (*(volatile u32 *)0x00F0000Cu)
static u32 pac_pf_t0, pac_pf[7];
static void pac_pf_mark(int k) {
    u32 t = PAC_TIMER;
    pac_pf[k] += (pac_pf_t0 - t) & 0xFFFFFu;
    PAC_TIMER = 0xFFFFFu;
    pac_pf_t0 = PAC_TIMER;
}
static void pac_pf_show(u32 vblanks, u8 ink) {
    static const char tag[7] = { 'I', 'Z', 'S', 'H', 'B', 'W', 'O' };
    char s[8];
    int k;
    for (k = 0; k < 7; k++) {
        s[0] = tag[k]; s[1] = ' ';
        pac_num(s + 2, (pac_pf[k] / vblanks + 500u) / 1000u, 4);
        s[6] = 'K'; s[7] = 0;
        tfb_fillrect(8, 200 + 16 * k, 7 * 8, 8, 0);
        tfb_text(8, 200 + 16 * k, s, ink, -1);
        pac_pf[k] = 0;
    }
}
#define PAC_PF(k) pac_pf_mark(k)
#else
#define PAC_PF(k) ((void)0)
#endif

#ifdef PAC_RAM2
/* Once per Pac-Man frame, for a board we can't see into otherwise:
 * - TEST returns to the geoserial manager, RAM kept. The kernel's vblank ISR looks for
 *   TEST at IN0 bit 2 (bank 0), where MAME has it, and on the board (2026-10-19) that
 *   never fired. STF's read_sw (m2-sdk m2_swio.h) reads TEST from bank 1 instead, bit 7
 *   (SERVICE bit 6). Watch both; fire only on a press, held 1/4 s, after the bit was
 *   seen released, so a bit that just reads 0 on some board can't throw us out.
 * - The panel shows the raw inputs (IN0 bank 0, IN0 bank 1, IN1) as they change, and
 *   every second the Z80's PC. */
static u8 pac_test_armed, pac_test_held;
static void pac_hex(char *s, u32 v, int n) {
    int i;
    for (i = 0; i < n; i++) s[i] = "0123456789ABCDEF"[(v >> (4 * (n - 1 - i))) & 15];
}
static void pac_board_watch(u8 ink, u32 frames) {
    static u32 shown = 0xffffffffu;
    char s[11];
    u8 b1, test;
    u32 now;
    M2_IO.bank = 1;
    b1 = M2_IO.in0;
    M2_IO.bank = 0;
    test = !(pac_raw_sys & M2_IN0_TEST) || !(b1 & 0x80u);
    if (!test) { pac_test_armed = 1; pac_test_held = 0; }
    else if (pac_test_armed && ++pac_test_held >= 15) {
        ((volatile u32 *)0x005F0020u)[1] = 1u;      /* M2_BREAK->request: the ISR aborts us */
        pac_test_armed = 0;
    }
    now = ((u32)pac_raw_sys << 16) | ((u32)b1 << 8) | pac_raw_p1;
    if (now != shown) {
        shown = now;
        s[0] = 'I'; s[1] = ' '; pac_hex(s + 2, now, 6); s[8] = 0;
        tfb_fillrect(8, 344, 10 * 8, 8, 0);
        tfb_text(8, 344, s, ink, -1);
    }
    if (frames % 60u == 0u) {
        s[0] = 'P'; s[1] = 'C'; s[2] = ' '; pac_hex(s + 3, z80.pc, 4); s[7] = 0;
        tfb_fillrect(8, 360, 10 * 8, 8, 0);
        tfb_text(8, 360, s, ink, -1);
    }
}
#define PAC_BOARD_WATCH(ink, f) pac_board_watch((u8)(ink), f)
#else
#define PAC_BOARD_WATCH(ink, f) ((void)0)
#endif

int main(void) {
    int i, ink = 1, best = -1, late = 0, skipped = 0;
    u32 last, t0, frames = 0, tick = 0;
    char buf[8];

    m2_init();
#ifdef PAC_SHARC
    m2_silicon_boot();                 /* SHARC firmware upload + colour pipeline + GEO seed */
    for (i = 0; i < 8; i++) { m2_frame_begin(); m2_frame_end(); }   /* prime the GEO */
#endif
    pac_linear_tilepal();              /* after m2_silicon_boot: m2_color_init rebuilds colorxlat */
    tfb_init();
    PAC_DG(3, 0);
    pac_reset();
    PAC_DG(4, 0);
    for (i = 0; i < 16; i++) {         /* Pac-Man palette -> tile palette (every FB bank) */
        u8 r, g, b;
        pac_palette_rgb(i, &r, &g, &b);
        tfb_setcolor((u8)i, M2_RGB(r >> 3, g >> 3, b >> 3));
        if (r + g + b > best) { best = r + g + b; ink = i; }
    }
#ifdef PAC_DIAG
    pac_dg_ink = (u8)ink;
    pac_dg_ram2();
#endif
    PAC_DG(5, 0);
#ifdef PAC_GEO_SPRITES
    pac_geo_sprites_init();
#endif

    pac_panel(64, PAC_TITLE);
    pac_panel(80, "Z80 ON I960");
#ifdef PAC_SHARC
    pac_panel(96, "STF SHARC");
#endif
#ifdef PAC_GEO_SPRITES
    pac_panel(168, "GEO SPRITES");
#endif
    pac_panel(112, "SPEED");
    PAC_DG(6, 0);
    pac_sound_init();
    pac_panel(152, pac_snd_ok ? "SCSP SOUND" : "NO SOUND");
    PAC_DG(7, 0);
    pac_hiscore_init();
    PAC_DG(8, 0);
#ifndef PAC_NO_HISCORE
    if (pac_hi_pending) pac_panel(184, "HI SAVED");
#endif

    last = t0 = frameVBL;
    for (;;) {
        /* Pac-Man runs at 60.61 Hz, the Model 2 refreshes at 57.52 Hz (MAME sega/model2.cpp:
         * 32 MHz/2 / (656 x 424)), so about every 19th vblank runs two Pac-Man frames and
         * draws only the second. Frameskip: after a vblank that overran, skip drawing the
         * next (at most PAC_MAX_SKIP in a row), so a busy Z80 keeps the game at full speed;
         * gameplay never overruns, only the power-on self-test does. */
        int render = !(late && skipped < PAC_MAX_SKIP), n, k;
        tick += PAC_HZ;
        for (n = 0; tick >= M2_HZ; n++) tick -= M2_HZ;
        for (k = 0; k < n; k++) {
            PAC_DG(0x10, frames);
            PAC_PF(6);
            pac_read_inputs();
            PAC_BOARD_WATCH(ink, frames);
            PAC_DG(0x11, frames);
            PAC_PF(0);
#if defined(PAC_GEO_SPRITES)
            pac_frame(render && k == n - 1);
            m2_frame_begin();          /* the sprites, as polygons */
            pac_geo_sprites();
            m2_frame_commit();
#else
#ifdef PAC_SHARC
            m2_frame_begin();          /* an empty GEO frame: keeps the SHARC's frame loop going */
            m2_frame_commit();
#endif
            pac_frame(render && k == n - 1);
#endif
            PAC_DG(0x12, frames);
            PAC_PF(1);
            pac_sound_update();
            PAC_DG(0x13, frames);
            PAC_PF(2);
            pac_hiscore_update();
            PAC_PF(3);
            frames++;
        }
        PAC_DG(0x14, frames);
        PAC_PF(6);
        if (render) pac_blit();
        PAC_DG(0x15, frames);
        PAC_PF(4);
        skipped = render ? 0 : skipped + 1;
        late = frameVBL != last;
        PAC_DG(0x16, frames);
#ifndef PAC_BENCH                      /* -DPAC_BENCH: run flat out, SPEED shows the headroom */
        while (frameVBL == last) {     /* one vblank's worth per vblank; feed the UART */
            m2_scsp_pump();
            PAC_HOST_POLL();
#ifdef PAC_IDLE_SINR
            pac_idle();
#endif
        }
#endif
        PAC_DG(0x17, frames);
        PAC_HOST_POLL();               /* also when every vblank overruns (no wait above) */
        PAC_PF(5);
        last = frameVBL;
        if (last - t0 >= 60) {         /* speed = emulated frames vs real Pac-Man time */
            pac_num(buf, frames * (100000u * (M2_HZ / 8u) / (PAC_HZ / 8u)) / ((last - t0) * 1000u), 3);
            buf[3] = '%'; buf[4] = 0;
            pac_panel_clear(128);
            pac_panel(128, buf);
#ifdef PAC_PROF
            pac_pf_show(last - t0, (u8)ink);
#endif
            frames = 0; t0 = last;
        }
    }
}
