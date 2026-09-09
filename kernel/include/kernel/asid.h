/*
 * asid.h - Address-space tags (kernel/memory/asid.c).
 *
 * A tag lets the TLB keep one address space's translations while another
 * runs, so a process switch need not empty it. The hardware calls them
 * ASIDs on AArch64 and PCIDs on x86-64; the difference is a width, which
 * `arch_mmu_asid_bits()` reports, so this allocator is generic.
 *
 * Tags are a small resource -- 255 at eight bits, 65,535 at sixteen,
 * 4,095 on x86-64 -- and the machine may hold more address spaces than
 * there are tags. The classic answer is generations: when the last tag
 * is handed out the generation advances and every tag issued before it
 * becomes stale by arithmetic rather than by a visit to every space. A
 * space with a stale tag is given a fresh one at its next switch-in, and
 * a CPU that has not flushed since the generation advanced flushes
 * everything before it uses any tag of the new one.
 */

#ifndef KERNEL_ASID_H
#define KERNEL_ASID_H

#include <kernel/types.h>

struct arch_mmu_context;

struct asid_stats {
    uint64_t allocs;      /* tags handed out */
    uint64_t rollovers;   /* generations advanced */
    uint64_t releases;    /* tags returned by a destroyed space */
    uint64_t flushes;     /* CPUs that flushed everything because of a rollover */
};

/* Once, from vmm_init, after the boot CPU's features are set. `bits` is
 * `arch_mmu_asid_bits()`; 0 disables tagging entirely and every call
 * below then reports "no tag, flush". */
void asid_init(unsigned bits);

/*
 * Give `ctx` a tag valid in the current generation if it has not got
 * one, and tell the caller whether *this CPU* must flush every tag
 * before using it -- true when the generation advanced since this CPU
 * last flushed. Callers switch address spaces with interrupts off, which
 * is what keeps the answer true until it is used.
 *
 * `ctx` is NULL for the kernel's own root, which runs under tag 0 and
 * must never be given one of its own: `arch_mmu_activate` ignores the
 * tag for a kernel context, so allocating one would consume a tag per
 * generation that nothing would ever release. The flush question is
 * still asked and answered.
 */
bool asid_switch_prepare(struct arch_mmu_context *ctx);

/* Return a destroyed space's tag. The caller has already invalidated the
 * tag's translations everywhere they might be held: releasing first
 * would let the next owner of the tag inherit them. */
void asid_release(struct arch_mmu_context *ctx);

uint64_t asid_generation(void);
void asid_get_stats(struct asid_stats *out);

/*
 * Paranoid mode (debug builds): keep tagging, but tell every switch to
 * flush anyway, so no translation survives one. Any isolation failure
 * that appears with tags trusted and vanishes here is a stale
 * translation by construction -- it is the bisecting tool for the one
 * class of bug this whole mechanism can introduce. It is also how the
 * benchmark gets its "before" number without a second build.
 */
void asid_set_paranoid(bool on);
/* Read `opt/cosmo/asid` from the boot: `paranoid` turns the above on
 * for the whole run. Called once, after asid_init. */
void asid_boot_config(void);
bool asid_paranoid(void);

/*
 * Self-tests only: advance the generation exactly as exhausting the pool
 * would, without allocating anything. Reaching a rollover by allocating
 * would call `asid_switch_prepare` on this CPU, which stamps it as
 * flushed -- and a test of the rollover flush must not consume the very
 * flush it is testing.
 */
bool asid_test_force_rollover(void);

/* Self-tests only: run the allocator at a narrower width than the
 * machine's, so that rollover can be reached in a test rather than in
 * the sixty-five-thousandth process. Returns false outside debug
 * builds. */
bool asid_test_set_bits(unsigned bits);

#endif /* KERNEL_ASID_H */
