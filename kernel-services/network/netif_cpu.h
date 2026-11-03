/*
 * netif_cpu.h - The interface layer's own state (kernel-services/network/netif.cee): one receive queue, work list and
 * worker per CPU, and the barrier items net_workers_barrier posts to them. In a header, rather than netif's file, so
 * CeeSafe can import the C layout it defines statics of (CeeSafe's type-system.md §31, ADR 0042). Private to netif:
 * beside it, not under kernel/include; its generated C and its host test reach it with -I kernel-services/network.
 */

#ifndef KERNEL_SERVICES_NETWORK_NETIF_CPU_H
#define KERNEL_SERVICES_NETWORK_NETIF_CPU_H

#include <kernel/completion.h>
#include <kernel/faultinject.h>
#include <kernel/list.h>
#include <kernel/mbuf.h>
#include <kernel/netif.h>
#include <kernel/percpu.h>
#include <kernel/spinlock.h>
#include <kernel/thread.h>
#include <kernel/wait.h>

/* One CPU's queue, work list and worker (unit 11). CPU 0's worker starts in net_init, the others in net_start_workers
 * after SMP bring-up; a packet steered to a CPU whose worker is not ready yet goes to CPU 0. */
struct net_cpu {
    struct mbufq rxq;
    struct list_node work;
    spinlock_t work_lock;
    struct waitqueue wq;
    struct wait_entry waiting;   /* the worker's entry while it sleeps on wq */
    struct thread *worker;
    uint32_t ready;              /* set by the worker, read by every CPU (atomic) */
    struct net_cpu_stats stats;
    unsigned id;
    char name[16];
};

struct net_cpus {
    struct net_cpu cpu[CONFIG_MAX_CPUS];
};

/* A worker barrier: runs on the network thread after everything queued before it. */
struct net_barrier {
    struct net_work work;
    struct completion done;
};

struct net_barriers {
    struct net_barrier barrier[CONFIG_MAX_CPUS];
};

/*
 * The worker's priority, decided by measurement (docs/audit/next-subsystem-wake-preempt.md, "The worker's priority"):
 * at 40, below default, a default-priority sender on the worker's own CPU was never displaced by it; at 31, above
 * default, the worker preempted its feeder on every enqueue (one-flow TCP fell to 12 MiB/s on AArch64, and one x86-64
 * boot in five hung); at 32, equal, every figure was within 40's spread or above it. Five boots per setting per
 * architecture, 2026-09-14; the table is in docs/kernel-services/network/design.md. Overridable for a reproduction:
 * make EXTRA_CFLAGS=-DNET_WORKER_PRIO=31 test.
 */
#ifndef NET_WORKER_PRIO
#define NET_WORKER_PRIO SCHED_PRIO_DEFAULT
#endif

/* C constant macros for CeeSafe (its type-system.md §39.4). */
#define NETIF_WORKER_PRIO() (NET_WORKER_PRIO)
#define NETIF_FI_RX_DUP() (FI_NET_RX_DUP)

#endif /* KERNEL_SERVICES_NETWORK_NETIF_CPU_H */
