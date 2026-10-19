/* GNU ld (i960-elf) script: Pac-Man as an image uploaded over geoserial (tools/ram2_upload.py)
 * instead of burned into the program EPROMs. The code runs in main RAM, where the kernel's
 * upload store keeps it; the game's working memory (.bss) is RAM2 (0x200000-0x23FFFF).
 *
 * The geoserial kernel in the EPROMs keeps its boot, interrupt table and vblank ISR (which
 * counts frameVBL); this image brings no startup code. tools/ram2/pacboot.c, a small
 * geoserial app, checks the image, zeroes .bss and calls the entry in the header below.
 *
 *   0x580000  header (32 bytes), code, rodata, .data: the store's slots 0, 1, 2...
 *             (64 KB each, end to end; at most 5, slot 5 holds pacboot)
 *   0x200000  .bss (RAM2, 256 KB)
 *   0x560000  stack: the app stack geoserial gives pacboot (grows up to 0x580000)
 *
 * The kernel's own RAM (0x500000-0x53FFFF, 0x5E0000-0x5FFFFF) isn't touched.
 */
OUTPUT_ARCH(i960:kb)
ENTRY(_main)

MEMORY
{
    store (rwx) : ORIGIN = 0x00580000, LENGTH = 0x050000
    ram2  (rw)  : ORIGIN = 0x00200000, LENGTH = 0x040000
}

SECTIONS
{
    .text :
    {
        /* +0 magic 'P2R1', +4 entry, +8 image size, +12 .bss start, +16 .bss size,
         * +20 STF pair-sum of the image (tools/ram2_stamp.py; the word itself is skipped) */
        LONG(0x31523250)
        LONG(_main)
        LONG(_image_end - 0x00580000)
        LONG(_bss_start)
        LONG(_bss_end - _bss_start)
        LONG(0)
        LONG(0)
        LONG(0)
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
        *(.data)
        *(.data.*)
        . = ALIGN(16);
        _image_end = .;
    } > store

    /* STF software globals, at the addresses the kernel's ISR uses (as testlinkrom_elf.ld) */
    _RAMBASE_START = 0x00500000;
    _timerFlag     = 0x0050008C;
    _polyBank      = 0x00501000;
    _BUFF_ADD      = 0x00501004;
    _buffIndex     = 0x0050100C;

    .bss (NOLOAD) :
    {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(16);
        _bss_end = .;
    } > ram2

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }
}
