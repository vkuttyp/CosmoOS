/*
 * auxv.h - what the kernel told this program about itself
 * (docs/audit/next-subsystem-pt-tls.md).
 *
 * The initial stack carries an auxiliary vector after `envp`'s terminator:
 * pairs of (tag, value) ending at `COSMO_AT_NULL`. It is how a program
 * learns the page size, its own entry point, and -- the reason this header
 * exists -- where its own program header table is mapped, from which it can
 * find its own `PT_TLS` without the kernel knowing anything about
 * thread-local storage.
 *
 * This is `getauxval` by another name, and deliberately so: a program that
 * wants its own ELF headers wants exactly what every other system hands it.
 */

#ifndef COSMO_AUXV_H
#define COSMO_AUXV_H

#include <uapi/cosmo/syscall.h>

/*
 * The value for `tag`, or 0 when the kernel did not pass it. Zero is also
 * a legitimate value for some tags -- `COSMO_AT_PHDR` is zero when the
 * program headers are not inside a mapped segment -- so a caller that must
 * tell "absent" from "zero" has to know that about its tag rather than ask
 * this function. For the tags that matter, zero means "do not use it".
 */
unsigned long cosmo_getauxval(unsigned long tag);

#endif /* COSMO_AUXV_H */
