/*
 * user.ld - Static user program layout.
 *
 * Linked at 4 MiB (the bottom of the user window). Three W^X segments;
 * the kernel maps each with the permissions of its program header and
 * refuses executable stacks.
 */

ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* R X */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* RW  */
    stack  PT_GNU_STACK FLAGS(6);
}

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text .text.*)
    } :text
    . = ALIGN(4096);

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata
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
