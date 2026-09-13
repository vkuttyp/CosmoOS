/*
 * user.ld - Static user program layout.
 *
 * Linked at 4 MiB (the bottom of the user window); the ELF and program
 * headers are part of the text segment so the loader can publish AT_PHDR,
 * which both Linux libcs and this one walk to find their own PT_TLS.
 * Three W^X segments plus the thread-local template;
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
    tls    PT_TLS   FLAGS(4);  /* the thread-local template: .tdata + .tbss, a copy per thread */
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

    /*
     * Thread-local storage. These two are a *template*: libc copies
     * `.tdata` and zeroes `.tbss` into per-thread storage at startup and
     * on every thread it creates (docs/audit/next-subsystem-pt-tls.md).
     * Without them a program using `__thread` does not link at all --
     * `ld.lld` refuses an STT_TLS symbol with no PT_TLS segment, which is
     * how this was discovered rather than assumed.
     *
     * They sit at the start of the data segment so the template's bytes
     * are inside a PT_LOAD libc can read, which is one of the things libc
     * checks before copying anything.
     */
    .tdata : {
        *(.tdata .tdata.*)
    } :data :tls
    .tbss : {
        *(.tbss .tbss.*)
    } :data :tls

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
