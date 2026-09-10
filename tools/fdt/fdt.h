/*
 * fdt.h - A flattened-device-tree writer, and the reader a guest needs
 * (docs/kernel-services/virtualization/design.md, "The machine a guest
 * is handed").
 *
 * A writer only: a header, one empty memory reservation, a structure
 * block built by begin_node/prop/end_node calls into a caller's buffer,
 * a strings block appended at finish. No parsing of what it wrote, no
 * overlays, no reallocation -- libfdt does all that and is ten times the
 * size of what an owner needs to describe one machine. The reader, in
 * fdt_read.c, is what a guest needs: find a node by path, read a property.
 * It uses no library, so a freestanding guest can carry it.
 */
#ifndef COSMO_TOOLS_FDT_H
#define COSMO_TOOLS_FDT_H

#include <stddef.h>
#include <stdint.h>

#define FDT_MAGIC        0xd00dfeedu
#define FDT_BEGIN_NODE   1u
#define FDT_END_NODE     2u
#define FDT_PROP         3u
#define FDT_NOP          4u
#define FDT_END          9u
#define FDT_VERSION      17u
#define FDT_LAST_COMPAT  16u

struct fdt_writer {
    uint8_t *buf;
    size_t cap;
    size_t struct_off;      /* where the structure block starts in buf */
    size_t pos;             /* next byte of the structure block */
    char strings[2048];     /* the strings block, until finish appends it */
    size_t strings_len;
    int depth;
    int err;                /* the first error, sticky: -ENOSPC or -EINVAL */
};

int fdt_begin(struct fdt_writer *w, void *buf, size_t cap);
int fdt_begin_node(struct fdt_writer *w, const char *name);
int fdt_prop(struct fdt_writer *w, const char *name, const void *val, uint32_t len);
int fdt_prop_u32(struct fdt_writer *w, const char *name, uint32_t v);
int fdt_prop_u64(struct fdt_writer *w, const char *name, uint64_t v);   /* as two cells */
int fdt_prop_cells(struct fdt_writer *w, const char *name, const uint32_t *cells, unsigned n);
int fdt_prop_str(struct fdt_writer *w, const char *name, const char *s);
int fdt_prop_strs(struct fdt_writer *w, const char *name, const char *const *s, unsigned n);
int fdt_prop_empty(struct fdt_writer *w, const char *name);
int fdt_end_node(struct fdt_writer *w);
int fdt_finish(struct fdt_writer *w, size_t *size);

/* The machine the CosmoOS hypervisor implements, from cosmo/hv_machine.h:
 * `nr_cpus` vCPUs, `ram_bytes` at `ram_base`, `bootargs` (may be NULL). */
int fdt_cosmo_virt(void *buf, size_t cap, unsigned nr_cpus, uint64_t ram_base, uint64_t ram_bytes,
                   const char *bootargs, size_t *size);

/* --- the reader (fdt_read.c) ------------------------------------------ */

uint32_t fdt_be32(const void *p);
uint64_t fdt_be64(const void *p);
/* The blob's totalsize if its header is sound and fits in `cap`, else 0. */
uint32_t fdt_check(const void *blob, size_t cap);
/* The property `name` of the node at `path` ("/", "/cpus/cpu@0"), or NULL. */
const void *fdt_get_prop(const void *blob, const char *path, const char *name, uint32_t *len);
/* How many children of the node at `path` have names starting `prefix`. */
unsigned fdt_count_children(const void *blob, const char *path, const char *prefix);

#endif /* COSMO_TOOLS_FDT_H */
