/* pacboot.c: the geoserial app that starts an uploaded Pac-Man image (lib/ram2.ld).
 *
 * tools/ram2_upload.py stores the image in the kernel's upload slots 0, 1, 2..., which sit
 * end to end from GS_STORE_BASE (0x580000, 64 KB each), where it is linked to run; then it
 * uploads this app and launches it. pacboot checks the image's header and STF pair-sum,
 * zeroes its .bss (0x541000, in the run area after this app), flushes the i960KB's
 * instruction cache and calls the entry. Pac-Man never returns. If the image is bad, pacboot leaves a code in the word at
 * 0x540020 and returns to the manager, where the host can read it. */

typedef unsigned int  u32;
typedef unsigned char u8;

#define STORE_BASE 0x00580000u          /* m2-x11 src/gs/gs_memory.h GS_STORE_BASE */
#define STORE_SIZE 0x00050000u          /* slots 0-4; slot 5 holds this app */
#define BSS_BASE   0x00541000u          /* the image's .bss: the run area after this app */
#define BSS_TOP    0x00560000u          /* GS_APP_STACK, this app's stack */
#define P2R_MAGIC  0x31523250u          /* 'P2R1' */
#define P2R_OFF_SUM 20u

/* 0x540020: 'P2..' + a reason, read by tools/ram2_upload.py if the board comes back */
#define PB_RUN     0x50320000u          /* checks passed, Pac-Man was started */
#define PB_MAGIC   0x50320001u          /* no image at the store (wrong magic) */
#define PB_SIZE    0x50320002u          /* image size or .bss outside the map */
#define PB_SUM     0x50320003u          /* pair-sum mismatch: an upload slot is corrupt */
__attribute__((section(".pacboot_status"))) volatile u32 pb_status = 0;

/* the STF ROM checksum, as m2-x11's gs_romsum: per 4-byte stride, bytes 0-1 add into a
 * 16-bit sum A and bytes 2-3 into sum B, skipping the stride that holds the sum */
static u32 pair_sum(const volatile u8 *p, u32 n, u32 skip) {
    u32 sa = 0, sb = 0, off;
    for (off = 0; off + 4u <= n; off += 4u) {
        if (off == skip) continue;
        sa += (u32)p[off] + (u32)p[off + 1u];
        sb += (u32)p[off + 2u] + (u32)p[off + 3u];
    }
    return (sa & 0xFFFFu) | ((sb & 0xFFFFu) << 16);
}

/* the 512-byte I-cache may hold lines of an earlier image run from the store: run 2.4 KB
 * of straight-line code to evict everything (m2-x11's gs_icache_flush) */
static void __attribute__((noinline)) icache_flush(void) {
    __asm__ volatile (".rept 600\n\tmov 0,g0\n\t.endr\n" ::: "g0", "memory");
}

int pacboot(void) {
    const volatile u32 *h = (const volatile u32 *)STORE_BASE;
    u32 size = h[2], bss = h[3], bss_size = h[4], k;
    volatile u32 *d;

    if (h[0] != P2R_MAGIC) { pb_status = PB_MAGIC; return 1; }
    if (size < 32u || size > STORE_SIZE || (size & 3u) ||
        bss < BSS_BASE || bss_size > BSS_TOP - BSS_BASE || bss + bss_size > BSS_TOP ||
        h[1] < STORE_BASE || h[1] >= STORE_BASE + size) { pb_status = PB_SIZE; return 2; }
    if (pair_sum((const volatile u8 *)STORE_BASE, size, P2R_OFF_SUM) != h[5]) {
        pb_status = PB_SUM; return 3;
    }
    pb_status = PB_RUN;

    d = (volatile u32 *)bss;
    for (k = 0; k < bss_size; k += 4u) *d++ = 0;
    icache_flush();
    ((void (*)(void))h[1])();
    for (;;) ;
}
