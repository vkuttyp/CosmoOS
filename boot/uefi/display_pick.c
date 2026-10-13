/*
 * display_pick.c - See display_pick.h. Nothing here calls the firmware.
 */

#include "display_pick.h"

/* EDID 1.3/1.4: bytes 0-7 the header, 18 and 19 the version and revision,
 * 24 the feature support byte (bit 1: the first detailed timing is the
 * preferred one, which 1.3 requires and 1.4 assumes), 54-71 the first
 * detailed timing: horizontal active pixels in byte 56 and the high nibble
 * of 58, vertical in 59 and the high nibble of 61. */
bool edid_preferred(const uint8_t *e, size_t len, uint32_t *w, uint32_t *h)
{
    static const uint8_t magic[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    if (e == NULL || len < 128)
        return false;
    for (unsigned i = 0; i < sizeof(magic); i++)
        if (e[i] != magic[i])
            return false;
    uint8_t sum = 0;
    for (unsigned i = 0; i < 128; i++)
        sum = (uint8_t)(sum + e[i]);
    if (sum != 0)
        return false;
    if (e[18] != 1)
        return false;   /* EDID 2.0 and unknown versions: another layout */
    if (e[19] < 3 && (e[24] & 0x02) == 0)
        return false;   /* 1.0-1.2: the first timing is merely a timing */
    const uint8_t *d = e + 54;
    if (d[0] == 0 && d[1] == 0)
        return false;   /* not a timing: a display descriptor */
    *w = d[2] | ((uint32_t)(d[4] >> 4) << 8);
    *h = d[5] | ((uint32_t)(d[7] >> 4) << 8);
    return *w != 0 && *h != 0;
}

/* One channel of a PixelBitMask format: the position of the lowest set bit
 * and the number of contiguous bits above it. A mask with holes in it is
 * not a channel, and is refused by returning 0 bits. */
static void mask_to_field(uint32_t mask, uint8_t *shift, uint8_t *bits)
{
    *shift = 0;
    *bits = 0;
    if (mask == 0)
        return;
    uint32_t sh = 0;
    while ((mask & 1u) == 0) {
        mask >>= 1;
        sh++;
    }
    uint32_t n = 0;
    while (mask & 1u) {
        mask >>= 1;
        n++;
    }
    if (mask != 0)
        return;   /* not contiguous */
    *shift = (uint8_t)sh;
    *bits = (uint8_t)n;
}

bool display_format(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi, struct display_format *f)
{
    *f = (struct display_format){ .bpp = 32 };
    switch (mi->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
        f->red_shift = 0;
        f->green_shift = 8;
        f->blue_shift = 16;
        f->red_bits = f->green_bits = f->blue_bits = 8;
        return true;
    case PixelBlueGreenRedReserved8BitPerColor:
        f->blue_shift = 0;
        f->green_shift = 8;
        f->red_shift = 16;
        f->red_bits = f->green_bits = f->blue_bits = 8;
        return true;
    case PixelBitMask: {
        mask_to_field(mi->PixelInformation.RedMask, &f->red_shift, &f->red_bits);
        mask_to_field(mi->PixelInformation.GreenMask, &f->green_shift, &f->green_bits);
        mask_to_field(mi->PixelInformation.BlueMask, &f->blue_shift, &f->blue_bits);
        if (f->red_bits == 0 || f->green_bits == 0 || f->blue_bits == 0)
            return false;
        uint32_t all = mi->PixelInformation.RedMask | mi->PixelInformation.GreenMask |
                       mi->PixelInformation.BlueMask | mi->PixelInformation.ReservedMask;
        uint32_t top = 0;
        for (uint32_t i = 0; i < 32; i++)
            if (all & (1u << i))
                top = i + 1;
        f->bpp = (top + 7) & ~7u;
        return true;
    }
    default:
        return false;
    }
}

uint32_t display_pick(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *const *modes, uint32_t n, uint32_t want_w,
                      uint32_t want_h)
{
    struct display_format f;
    for (uint32_t m = 0; m < n; m++)
        if (modes[m] != NULL && modes[m]->HorizontalResolution == want_w &&
            modes[m]->VerticalResolution == want_h && display_format(modes[m], &f))
            return m;
    return UINT32_MAX;
}
