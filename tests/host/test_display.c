/*
 * test_display.c - Host test of the loader's display mode decisions
 * (boot/uefi/display_pick.c, docs/boot/design.md "The display mode").
 * ASan/UBSan.
 *
 * No firmware the boots run under offers an EDID (QEMU's OVMF and AAVMF,
 * Parallels Desktop 27), so the loader's mode setting is reached only
 * here: an EDID it must read, the ones it must refuse, and the modes it
 * may and may not pick.
 */

#include "harness.h"

#include "display_pick.h"

#include <string.h>

/* A valid EDID 1.<rev> block whose first detailed timing is w x h. */
static void edid(uint8_t e[128], uint8_t rev, uint8_t features, uint32_t w, uint32_t h)
{
    static const uint8_t magic[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    memset(e, 0, 128);
    memcpy(e, magic, sizeof(magic));
    e[18] = 1;
    e[19] = rev;
    e[24] = features;
    uint8_t *d = e + 54;
    d[0] = 0x02;   /* a pixel clock: a timing, not a display descriptor */
    d[1] = 0x3a;
    d[2] = (uint8_t)w;
    d[4] = (uint8_t)((w >> 8) << 4);
    d[5] = (uint8_t)h;
    d[7] = (uint8_t)((h >> 8) << 4);
    uint8_t sum = 0;
    for (unsigned i = 0; i < 127; i++)
        sum = (uint8_t)(sum + e[i]);
    e[127] = (uint8_t)(0x100 - sum);
}

static void test_reads_the_preferred_timing(void)
{
    uint8_t e[128];
    uint32_t w = 0, h = 0;
    edid(e, 4, 0x0a, 1920, 1080);
    EXPECT(edid_preferred(e, sizeof(e), &w, &h));
    EXPECT(w == 1920 && h == 1080);
    /* 1.3 requires the preferred-timing bit; it is the first timing either way. */
    edid(e, 3, 0x0a, 2560, 1600);
    EXPECT(edid_preferred(e, sizeof(e), &w, &h));
    EXPECT(w == 2560 && h == 1600);
    /* 1.0-1.2: preferred only when the feature byte says so. */
    edid(e, 2, 0x02, 1280, 1024);
    EXPECT(edid_preferred(e, sizeof(e), &w, &h));
    EXPECT(w == 1280 && h == 1024);
    /* Extension blocks after the first 128 bytes are not read. */
    uint8_t big[256];
    edid(big, 4, 0x0a, 1024, 768);
    memset(big + 128, 0x5a, 128);
    EXPECT(edid_preferred(big, sizeof(big), &w, &h));
    EXPECT(w == 1024 && h == 768);
}

static void test_refuses_what_is_not_a_preference(void)
{
    uint8_t e[128];
    uint32_t w = 0, h = 0;
    edid(e, 2, 0x00, 1280, 1024);   /* 1.2 without the bit: merely a timing */
    EXPECT(!edid_preferred(e, sizeof(e), &w, &h));
    edid(e, 4, 0x0a, 1920, 1080);
    EXPECT(!edid_preferred(e, 127, &w, &h));   /* short */
    EXPECT(!edid_preferred(NULL, 128, &w, &h));
    e[100] ^= 1;   /* the checksum no longer holds */
    EXPECT(!edid_preferred(e, sizeof(e), &w, &h));
    edid(e, 4, 0x0a, 1920, 1080);
    e[1] = 0;   /* the header, with the checksum fixed up */
    e[127] = (uint8_t)(e[127] + 0xff);
    EXPECT(!edid_preferred(e, sizeof(e), &w, &h));
    edid(e, 4, 0x0a, 1920, 1080);
    e[18] = 2;   /* EDID 2.0: another layout */
    e[127] = (uint8_t)(e[127] - 1);
    EXPECT(!edid_preferred(e, sizeof(e), &w, &h));
    /* A display descriptor where the timing should be. */
    edid(e, 4, 0x0a, 1920, 1080);
    uint8_t before = (uint8_t)(e[54] + e[55]);
    e[54] = e[55] = 0;
    e[127] = (uint8_t)(e[127] + before);
    EXPECT(!edid_preferred(e, sizeof(e), &w, &h));
    /* A zero size. */
    edid(e, 4, 0x0a, 0, 1080);
    EXPECT(!edid_preferred(e, sizeof(e), &w, &h));
}

static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION mode(uint32_t w, uint32_t h, EFI_GRAPHICS_PIXEL_FORMAT fmt)
{
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION m;
    memset(&m, 0, sizeof(m));
    m.HorizontalResolution = w;
    m.VerticalResolution = h;
    m.PixelFormat = fmt;
    m.PixelsPerScanLine = w;
    return m;
}

static void test_pixel_layouts(void)
{
    struct display_format f;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION m = mode(800, 600, PixelBlueGreenRedReserved8BitPerColor);
    EXPECT(display_format(&m, &f));
    EXPECT(f.bpp == 32 && f.red_shift == 16 && f.green_shift == 8 && f.blue_shift == 0 && f.red_bits == 8);
    m.PixelFormat = PixelRedGreenBlueReserved8BitPerColor;
    EXPECT(display_format(&m, &f));
    EXPECT(f.red_shift == 0 && f.blue_shift == 16);
    m.PixelFormat = PixelBitMask;   /* 16-bit 5:6:5 */
    m.PixelInformation.RedMask = 0xf800;
    m.PixelInformation.GreenMask = 0x07e0;
    m.PixelInformation.BlueMask = 0x001f;
    EXPECT(display_format(&m, &f));
    EXPECT(f.bpp == 16 && f.red_shift == 11 && f.red_bits == 5 && f.green_bits == 6 && f.blue_bits == 5);
    m.PixelInformation.GreenMask = 0x05e0;   /* a hole in a channel */
    EXPECT(!display_format(&m, &f));
    m.PixelInformation.GreenMask = 0;   /* a missing channel */
    EXPECT(!display_format(&m, &f));
    m.PixelFormat = PixelBltOnly;
    EXPECT(!display_format(&m, &f));
}

static void test_picks_the_first_usable_match(void)
{
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION a = mode(1024, 768, PixelBlueGreenRedReserved8BitPerColor);
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION blt = mode(1920, 1080, PixelBltOnly);
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION holes = mode(1920, 1080, PixelBitMask);
    holes.PixelInformation.RedMask = 0x00ff00ff;
    holes.PixelInformation.GreenMask = 0x0000ff00;
    holes.PixelInformation.BlueMask = 0x000000ff;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION good = mode(1920, 1080, PixelBlueGreenRedReserved8BitPerColor);
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION again = good;
    const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *modes[] = { &a, NULL, &blt, &holes, &good, &again };
    /* The 1920x1080 modes the handoff cannot describe come first and are passed over. */
    EXPECT(display_pick(modes, 6, 1920, 1080) == 4);
    EXPECT(display_pick(modes, 6, 1024, 768) == 0);
    EXPECT(display_pick(modes, 4, 1920, 1080) == UINT32_MAX);   /* only unusable ones */
    EXPECT(display_pick(modes, 6, 2560, 1440) == UINT32_MAX);   /* not offered */
    EXPECT(display_pick(modes, 0, 1024, 768) == UINT32_MAX);
}

static const struct host_test tests[] = {
    { "reads the preferred timing", test_reads_the_preferred_timing },
    { "refuses what is not a preference", test_refuses_what_is_not_a_preference },
    { "pixel layouts", test_pixel_layouts },
    { "picks the first usable match", test_picks_the_first_usable_match },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
