/*
 * itimer.c - ITIMER_REAL: SIGALRM after a delay, and again every interval
 * (setitimer, alarm; docs/kernel/process/design.md, "Interval timer").
 *
 * One kernel timer per process. The callback runs in interrupt context,
 * re-arms itself by the interval and sends SIGALRM -- signal_send takes
 * only the process's spinlock and wakes, which the terminal's ^C already
 * does from interrupt context (tty_input). The setters hold itimer_lock and
 * cancel synchronously before re-arming, so a start never meets a pending
 * timer, and exit cancels synchronously, so no callback can touch a process
 * after it is freed.
 */

#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/timer.h>

static void itimer_fire(struct timer *t, void *arg)
{
    struct process *p = arg;
    uint64_t iv = __atomic_load_n(&p->itimer_interval_ns, __ATOMIC_RELAXED);
    if (iv != 0) {
        __atomic_store_n(&p->itimer_expires_ns, clock_deadline_ns(iv), __ATOMIC_RELAXED);
        timer_start(t, iv);
    } else {
        __atomic_store_n(&p->itimer_expires_ns, 0, __ATOMIC_RELAXED);
    }
    struct signal_info info = { .sig = SIGALRM, .source = SIGSRC_KERNEL };
    signal_send(p, SIGALRM, &info);
}

void process_itimer_init(struct process *p)
{
    timer_setup(&p->itimer, itimer_fire, p);
    mutex_init(&p->itimer_lock, "itimer");
}

/* The time left, never 0 for an armed timer whose callback has not run.
 * The deadline is in clock_deadline_ns's domain (timer.h), which a
 * migration between arming and asking does not change; "now" is read in
 * the same domain, as clock_deadline_ns(0). */
static uint64_t remaining(struct process *p)
{
    uint64_t exp = __atomic_load_n(&p->itimer_expires_ns, __ATOMIC_RELAXED);
    if (exp == 0)
        return 0;
    uint64_t now = clock_deadline_ns(0);
    return exp > now ? exp - now : 1;
}

void process_itimer_set(struct process *p, uint64_t value_ns, uint64_t interval_ns, uint64_t *old_value,
                        uint64_t *old_interval)
{
    mutex_lock(&p->itimer_lock);
    timer_cancel_sync(&p->itimer);   /* re-cancels if the callback re-armed meanwhile */
    if (old_value)
        *old_value = remaining(p);
    if (old_interval)
        *old_interval = __atomic_load_n(&p->itimer_interval_ns, __ATOMIC_RELAXED);
    if (value_ns != 0) {
        __atomic_store_n(&p->itimer_interval_ns, interval_ns, __ATOMIC_RELAXED);
        __atomic_store_n(&p->itimer_expires_ns, clock_deadline_ns(value_ns), __ATOMIC_RELAXED);
        timer_start(&p->itimer, value_ns);
    } else {
        __atomic_store_n(&p->itimer_interval_ns, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&p->itimer_expires_ns, 0, __ATOMIC_RELAXED);
    }
    mutex_unlock(&p->itimer_lock);
}

void process_itimer_get(struct process *p, uint64_t *value, uint64_t *interval)
{
    mutex_lock(&p->itimer_lock);
    *value = remaining(p);
    *interval = __atomic_load_n(&p->itimer_interval_ns, __ATOMIC_RELAXED);
    mutex_unlock(&p->itimer_lock);
}

void process_itimer_stop(struct process *p)
{
    timer_cancel_sync(&p->itimer);
    __atomic_store_n(&p->itimer_interval_ns, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&p->itimer_expires_ns, 0, __ATOMIC_RELAXED);
}
