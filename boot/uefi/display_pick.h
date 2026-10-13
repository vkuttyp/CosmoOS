/*
 * display_pick.h - The display mode decisions that need no firmware: the
 * EDID's preferred resolution, whether a mode's pixels can be handed to
 * the kernel, and which offered mode to set. Compiled into the loader and
 * into the host test (tests/host/test_display.c).
 */

#ifndef COSMO_BOOT_DISPLAY_PICK_H
#define COSMO_BOOT_DISPLAY_PICK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "efi.h"

/* The resolution an EDID names as preferred: its first detailed timing, for
 * EDID 1.3 and later always, for 1.0 to 1.2 only when the feature byte
 * says so. False without a valid 128-byte block (header, checksum,
 * version 1) or such a timing. */
bool edid_preferred(const uint8_t *e, size_t len, uint32_t *w, uint32_t *h);

/* A mode's pixel layout as the boot info carries it. */
struct display_format {
    uint8_t red_shift, red_bits, green_shift, green_bits, blue_shift, blue_bits;
    uint32_t bpp;
};

/* The layout of `mi`'s pixels, or false when it has no linear framebuffer
 * the kernel can draw on: PixelBltOnly, an unknown format, or a bit mask
 * whose colour channels are empty or not contiguous. */
bool display_format(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi, struct display_format *f);

/* The first of the `n` offered modes that is want_w x want_h and has a
 * usable layout (display_format), or UINT32_MAX. A NULL entry is a mode the
 * firmware would not describe. */
uint32_t display_pick(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *const *modes, uint32_t n, uint32_t want_w,
                      uint32_t want_h);

#endif
