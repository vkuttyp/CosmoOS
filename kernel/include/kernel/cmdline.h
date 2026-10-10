/*
 * cmdline.h - The kernel command line (docs/boot/design.md, "The command
 * line").
 *
 * The loader passes the text of \cosmo\cmdline through the boot protocol
 * (v7). It is a list of tokens separated by white space; a token is
 * `key=value` or a bare `key`; `#` starts a comment that runs to the end
 * of the line. The first token with a key wins. Keys the kernel does not
 * know are ignored, so a command line written for a newer kernel still
 * boots an older one.
 *
 * The parser is free of kernel dependencies (host tests link it).
 */

#ifndef KERNEL_CMDLINE_H
#define KERNEL_CMDLINE_H

#include <stddef.h>

/* The value of the first `key=` token in `text` ("" for a bare `key`),
 * copied with a NUL into `out`. Returns the value's length, -1 when the
 * key is absent, -2 when the value does not fit in `out_len - 1` bytes
 * (nothing is copied: a truncated device name is a different device). */
int cmdline_find(const char *text, const char *key, char *out, size_t out_len);

/* Kernel: log the command line once at boot. */
void cmdline_log(void);

/* Kernel: cmdline_find over the command line the loader passed. */
int cmdline_get(const char *key, char *out, size_t out_len);

#endif /* KERNEL_CMDLINE_H */
