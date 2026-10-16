/*
 * version.h - Kernel identity strings.
 *
 * COSMO_BUILD_ID and COSMO_BUILD_TYPE are injected by the build system
 * (build/toolchain.mk) from the git tree and BUILD variable, so a binary
 * always says which sources produced it. The project name is provisional.
 */

#ifndef KERNEL_VERSION_H
#define KERNEL_VERSION_H

#define KERNEL_NAME    "CosmoOS"
#define KERNEL_VERSION "0.0.1"
/* The release the Linux personality reports: uname's `release` and the
 * first word after "Linux version" in /proc/version (shell completeness
 * S3). A Linux program reads a 6.x kernel's interfaces from it. */
#define KERNEL_LINUX_RELEASE "6.0.0-cosmo"

#ifndef COSMO_BUILD_ID
#define COSMO_BUILD_ID "unknown"
#endif

#ifndef COSMO_BUILD_TYPE
#define COSMO_BUILD_TYPE "unknown"
#endif

#endif /* KERNEL_VERSION_H */
