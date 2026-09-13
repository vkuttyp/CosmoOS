/*
 * hvkick.h - the two words that let one thread stop another's vCPU
 * (docs/audit/next-subsystem-vcpu-threads.md).
 *
 * A vCPU inside `arch_hv_vcpu_run` cannot be reached by anything but a host
 * interrupt, so stopping one means setting a flag and sending an IPI to the
 * CPU it is on. Both halves are needed and neither is sufficient, which is
 * why this is a handshake rather than a flag:
 *
 *   the runner                          the kicker (SYS_vcpu_stop)
 *   ----------                          --------------------------
 *   publish in_guest = cpu | IN_GUEST   store stop = 1
 *   read stop; abandon the entry if set read in_guest; IPI the cpu it names
 *   enter the guest
 *   clear in_guest after the exit
 *
 * **Both store-then-load pairs are sequentially consistent, and that is
 * load-bearing.** Release/acquire permits a store to be reordered with a
 * later load, and if both pairs slip then both sides miss: the kicker reads
 * an `in_guest` from before the publication and sends no IPI, while the
 * runner reads a `stop` from before the store and enters the guest. A
 * spinning guest would then run forever with its stop pending. A single
 * total order over the four accesses cannot contain both misses -- if the
 * kicker's load precedes the runner's store then the kicker's store
 * precedes that load and so precedes the runner's store, so the runner's
 * load must see it, and two misses would need a cycle. This is Dekker's
 * handshake; do not weaken it to acquire/release.
 *
 * The publication has to sit **inside the backend's interrupt-disabled
 * region, immediately around the guest entry**, and not in the generic loop
 * that calls it. That is not tidiness: an IPI that arrives after the
 * publication must either abandon the entry or leave the guest, and what
 * makes the second true is that interrupts are disabled from before the
 * publication until after the entry, so the IPI stays pending and the
 * architecture's external-interrupt exiting (`PIN_EXT_INTR_EXITING` on
 * x86-64, `HCR_EL2.IMO` on AArch64) turns it into an immediate exit.
 * Published in the generic loop instead, an IPI could be taken as an
 * ordinary host interrupt *before* the entry and leave the guest running.
 */

#ifndef KERNEL_HVKICK_H
#define KERNEL_HVKICK_H

#include <stdbool.h>

#define HV_IN_GUEST 0x80000000u   /* set in `in_guest` while a guest is entered */

struct hv_kick {
    unsigned stop;       /* the owner asked this vCPU to leave its run */
    unsigned in_guest;   /* HV_IN_GUEST | the host CPU, or 0 */
};

/* The runner, immediately before the entry and inside IRQs-off. */
static inline void hv_kick_entering(struct hv_kick *k, unsigned cpu)
{
    __atomic_store_n(&k->in_guest, HV_IN_GUEST | cpu, __ATOMIC_SEQ_CST);
}

/* The runner's half of the handshake: after publishing, before entering. */
static inline bool hv_kick_stopped(const struct hv_kick *k)
{
    return __atomic_load_n(&k->stop, __ATOMIC_SEQ_CST) != 0;
}

/* The runner, immediately after the exit. */
static inline void hv_kick_left(struct hv_kick *k)
{
    __atomic_store_n(&k->in_guest, 0, __ATOMIC_RELEASE);
}

/* The runner, outside the entry: take the stop if one is pending. Consumed
 * by exchange rather than read-then-clear so that a stop arriving inside
 * the window is not swallowed -- the exchange either returns it, and this
 * run stops, or lands after it, and the next run does. */
static inline bool hv_kick_take(struct hv_kick *k)
{
    return __atomic_exchange_n(&k->stop, 0u, __ATOMIC_ACQ_REL) != 0;
}

#endif /* KERNEL_HVKICK_H */
