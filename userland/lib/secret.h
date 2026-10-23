/*
 * secret -- a passphrase read from the standard input (roadmap 1.2 B5),
 * for cosmo-key and cosmo-install.
 */
#ifndef SECRET_H
#define SECRET_H

#include <stddef.h>

#define SECRET_MAX 256

/* One line, with `prompt` on the standard output and echo off when the
 * input is a terminal: the length, or -1 at the end of input. */
int secret_read(const char *prompt, char *buf, size_t n);
/* A new passphrase, asked twice: its length, or -1 (said) when it is
 * empty, the two differ or the input ended. */
int secret_read_new(const char *prog, char *buf, size_t n);
/* Overwrite `n` bytes the compiler may not elide. */
void secret_wipe(void *p, size_t n);

#endif
