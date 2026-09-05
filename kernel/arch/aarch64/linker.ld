/*
 * linker.ld - AArch64 kernel image layout (docs/kernel/arch/aarch64/design.md).
 *
 * The same layout as x86-64: a higher-half image at 0xFFFFFFFF80000000
 * (inside TTBR1's half, within +-4 GiB of everything -mcmodel=small must
 * reach), three PT_LOAD segments with disjoint permissions (RX / R / RW)
 * so the loader maps them W^X straight from the program headers, and a
 * PT_NOTE with the boot protocol note. Every segment boundary is page
 * aligned. The exception vector table needs 2 KiB alignment; vectors.S
 * requests it and .text honours it.
 */

OUTPUT_ARCH(aarch64)
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* R X */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* RW  */
    notes  PT_NOTE FLAGS(4);
}

SECTIONS
{
    . = KERNEL_VMA;
    __kernel_start = .;

    /* --- text --- */
    __text_start = .;
    .text : {
        KEEP(*(.text.entry))
        *(.text .text.*)
    } :text
    . = ALIGN(4096);
    __text_end = .;

    /* --- read-only data --- */
    __rodata_start = .;
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata
    . = ALIGN(16);
    __ksymtab_start = .;
    .ksymtab : {
        KEEP(*(.ksymtab))
    } :rodata
    __ksymtab_end = .;
    .note.cosmoboot : {
        KEEP(*(.note.cosmoboot))
    } :rodata :notes
    .note.gnu.build-id : {
        *(.note.gnu.build-id)
    } :rodata :notes
    . = ALIGN(4096);
    __rodata_end = .;

    /* --- writable data --- */
    __data_start = .;
    .data : {
        *(.data .data.*)
    } :data
    __data_end = .;

    __bss_start = .;
    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data
    . = ALIGN(4096);
    __bss_end = .;

    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr)
        *(.comment)
        *(.note.GNU-stack)
        *(.interp)
    }
}
