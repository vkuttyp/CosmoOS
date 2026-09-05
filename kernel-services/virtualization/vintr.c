/*
 * vintr.c - VirtualInterrupt: the per-vCPU pending vector set
 * (docs/kernel-services/virtualization/design.md, "VirtualInterrupt").
 *
 * Stage 1 has no emulated interrupt controller: the owner makes a
 * vector pending, the lowest one is offered to the backend before each
 * entry and the hardware delivers it when the guest is interruptible.
 */

#include <kernel/errno.h>

#include "hv_internal.h"

void vintr_init(struct vcpu *v)
{
    spinlock_init(&v->irq_lock, "vcpu-irq");
    for (unsigned i = 0; i < 4; i++)
        v->pending[i] = 0;
    v->offered = -1;
}

int vcpu_inject(struct vcpu *v, unsigned vector)
{
    if (vector < 32 || vector > 255)
        return -EINVAL;
    arch_irq_state_t s = spin_lock_irqsave(&v->irq_lock);
    v->pending[vector / 64] |= 1ull << (vector % 64);
    spin_unlock_irqrestore(&v->irq_lock, s);
    return 0;
}

static int lowest_locked(const struct vcpu *v)
{
    for (unsigned w = 0; w < 4; w++) {
        if (v->pending[w])
            return (int)(w * 64 + (unsigned)__builtin_ctzll(v->pending[w]));
    }
    return -1;
}

int vintr_take_lowest(struct vcpu *v)
{
    arch_irq_state_t s = spin_lock_irqsave(&v->irq_lock);
    int r = lowest_locked(v);
    spin_unlock_irqrestore(&v->irq_lock, s);
    return r;
}

int vcpu_lowest_pending(struct vcpu *v)
{
    return vintr_take_lowest(v);
}

void vintr_clear(struct vcpu *v, int vector)
{
    if (vector < 0)
        return;
    arch_irq_state_t s = spin_lock_irqsave(&v->irq_lock);
    v->pending[(unsigned)vector / 64] &= ~(1ull << ((unsigned)vector % 64));
    spin_unlock_irqrestore(&v->irq_lock, s);
}

bool vintr_any(struct vcpu *v)
{
    return vintr_take_lowest(v) >= 0;
}
