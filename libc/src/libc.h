/* libc.h - Internal declarations shared by the library's source files. */

#ifndef LIBC_INTERNAL_H
#define LIBC_INTERNAL_H

#include <stddef.h>
#include <sys/types.h>

#include <stdio.h>

#include <cosmo/syscall.h>

/* Translate the kernel's negative errno convention: sets errno and
 * returns -1 on failure, the value itself otherwise. */
long __syscall_ret(long r);

void __libc_start(int argc, char **argv, char **envp) __attribute__((noreturn));
/* tcb.c: the thread pointer. __cosmo_tcb_init installs the first thread's
 * block and must run before anything that can set errno. A created
 * thread's block is installed by the kernel from `cosmo_thread.tls`. */
int __cosmo_tcb_init(void);
unsigned __cosmo_tcb_tid(void);

void __stdio_init(void);
void __stdio_flush_all(void);


/* stdio's lock, and the unlocked write core, so printf can hold the lock
 * across a whole format rather than per chunk (libc/src/stdio.c). */
void __stdio_lock(void);
void __stdio_unlock(void);
size_t __fwrite_nolock(const void *buf, size_t size, size_t n, FILE *f);

#endif
