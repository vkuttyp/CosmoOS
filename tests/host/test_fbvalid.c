/*
 * test_fbvalid.c - Host test of the framebuffer description validator
 * (kernel/core/fbvalid.c, docs/kernel/diagnostics/testing.md). ASan/UBSan.
 *
 * The validator decides whether the kernel may write to memory the
 * firmware described. Everything it lets through bounds every pixel the
 * console draws, so the interesting cases are the ones it must refuse.
 */

#include "harness.h"

#include <kernel/bootinfo.h>

#include <string.h>

/* A description of the mode QEMU's q35 boots with, which is valid. */
static struct cosmoboot_info good(void)
{
    struct cosmoboot_info i;
    memset(&i, 0, sizeof(i));
    i.fb_phys = 0x80000000ull;
    i.fb_width = 1280;
    i.fb_height = 800;
    i.fb_bpp = 32;
    i.fb_pitch = 1280 * 4;
    i.fb_size = (uint64_t)i.fb_pitch * i.fb_height;
    i.fb_red_shift = 16;
    i.fb_red_bits = 8;
    i.fb_green_shift = 8;
    i.fb_green_bits = 8;
    i.fb_blue_shift = 0;
    i.fb_blue_bits = 8;
    return i;
}

static bool ok(const struct cosmoboot_info *i)
{
    struct bootinfo_framebuffer fb;
    const char *why = NULL;
    memset(&fb, 0, sizeof(fb));
    bool rc = bootinfo_fb_validate(i, &fb, &why);
    EXPECT(why != NULL);          /* a reason either way */
    EXPECT(rc || why[0] != '\0'); /* refusals say why */
    return rc;
}

static void test_accepts_a_real_mode(void)
{
    struct cosmoboot_info i = good();
    struct bootinfo_framebuffer fb;
    const char *why = NULL;
    memset(&fb, 0, sizeof(fb));
    EXPECT(bootinfo_fb_validate(&i, &fb, &why));
    EXPECT(fb.phys == i.fb_phys);
    EXPECT(fb.width == 1280 && fb.height == 800 && fb.pitch == 5120 && fb.bpp == 32);
    EXPECT(fb.red_bits == 8 && fb.blue_shift == 0);

    /* A pitch longer than a row (padded scan lines) is normal. */
    i.fb_pitch = 1280 * 4 + 64;
    i.fb_size = (uint64_t)i.fb_pitch * i.fb_height;
    EXPECT(ok(&i));

    /* So is a 16-bit mode with narrow channels. */
    i = good();
    i.fb_bpp = 16;
    i.fb_pitch = 1280 * 2;
    i.fb_size = (uint64_t)i.fb_pitch * i.fb_height;
    i.fb_red_shift = 11;
    i.fb_red_bits = 5;
    i.fb_green_shift = 5;
    i.fb_green_bits = 6;
    i.fb_blue_shift = 0;
    i.fb_blue_bits = 5;
    EXPECT(ok(&i));
}

static void test_absent_is_not_an_error(void)
{
    struct cosmoboot_info i;
    memset(&i, 0, sizeof(i));
    struct bootinfo_framebuffer fb;
    const char *why = NULL;
    EXPECT(!bootinfo_fb_validate(&i, &fb, &why));
    EXPECT(strcmp(why, "no framebuffer") == 0);
    EXPECT(!bootinfo_fb_validate(NULL, NULL, NULL));   /* NULLs are tolerated */
}

static void test_refuses_memory_it_does_not_have(void)
{
    /* One byte short of the last row. */
    struct cosmoboot_info i = good();
    i.fb_size -= 1;
    EXPECT(!ok(&i));

    /* A pitch shorter than a row of pixels: rows would overlap and the
     * last one would run past the end. */
    i = good();
    i.fb_pitch = 1280 * 4 - 4;
    EXPECT(!ok(&i));

    /* Geometry that would overflow a 32-bit product but not a 64-bit one:
     * the answer must come from the arithmetic, not from wrapping. */
    i = good();
    i.fb_width = 0xffff;
    i.fb_height = 0xffff;
    i.fb_pitch = 0xffff * 4;
    i.fb_size = 4096;
    EXPECT(!ok(&i));

    /* A framebuffer whose end wraps the address space. */
    i = good();
    i.fb_phys = 0xffffffffffffe000ull;
    i.fb_size = 0x8000;
    EXPECT(!ok(&i));

    i = good();
    i.fb_phys = 0;
    i.fb_size = 4096;   /* not "absent": a real size at physical zero */
    EXPECT(!ok(&i));
}

static void test_refuses_impossible_pixels(void)
{
    struct cosmoboot_info i = good();
    i.fb_width = 0;
    EXPECT(!ok(&i));

    i = good();
    i.fb_height = 0;
    EXPECT(!ok(&i));

    /* Only 8, 16, 24 and 32 bits a pixel are drawable. The colour fields
     * are made to fit each width, so this loop tests the pixel size rule
     * and nothing else. */
    for (uint32_t bpp = 0; bpp <= 64; bpp++) {
        i = good();
        i.fb_bpp = bpp;
        i.fb_pitch = 1280 * 8;
        i.fb_size = (uint64_t)i.fb_pitch * i.fb_height;
        i.fb_red_bits = i.fb_green_bits = i.fb_blue_bits = 1;
        i.fb_red_shift = 2;
        i.fb_green_shift = 1;
        i.fb_blue_shift = 0;
        bool expected = bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32;
        EXPECT(ok(&i) == expected);
    }

    /* A channel that runs off the end of a pixel, and one with no width:
     * either would make the console write bits that are not the pixel. */
    i = good();
    i.fb_red_shift = 28;
    i.fb_red_bits = 8;
    EXPECT(!ok(&i));

    i = good();
    i.fb_green_bits = 0;
    EXPECT(!ok(&i));

    i = good();
    i.fb_blue_bits = 33;
    EXPECT(!ok(&i));

    /* The boundary case is allowed: the topmost channel may end exactly
     * at the last bit. */
    i = good();
    i.fb_red_shift = 24;
    i.fb_red_bits = 8;
    EXPECT(ok(&i));
}

static const struct host_test tests[] = {
    { "accepts a real mode", test_accepts_a_real_mode },
    { "absent is not an error", test_absent_is_not_an_error },
    { "refuses memory it does not have", test_refuses_memory_it_does_not_have },
    { "refuses impossible pixels", test_refuses_impossible_pixels },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
