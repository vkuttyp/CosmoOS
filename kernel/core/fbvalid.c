/*
 * fbvalid.c - Is this framebuffer description one we may write to?
 *
 * The loader reports what the firmware told it. Everything the kernel
 * later does to the display -- every pixel written by the console sink --
 * is bounded by the answer this file gives, so it is a pure function of
 * the boot fields with no kernel dependencies: it compiles into the
 * kernel, the host test (tests/host/test_fbvalid.c) and the fuzz target
 * (tests/fuzz/fuzz_fbvalid.c) unchanged.
 *
 * The failure this prevents is writing outside the mapping on someone
 * else's firmware, which is the kind of bug a machine with no serial port
 * cannot report.
 */

#include <stddef.h>

#include <kernel/bootinfo.h>

static bool fail(const char **why, const char *reason)
{
    if (why)
        *why = reason;
    return false;
}

bool bootinfo_fb_validate(const struct cosmoboot_info *info, struct bootinfo_framebuffer *out,
                          const char **why)
{
    if (why)
        *why = "";
    if (info == NULL)
        return fail(why, "no boot information");

    /* Absent is spelled all-zero, and is not an error. */
    if (info->fb_phys == 0 && info->fb_size == 0 && info->fb_width == 0 && info->fb_height == 0)
        return fail(why, "no framebuffer");

    if (info->fb_phys == 0)
        return fail(why, "framebuffer at physical zero");
    if (info->fb_width == 0 || info->fb_height == 0)
        return fail(why, "zero width or height");
    if (info->fb_bpp != 8 && info->fb_bpp != 16 && info->fb_bpp != 24 && info->fb_bpp != 32)
        return fail(why, "bits per pixel is not 8, 16, 24 or 32");

    uint64_t bytes_per_pixel = info->fb_bpp / 8;
    uint64_t row_bytes = (uint64_t)info->fb_width * bytes_per_pixel;
    if (info->fb_pitch < row_bytes)
        return fail(why, "pitch is shorter than a row of pixels");

    /* Both operands are 32-bit, so the product cannot overflow 64 bits. */
    uint64_t needed = (uint64_t)info->fb_pitch * info->fb_height;
    if (needed > info->fb_size)
        return fail(why, "the rows do not fit the framebuffer");
    if (info->fb_phys + info->fb_size < info->fb_phys)
        return fail(why, "the framebuffer wraps the address space");

    const uint8_t shifts[3] = { info->fb_red_shift, info->fb_green_shift, info->fb_blue_shift };
    const uint8_t bits[3] = { info->fb_red_bits, info->fb_green_bits, info->fb_blue_bits };
    for (unsigned i = 0; i < 3; i++) {
        if (bits[i] == 0 || bits[i] > info->fb_bpp)
            return fail(why, "a colour channel has no usable width");
        if ((uint32_t)shifts[i] + bits[i] > info->fb_bpp)
            return fail(why, "a colour channel runs off the end of a pixel");
    }

    if (out) {
        out->phys = info->fb_phys;
        out->size = info->fb_size;
        out->width = info->fb_width;
        out->height = info->fb_height;
        out->pitch = info->fb_pitch;
        out->bpp = info->fb_bpp;
        out->red_shift = info->fb_red_shift;
        out->red_bits = info->fb_red_bits;
        out->green_shift = info->fb_green_shift;
        out->green_bits = info->fb_green_bits;
        out->blue_shift = info->fb_blue_shift;
        out->blue_bits = info->fb_blue_bits;
    }
    return true;
}
