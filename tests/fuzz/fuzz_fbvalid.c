/*
 * fuzz_fbvalid.c - The framebuffer description the firmware handed us,
 * from arbitrary bytes (kernel/core/fbvalid.c).
 *
 * The validator is what stands between a firmware quirk and the console
 * writing outside its mapping, so the interesting property is not that it
 * accepts good input but that whatever it accepts is arithmetically safe:
 * every pixel of every row lies inside the memory the description claims,
 * and every colour field lies inside a pixel. That is checked here for
 * each accepted case, so a mutation that gets past the validator with an
 * unsafe geometry fails the target rather than passing quietly.
 */

#include "fuzz.h"

#include <kernel/bootinfo.h>

#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct cosmoboot_info info;
    memset(&info, 0, sizeof(info));

    /* The framebuffer block is 40 bytes; take them from the input. */
    uint8_t raw[40];
    memset(raw, 0, sizeof(raw));
    memcpy(raw, data, size < sizeof(raw) ? size : sizeof(raw));

    memcpy(&info.fb_phys, raw + 0, 8);
    memcpy(&info.fb_size, raw + 8, 8);
    memcpy(&info.fb_width, raw + 16, 4);
    memcpy(&info.fb_height, raw + 20, 4);
    memcpy(&info.fb_pitch, raw + 24, 4);
    memcpy(&info.fb_bpp, raw + 28, 4);
    info.fb_red_shift = raw[32];
    info.fb_red_bits = raw[33];
    info.fb_green_shift = raw[34];
    info.fb_green_bits = raw[35];
    info.fb_blue_shift = raw[36];
    info.fb_blue_bits = raw[37];

    struct bootinfo_framebuffer fb;
    const char *why = NULL;
    memset(&fb, 0, sizeof(fb));
    if (!bootinfo_fb_validate(&info, &fb, &why)) {
        FUZZ_ASSERT(why != NULL && why[0] != '\0');
        return 0;
    }

    /* Accepted: the geometry must be one the console can draw in safely. */
    FUZZ_ASSERT(fb.bpp == 8 || fb.bpp == 16 || fb.bpp == 24 || fb.bpp == 32);
    FUZZ_ASSERT(fb.width > 0 && fb.height > 0);
    FUZZ_ASSERT((uint64_t)fb.width * (fb.bpp / 8) <= fb.pitch);
    FUZZ_ASSERT((uint64_t)fb.pitch * fb.height <= fb.size);
    FUZZ_ASSERT(fb.phys != 0 && fb.phys + fb.size >= fb.phys);

    /* The last pixel of the last row, which is the furthest the console
     * can write, lies inside the framebuffer. */
    uint64_t last = (uint64_t)(fb.height - 1) * fb.pitch + (uint64_t)(fb.width - 1) * (fb.bpp / 8);
    FUZZ_ASSERT(last + (fb.bpp / 8) <= fb.size);

    FUZZ_ASSERT(fb.red_bits > 0 && (uint32_t)fb.red_shift + fb.red_bits <= fb.bpp);
    FUZZ_ASSERT(fb.green_bits > 0 && (uint32_t)fb.green_shift + fb.green_bits <= fb.bpp);
    FUZZ_ASSERT(fb.blue_bits > 0 && (uint32_t)fb.blue_shift + fb.blue_bits <= fb.bpp);
    return 0;
}

/* Seeds: the two modes the harness really boots with, and the shapes a
 * mutation would take a while to find on its own. */
static size_t seed_mode(uint8_t *buf, size_t cap, uint32_t w, uint32_t h, uint32_t bpp, uint64_t size)
{
    if (cap < 40)
        return 0;
    memset(buf, 0, 40);
    uint64_t phys = 0x80000000ull;
    uint32_t pitch = w * (bpp / 8);
    memcpy(buf + 0, &phys, 8);
    memcpy(buf + 8, &size, 8);
    memcpy(buf + 16, &w, 4);
    memcpy(buf + 20, &h, 4);
    memcpy(buf + 24, &pitch, 4);
    memcpy(buf + 28, &bpp, 4);
    buf[32] = 16; buf[33] = 8;    /* red */
    buf[34] = 8;  buf[35] = 8;    /* green */
    buf[36] = 0;  buf[37] = 8;    /* blue */
    return 40;
}

size_t fuzz_seed(unsigned i, uint8_t *buf, size_t cap)
{
    switch (i) {
    case 0:   /* q35 with QEMU's VGA */
        return seed_mode(buf, cap, 1280, 800, 32, 1280ull * 4 * 800);
    case 1:   /* virt with a ramfb */
        return seed_mode(buf, cap, 800, 600, 32, 800ull * 4 * 600);
    case 2: {   /* one byte short of the last row */
        size_t n = seed_mode(buf, cap, 1280, 800, 32, 1280ull * 4 * 800 - 1);
        return n;
    }
    case 3: {   /* absent, which is not an error */
        if (cap < 40)
            return 0;
        memset(buf, 0, 40);
        return 40;
    }
    case 4: {   /* a geometry whose product overflows 32 bits */
        size_t n = seed_mode(buf, cap, 0xffff, 0xffff, 32, 4096);
        return n;
    }
    default:
        return 0;
    }
}
