/*
 * tcp_floor.c - The C under kernel-services/network/tcp.cee (roadmap 2.0 SA6).
 *
 * TCP is CeeSafe (tcp.cee). What stays here is what CeeSafe cannot hold, each
 * behind a function tcp.cee calls:
 *
 *   - the pcb fields only some builds have: the debug builds' liveness mark
 *     (test_mark) and the self-test builds' retransmission arming time
 *     (rexmit_armed_ns) -- a CeeSafe declaration of a struct's field must
 *     hold in every build;
 *   - the host test's state-change hook, which a host test or fuzz target
 *     defines when built with TCP_HOST_TEST and nothing defines otherwise;
 *   - the debug builds' self-test seams: a timer callback held before it
 *     takes its reference (docs/audit/next-subsystem-lifetime-windows.md)
 *     and a client's final ACK held unprocessed
 *     (docs/audit/next-subsystem-accept-order.md), as tcp.c had them.
 */

#include <kernel/net/tcp.h>
#include <kernel/netif.h>
#include <kernel/sched.h>

#include <arch/cpu.h>

/* Declared for tcp.cee only; not part of tcp.h's interface. */
void tcp_floor_state_change(struct tcp_pcb *pcb, uint32_t from, uint32_t to);
void tcp_floor_mark_live(struct tcp_pcb *pcb);
void tcp_floor_mark_dead(struct tcp_pcb *pcb);
void tcp_floor_kick_hook(struct tcp_pcb *pcb);
void tcp_floor_rexmit_armed(struct tcp_pcb *pcb, uint64_t ns);
uint64_t tcp_floor_rexmit_armed_at(struct tcp_pcb *pcb);
bool tcp_floor_hold_ack(struct netif *nif, struct mbuf *m, const struct ipv4_hdr *ip4, uint16_t dport, uint16_t sport,
                        uint8_t flags, uint32_t hlen, uint32_t len);

#ifdef TCP_HOST_TEST
/* The host targets' oracle (tests/fuzz/fuzz_tcp_segments.c, tests/host/test_tcp.c): every state change passes here,
 * before it is stored. Not compiled into the kernel. */
void tcp_test_state_change(struct tcp_pcb *pcb, enum tcp_state from, enum tcp_state to);
#endif

void tcp_floor_state_change(struct tcp_pcb *pcb, uint32_t from, uint32_t to)
{
#ifdef TCP_HOST_TEST
    tcp_test_state_change(pcb, (enum tcp_state)from, (enum tcp_state)to);
#else
    (void)pcb;
    (void)from;
    (void)to;
#endif
}

void tcp_floor_rexmit_armed(struct tcp_pcb *pcb, uint64_t ns)
{
#if CONFIG_SELFTEST
    pcb->rexmit_armed_ns = ns;
#else
    (void)pcb;
    (void)ns;
#endif
}

uint64_t tcp_floor_rexmit_armed_at(struct tcp_pcb *pcb)
{
#if CONFIG_SELFTEST
    return pcb->rexmit_armed_ns;
#else
    (void)pcb;
    return 0;
#endif
}

#if CONFIG_DEBUG
/*
 * The timer-callback/free window
 * (docs/audit/next-subsystem-lifetime-windows.md).
 *
 * The hook holds a callback *before* it takes its reference, which is
 * the only placement that tests what it claims to: timer_kick's first
 * act is pcb_get, so a hold after that is protected by reference
 * counting and would pass with timer_cancel_sync stubbed out. Held where
 * it is, the interval belongs to synchronous cancellation alone.
 *
 * The pcb is poisoned as it is freed and the callback checks the mark
 * again *after* the hold, not only on entry: an entry-only check passes
 * and then says nothing about what happened during the interval.
 */
#define TCP_PCB_LIVE 0x5450434Bu   /* 'TPCK' */
#define TCP_PCB_DEAD 0x44454144u   /* 'DEAD' */

static struct tcp_pcb *g_test_hold_cb;   /* hold this pcb's next callback; NULL: none */
static unsigned g_test_cb_passed;    /* other pcbs' callbacks let through while armed */
static unsigned g_test_cb_entered;   /* it is inside, before its pcb_get */
static unsigned g_test_cb_release;   /* let it go */
static unsigned g_test_cb_saw_dead;  /* it found the poison: the bug */
static unsigned g_test_cb_checked;   /* liveness checks that passed after the hold */
static unsigned g_test_cb_cpu;       /* the CPU the held callback ran on */

