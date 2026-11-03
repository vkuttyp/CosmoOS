/*
 * tap_floor.c - What tap.cee cannot hold (roadmap 2.0 SA6): the debug
 * builds' release seam, two exported variables tap.h declares and the
 * net-tap-release-order selftest sets, and the call tap.cee makes after its
 * purges.
 */

#include <kernel/net/tap.h>

void tap_floor_after_purge(void);

#if CONFIG_DEBUG
void (*tap_test_after_purge)(void *arg);
void *tap_test_after_purge_arg;
#endif

/* After the last close's purges, before its slot is given back: a frame
 * arriving now must make nothing (net-tap-release-order). */
void tap_floor_after_purge(void)
{
#if CONFIG_DEBUG
    if (tap_test_after_purge)
        tap_test_after_purge(tap_test_after_purge_arg);
#endif
}
