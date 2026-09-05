/*
 * user.ld - Static user program layout.
 *
 * Linked at 4 MiB (the bottom of the user window); the ELF and program
 * headers are part of the text segment so the loader can publish AT_PHDR
 * (Linux libcs walk it for TLS). Three W^X segments;
 * the kernel maps each with the permissions of its program header and
 * refuses executable stacks.
 */

ENTRY(_start)

PHDRS
{
    text   PT_LOAD FILEHDR PHDRS FLAGS(5);   /* R X; carries the ELF and program headers (AT_PHDR) */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* RW  */
    note   PT_NOTE  FLAGS(4);  /* the CosmoOS ABI note, inside the rodata segment */
    stack  PT_GNU_STACK FLAGS(6);
}

SECTIONS
{
    . = 0x400000 + SIZEOF_HEADERS;

    .text : {
        *(.text .text.*)
    } :text
    . = ALIGN(4096);

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata
    .note.cosmo : {
        KEEP(*(.note.cosmo))
    } :rodata :note
    . = ALIGN(4096);

    .data : {
        *(.data .data.*)
    } :data
    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr)
        *(.comment)
        *(.note.GNU-stack)
    }
}
