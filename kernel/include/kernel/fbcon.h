/*
 * fbcon.h - The framebuffer console sink (kernel/core/fbcon.c).
 *
 * fbcon_init() maps the framebuffer the loader reported (boot protocol
 * v6), shows the newest screenful of the log ring, and registers a
 * console sink. A machine without a framebuffer keeps the serial console
 * and nothing here reports an error.
 *
 * The geometry and cursor accessors exist for the self-test, which reads
 * pixels back to check what was drawn; nothing else should need them.
 */

#ifndef KERNEL_FBCON_H
#define KERNEL_FBCON_H

#include <stdbool.h>
#include <stdint.h>

void fbcon_init(void);
/* The framebuffer as the console from the first line, when there is no
 * serial console (kernel_main): through the loader's direct map and a
 * static buffer, no allocation. Needs the architecture to keep that range
 * mapped in its own tables (AArch64: map_early_devices). */
void fbcon_early_init(void);
bool fbcon_present(void);

struct fbcon_stats {
    uint64_t bytes;     /* bytes handed to the sink */
    uint64_t glyphs;    /* cells drawn */
    uint64_t scrolls;
    uint64_t repaints;
};

void fbcon_get_stats(struct fbcon_stats *out);

struct fbcon_geometry {
    volatile void *base;   /* first visible pixel */
    uint32_t pitch;        /* bytes per pixel row */
    uint32_t bytes_pp;
    uint32_t width, height;
    uint32_t scale;        /* a font pixel is scale x scale screen pixels */
    uint32_t cols, rows;
    uint32_t fg, bg;       /* packed pixel values the sink draws with */
    uint32_t scroll_rows;  /* cell rows one scroll moves */
};

/* False when there is no framebuffer console. */
bool fbcon_geometry(struct fbcon_geometry *out);

/* The cursor, in cells. */
void fbcon_cursor(uint32_t *col, uint32_t *row);

/*
 * A display driver's memory for the console (the virtio-gpu unit). The
 * driver keeps the struct alive while attached. 32-bit pixels only. The
 * device shows the memory only when the driver flushes it: the sink
 * records what it drew, the driver takes that band with fbcon_take_damage
 * from a thread and flushes it. In panic mode the sink calls panic_flush
 * itself, with the band, after each write: from panic context, with no
 * lock, interrupts in any state and no thread to hand to.
 */
struct fbcon_surface {
    void *base;              /* first pixel, kernel virtual, write-back memory */
    uint32_t width, height;  /* pixels */
    uint32_t pitch;          /* bytes per row */
    uint32_t bpp;            /* 32 */
    uint8_t red_shift, red_bits;
    uint8_t green_shift, green_bits;
    uint8_t blue_shift, blue_bits;
    void (*panic_flush)(void *arg, uint32_t y0, uint32_t y1);
    void *arg;
};

/* Move the console onto `s`: cleared, then the newest screenful of the
 * log. The previous surface, if any, is dropped (its owner frees it after
 * this returns). Thread context. 0, -EINVAL (not a usable surface) or
 * -ENOMEM. */
int fbcon_attach(const struct fbcon_surface *s);
/* Leave `s` (no effect if it is not the one attached): back to the boot
 * framebuffer, or to no screen console when there was none. On return no
 * sink write touches `s`. Thread context. */
void fbcon_detach(const struct fbcon_surface *s);
/* The pixel rows [y0, y1) drawn on `s` since the last call, cleared;
 * false when there are none (or `s` is not attached). Thread context. */
bool fbcon_take_damage(const struct fbcon_surface *s, uint32_t *y0, uint32_t *y1);

#endif /* KERNEL_FBCON_H */
