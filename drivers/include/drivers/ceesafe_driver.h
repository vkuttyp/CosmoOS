/*
 * ceesafe_driver.h - The C floor every CeeSafe driver module links
 * (drivers/ceesafe/driver_floor.c; roadmap 2.0 SA4, docs/build/ceesafe.md,
 * "Driver modules"). CeeSafe has no variadic calls and cannot read a
 * configuration macro, so the floor gives it what it needs: kernel log
 * lines, the build's self-test switch and a device's name. Nothing here
 * is driver specific.
 */

#ifndef DRIVERS_CEESAFE_DRIVER_H
#define DRIVERS_CEESAFE_DRIVER_H

#include <kernel/types.h>

/* One kernel log line at `level` (enum klog_level) from `fmt`, a static
 * string in which "%s" stands for `s` and "%u" for `value` (decimal); any
 * other character, "%" included, is copied as it is. The line is cut at
 * 191 bytes. A null `s` prints as "(null)". */
void cosmo_drv_klog(unsigned level, const char *fmt, const char *s, uint64_t value);

/* CONFIG_SELFTEST: whether this build runs the kernel's self-tests. */
bool cosmo_drv_selftest(void);

struct device;
/* The device's name (device.h's name[], always terminated): CeeSafe can
 * point to a C char but not import a char array. */
const char *cosmo_drv_device_name(const struct device *dev);

#endif /* DRIVERS_CEESAFE_DRIVER_H */
