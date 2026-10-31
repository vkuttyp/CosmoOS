/*
 * fuzz_cputime.c - CPU-time accounting (kernel/process/cputime.c, generated
 * from CeeSafe) driven by arbitrary operations.
 *
 * The input is a program: each step's first byte picks a tick (user or
 * system), a setitimer, a getitimer or a split, and the next eight bytes
 * are its operand. Checked after every step: no CeeSafe check fails (a failed
 * one aborts through tests/host/ceesafe_panic.c), the exact scaling agrees
 * with 128-bit arithmetic, a split adds up to the run time and never goes
 * below an earlier one, a disarmed timer reads 0 and never fires, and an
 * armed one never reads 0.
 */

#include "fuzz.h"

#include <kernel/cputime.h>

#include <string.h>

#define TICK 4000000ull

static uint64_t operand(const uint8_t *data, size_t size, size_t *at)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | (*at < size ? data[(*at)++] : 0);
    return v;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct cputime ct;
    memset(&ct, 0, sizeof(ct));
    uint64_t run = 0;
    size_t at = 0;
    while (at < size) {
        uint8_t op = data[at++];
        uint64_t x = operand(data, size, &at);
        unsigned which = (op & 1) ? CPUTIME_ITIMER_PROF : CPUTIME_ITIMER_VIRTUAL;
        switch ((op >> 1) % 6) {
        case 0:
        case 1: {
            bool user = (op >> 1) % 6 == 0;
            bool virt_armed = ct.virt.expires_ns != 0, prof_armed = ct.prof.expires_ns != 0;
            bool virt_behind = virt_armed && ct.virt.expires_ns <= ct.sample.user_ns;
            unsigned due = cputime_tick(&ct, user, TICK);
            FUZZ_ASSERT(!(due & CPUTIME_DUE_VIRT) || virt_armed);
            FUZZ_ASSERT(!(due & CPUTIME_DUE_PROF) || prof_armed);
            /* System time never moves VIRTUAL's clock: on a system tick it
             * fires only to catch up, a periodic timer whose interval is
             * shorter than a tick firing once per tick, as Linux's does. */
            FUZZ_ASSERT(!(due & CPUTIME_DUE_VIRT) || user || virt_behind);
            break;
        }
        case 2: {
            uint64_t ov = 0, oi = 0, interval = x >> (x & 63);
            FUZZ_ASSERT(cputime_itimer_set(&ct, which, x, interval, TICK, &ov, &oi));
            uint64_t v = 0, i = 0;
            FUZZ_ASSERT(cputime_itimer_get(&ct, which, TICK, &v, &i));
            FUZZ_ASSERT(x == 0 ? (v == 0 && i == 0) : (v != 0 && i == interval));
            break;
        }
        case 3: {
            uint64_t v = 0, i = 0;
            FUZZ_ASSERT(cputime_itimer_get(&ct, which, TICK, &v, &i));
            const struct cputime_itimer *t = which == CPUTIME_ITIMER_PROF ? &ct.prof : &ct.virt;
            FUZZ_ASSERT((v == 0) == (t->expires_ns == 0));
            FUZZ_ASSERT(!cputime_itimer_get(&ct, (unsigned)(x | 4), TICK, &v, &i));
            break;
        }
        case 4: {
            run += x >> 20;   /* run time only grows */
            struct cputime_pair before = ct.reported;
            cputime_adjust(run, &ct.sample, &ct.reported);
            FUZZ_ASSERT(ct.reported.user_ns >= before.user_ns && ct.reported.system_ns >= before.system_ns);
            uint64_t sum = ct.reported.user_ns + ct.reported.system_ns;
            FUZZ_ASSERT(sum >= ct.reported.user_ns);   /* no wrap */
            FUZZ_ASSERT(before.user_ns + before.system_ns >= run ? sum == before.user_ns + before.system_ns : sum == run);
            break;
        }
        default: {
            uint64_t b = operand(data, size, &at), c = operand(data, size, &at);
            if (c == 0)
                c = 1;
            uint64_t a = x % c;
            FUZZ_ASSERT(cputime_scale(a, b, c) == (uint64_t)((unsigned __int128)a * b / c));
            if (x <= c)
                FUZZ_ASSERT(cputime_scale(x, b, c) == (uint64_t)((unsigned __int128)x * b / c));
            break;
        }
        }
    }
    return 0;
}

/* One step of a seed program: the op byte, then its operand big-endian. */
static size_t put_step(uint8_t *buf, size_t cap, size_t at, uint8_t op, uint64_t x)
{
    if (at + 9 > cap)
        return at;
    buf[at] = op;
    for (int i = 0; i < 8; i++)
        buf[at + 1 + (size_t)i] = (uint8_t)(x >> (56 - 8 * i));
    return at + 9;
}

/* Op bytes: (kind << 1) | timer, kind 0 a user tick, 1 a system tick,
 * 2 a set, 3 a get, 4 a split, 5 a scaling; timer 0 VIRTUAL, 1 PROF. */
size_t fuzz_seed(unsigned i, uint8_t *buf, size_t cap)
{
    size_t at = 0;
    switch (i) {
    case 0:   /* both timers periodic, then a mix of ticks with gets and splits */
        at = put_step(buf, cap, at, 2 << 1, 2 * TICK);
        at = put_step(buf, cap, at, (2 << 1) | 1, 3 * TICK);
        for (int k = 0; k < 12; k++) {
            at = put_step(buf, cap, at, (uint8_t)((k % 3 == 0 ? 1 : 0) << 1), 0);
            at = put_step(buf, cap, at, (uint8_t)((3 << 1) | (k & 1)), 0);
            at = put_step(buf, cap, at, 4 << 1, (uint64_t)k << 28);
        }
        return at;
    case 1:   /* "never", then clearing */
        at = put_step(buf, cap, at, 2 << 1, UINT64_MAX);
        at = put_step(buf, cap, at, 0, 0);
        at = put_step(buf, cap, at, 2 << 1, 0);
        return put_step(buf, cap, at, 3 << 1, 0);
    case 2:   /* scaling at the extremes */
        at = put_step(buf, cap, at, 5 << 1, UINT64_MAX);
        at = put_step(buf, cap, at, 0, UINT64_MAX);   /* the extra operands of a scaling */
        at = put_step(buf, cap, at, 0, UINT64_MAX - 1);
        return at;
    default:
        return 0;
    }
}
