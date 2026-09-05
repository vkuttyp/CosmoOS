/*
 * linker.ld - x86-64 kernel image layout.
 *
 * Higher-half kernel at -2 GiB so -mcmodel=kernel applies. Three PT_LOAD
 * segments with disjoint permissions (RX / R / RW) so the loader can map
 * them W^X directly from the program headers, plus a PT_NOTE carrying the
 * boot protocol note. Every segment boundary is page aligned.
 */

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
    /* Exported symbol table for modules (EXPORT_SYMBOL). KEEP: nothing
     * references the records, --gc-sections would drop them. */
    /* Exception fixup table (kernel/extable.h). KEEP: only the fault
     * handler's runtime scan references the entries. */
    . = ALIGN(4);
    __ex_table_start = .;
    .ex_table : {
        KEEP(*(.ex_table))
    } :rodata
    __ex_table_end = .;
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
