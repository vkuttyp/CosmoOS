/*
 * arch/fwcfg.h - Firmware configuration items (QEMU fw_cfg on x86).
 *
 * An architecture returns -ENODEV when no such device exists; the
 * generic layer then reports "no parameters", which is the normal state
 * on real hardware.
 */

#ifndef ARCH_FWCFG_H
#define ARCH_FWCFG_H

#include <kernel/types.h>

/* Copy the item named `name` (e.g. "opt/cosmo/nettest") into buf, at most
 * len bytes; returns its full size (may exceed len), -ENOENT, -ENODEV. */
int arch_fwcfg_read(const char *name, void *buf, size_t len);

#endif /* ARCH_FWCFG_H */
