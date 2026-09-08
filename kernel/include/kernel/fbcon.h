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
    uint32_t cols, rows;
    uint32_t fg, bg;       /* packed pixel values the sink draws with */
    uint32_t scroll_rows;  /* cell rows one scroll moves */
};

/* False when there is no framebuffer console. */
bool fbcon_geometry(struct fbcon_geometry *out);

/* The cursor, in cells. */
void fbcon_cursor(uint32_t *col, uint32_t *row);

#endif /* KERNEL_FBCON_H */
