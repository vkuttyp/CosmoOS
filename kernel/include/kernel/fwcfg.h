/*
 * fwcfg.h - Boot parameters from the platform's firmware configuration.
 */

#ifndef KERNEL_FWCFG_H
#define KERNEL_FWCFG_H

#include <kernel/types.h>

/* NUL-terminated copy of "opt/cosmo/<key>" into buf. true if present. */
bool fwcfg_get_string(const char *key, char *buf, size_t len);

#endif /* KERNEL_FWCFG_H */
