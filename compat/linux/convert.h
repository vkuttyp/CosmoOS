/*
 * convert.h - Pure conversions between the Linux ABI and the native shapes
 * (docs/compat/linux/design.md). No kernel state: the host test compiles
 * this file against the host toolchain.
 */

#ifndef COMPAT_LINUX_CONVERT_H
#define COMPAT_LINUX_CONVERT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/net/inet.h>
#include <uapi/cosmo/syscall.h>

#include "linux_abi.h"

/* Linux O_* to COSMO_O_*; -1 when a flag the kernel cannot honour is set. */
int lx_open_flags(unsigned lx, unsigned *native);
/* Native stat record to the Linux struct stat. */
void lx_stat_from_native(const struct cosmo_stat *st, struct lx_stat *out);
/* Native exit status (exit n, 128+sig, 139) to the Linux wait status word. */
int lx_wait_status(int native_status);
/* Linux sockaddr (in or in6, `len` bytes) to a netaddr; -EINVAL/-EAFNOSUPPORT. */
int lx_sockaddr_to_netaddr(const void *sa, size_t len, struct netaddr *out);
/* A netaddr as a Linux sockaddr into `out` (at most `cap` bytes written);
 * returns the full size the caller's buffer needed. */
size_t lx_sockaddr_from_netaddr(const struct netaddr *in, void *out, size_t cap);
/* Native getdents records (`in`, `inlen`) to linux_dirent64 records in `out`;
 * returns bytes written, stopping before a record that would not fit. */
/* Native COSMO_DT_* -> Linux DT_* (regular 8, directory 4, ...). */
uint8_t lx_dirent_type(uint8_t native);

size_t lx_dirents_from_native(const uint8_t *in, size_t inlen, uint8_t *out, size_t outcap);
/* Linux PROT_* to the native COSMO_PROT_* (the bits coincide; validated). */
int lx_prot(unsigned lx, int *native);

#endif