/* Armed by identity: a callback is held only if it is `pcb`'s. The
 * first version held the next callback of *any* pcb, and a connection
 * an earlier test left behind fired first and was held in the test's
 * place (docs/testing/flakes.md, "tcp-pcb-timer-free held a stranger's
 * callback"). The caller's reference keeps `pcb` alive while armed, so
 * no other pcb can share its address. */
void tcp_test_hold_callback(struct tcp_pcb *pcb)
{
    __atomic_store_n(&g_test_cb_passed, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_test_cb_entered, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_test_cb_release, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_test_cb_saw_dead, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_test_cb_checked, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_test_cb_cpu, ~0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_test_hold_cb, pcb, __ATOMIC_RELEASE);
}
unsigned tcp_test_callback_passed(void) { return __atomic_load_n(&g_test_cb_passed, __ATOMIC_ACQUIRE); }
bool tcp_test_callback_entered(void) { return __atomic_load_n(&g_test_cb_entered, __ATOMIC_ACQUIRE) != 0; }
void tcp_test_release_callback(void) { __atomic_store_n(&g_test_cb_release, 1u, __ATOMIC_RELEASE); }
unsigned tcp_test_callback_saw_dead(void) { return __atomic_load_n(&g_test_cb_saw_dead, __ATOMIC_ACQUIRE); }
unsigned tcp_test_callback_checked(void) { return __atomic_load_n(&g_test_cb_checked, __ATOMIC_ACQUIRE); }
unsigned tcp_test_callback_cpu(void) { return __atomic_load_n(&g_test_cb_cpu, __ATOMIC_ACQUIRE); }

/* Arm the pcb's rexmit timer. Must be called from the CPU the callback
 * should run on: a timer lands on the queue of the CPU that starts it,
 * and this window needs the callback on a CPU other than the closer's. */
void tcp_test_arm_rexmit(struct tcp_pcb *pcb, uint64_t ns)
{
    timer_cancel(&pcb->rexmit);
    timer_start(&pcb->rexmit, ns);
}

/*
 * Hold one client's final ACK to a listener, unprocessed, until the test
 * delivers it (docs/audit/next-subsystem-accept-order.md). A connection
 * joins its listener's accept queue when the listener processes that ACK,
 * so holding the first client's while the second's handshake completes
 * makes the later `connect()` the first accepted -- an order the network
 * workers produce on their own only when a host stalls one of them, made
 * certain.
 *
 * Armed by both ports: the listener's and the held client's. The first
 * version matched the listener port alone and would have held whichever
 * client's ACK reached tcp_input first -- the second's, in the very race
 * the test forces (Greptile, #269).
 *
 * One state word decides who owns the segment. The hook moves ARMED to
 * CLAIMING, fills the fields, and publishes HELD; delivery either disarms
 * an unclaimed hold (ARMED to IDLE) or waits out a claim in progress and
 * takes the segment. So a delivery can never miss a segment that a worker
 * is between claiming and storing (Greptile, #269). Taken in tcp_input
 * before any lock, so the hold delays that one segment and nothing else.
 */
enum { HOLD_IDLE, HOLD_ARMED, HOLD_CLAIMING, HOLD_HELD };
static unsigned g_test_hold_state;
static uint16_t g_test_hold_dport, g_test_hold_sport;
static struct mbuf *g_test_held_ack;
static struct ipv4_hdr g_test_held_ip4; /* what delivering it needs back */
static struct netif *g_test_held_nif;

void tcp_test_hold_ack(uint16_t dport, uint16_t sport)
{
    g_test_hold_dport = dport;
    g_test_hold_sport = sport;
    __atomic_store_n(&g_test_hold_state, (unsigned)HOLD_ARMED, __ATOMIC_RELEASE);   /* the ports are visible with it */
}

bool tcp_test_ack_held(void)
{
    return __atomic_load_n(&g_test_hold_state, __ATOMIC_ACQUIRE) == HOLD_HELD;
}

