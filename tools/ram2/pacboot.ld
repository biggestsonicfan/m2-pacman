/* pacboot: the geoserial app (GSX1, as m2-x11's lib/app.ld) that starts a RAM2 Pac-Man image.
 * The kernel copies it to its app run base, 0x540000, and calls the entry on the app stack. */
OUTPUT_ARCH(i960:kb)
ENTRY(_pacboot)

MEMORY
{
    run (rwx) : ORIGIN = 0x00540000, LENGTH = 0x00020000
}

SECTIONS
{
    .text 0x00540000 :
    {
        LONG(0x47535831)                    /* +0  magic 'GSX1' */
        LONG(0x00010000)                    /* +4  api id: version 1, no syscalls used */
        LONG(_pacboot - 0x00540000)         /* +8  entry offset */
        LONG(_pacboot_end - 0x00540000)     /* +12 load size (the file size) */
        LONG(0)                             /* +16 bss size: none */
        LONG(0x1000)                        /* +20 stack it needs */
        LONG(0x00000002)                    /* +24 flags: GS_APPF_SOFTFLOAT */
        LONG(0)                             /* +28 pair-sum, tools/ram2_stamp.py */
        KEEP(*(.pacboot_status))           /* +32 result word: the host peeks 0x540020 */
        *(.text) *(.text.*) *(.rodata) *(.rodata.*) *(.data) *(.data.*)
        *(.bss) *(.bss.*) *(COMMON)        /* zeros in the image: the header says no bss */
        . = ALIGN(16);
        _pacboot_end = .;
    } > run

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }
}