bool tcp_test_deliver_held_ack(void)
{
    unsigned st = HOLD_ARMED;
    if (__atomic_compare_exchange_n(&g_test_hold_state, &st, (unsigned)HOLD_IDLE, false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE))
        return false;   /* never claimed: disarmed */
    while ((st = __atomic_load_n(&g_test_hold_state, __ATOMIC_ACQUIRE)) == HOLD_CLAIMING)
        sched_yield();   /* a worker is between the claim and the store: a few instructions */
    if (st != HOLD_HELD)
        return false;   /* idle: nothing armed */
    struct mbuf *m = g_test_held_ack;
    g_test_held_ack = NULL;
    __atomic_store_n(&g_test_hold_state, (unsigned)HOLD_IDLE, __ATOMIC_RELEASE);
    tcp_input(g_test_held_nif, m, &g_test_held_ip4, NULL);   /* idle: it is not held again */
    return true;
}

void tcp_floor_mark_live(struct tcp_pcb *pcb)
{
    __atomic_store_n(&pcb->test_mark, TCP_PCB_LIVE, __ATOMIC_RELEASE);
}

/* Poisoned before the free, so a callback still inside reads a mark rather than whatever the allocator hands out
 * next. */
void tcp_floor_mark_dead(struct tcp_pcb *pcb)
{
    pcb->test_mark = TCP_PCB_DEAD;
}

/* In the timer callback, before its pcb_get: for this interval nothing but timer_cancel_sync refusing to return keeps
 * the pcb alive. */
void tcp_floor_kick_hook(struct tcp_pcb *pcb)
{
    {
        struct tcp_pcb *want = pcb;
        if (__atomic_compare_exchange_n(&g_test_hold_cb, &want, NULL, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            /* Before pcb_get: for this interval nothing but
             * timer_cancel_sync refusing to return keeps the pcb alive. */
            __atomic_store_n(&g_test_cb_cpu, arch_cpu_id(), __ATOMIC_RELEASE);
            __atomic_store_n(&g_test_cb_entered, 1u, __ATOMIC_RELEASE);
            while (!__atomic_load_n(&g_test_cb_release, __ATOMIC_ACQUIRE))
                arch_cpu_relax();
            if (__atomic_load_n(&pcb->test_mark, __ATOMIC_ACQUIRE) != TCP_PCB_LIVE)
                __atomic_fetch_add(&g_test_cb_saw_dead, 1u, __ATOMIC_ACQ_REL);
            else
                __atomic_fetch_add(&g_test_cb_checked, 1u, __ATOMIC_ACQ_REL);
        } else if (want != NULL) {
            __atomic_fetch_add(&g_test_cb_passed, 1u, __ATOMIC_ACQ_REL);   /* armed, for another pcb */
        }
    }
}

/* In tcp_input: take `m` if it is the armed client's bare ACK to the armed port. */
bool tcp_floor_hold_ack(struct netif *nif, struct mbuf *m, const struct ipv4_hdr *ip4, uint16_t dport, uint16_t sport,
                        uint8_t flags, uint32_t hlen, uint32_t len)
{
    if (__atomic_load_n(&g_test_hold_state, __ATOMIC_ACQUIRE) != HOLD_ARMED || ip4 == NULL ||
        dport != g_test_hold_dport || sport != g_test_hold_sport || len != hlen ||
        (flags & (TH_SYN | TH_ACK | TH_FIN | TH_RST)) != TH_ACK)
        return false;
    unsigned st = HOLD_ARMED;
    if (!__atomic_compare_exchange_n(&g_test_hold_state, &st, (unsigned)HOLD_CLAIMING, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return false;   /* delivery disarmed it first */
    g_test_held_ip4 = *ip4;
    g_test_held_nif = nif;
    g_test_held_ack = m;
    __atomic_store_n(&g_test_hold_state, (unsigned)HOLD_HELD, __ATOMIC_RELEASE);   /* the fields are visible with it */
    return true;
}
#else
void tcp_floor_mark_live(struct tcp_pcb *pcb) { (void)pcb; }
void tcp_floor_mark_dead(struct tcp_pcb *pcb) { (void)pcb; }
void tcp_floor_kick_hook(struct tcp_pcb *pcb) { (void)pcb; }
bool tcp_floor_hold_ack(struct netif *nif, struct mbuf *m, const struct ipv4_hdr *ip4, uint16_t dport, uint16_t sport,
                        uint8_t flags, uint32_t hlen, uint32_t len)
{
    (void)nif;
    (void)m;
    (void)ip4;
    (void)dport;
    (void)sport;
    (void)flags;
    (void)hlen;
    (void)len;
    return false;
}
#endif
