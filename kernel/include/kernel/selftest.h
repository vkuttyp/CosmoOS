/*
 * selftest.h - Boot-time kernel self-tests.
 *
 * Runs when CONFIG_SELFTEST=1 (debug builds by default). Each test prints
 * one "SELFTEST: <name> ... ok|FAIL" line and the run ends with
 * "SELFTEST: PASS (n tests)" or "SELFTEST: FAIL (k of n)". The QEMU
 * harness keys on these lines. Tests must leave the kernel in the state
 * they found it.
 */

#ifndef KERNEL_SELFTEST_H
#define KERNEL_SELFTEST_H

#include <stdbool.h>

/* Returns the number of failed tests. */
int selftest_run_all(void);

/* Test entry points provided by subsystems. Each returns true on success
 * or sets *reason to an immortal string and returns false. */
bool selftest_pmm(const char **reason);
bool selftest_vmm(const char **reason);
bool selftest_user_vmm(const char **reason);   /* kernel/memory/memtest.c: user regions, PROT_NONE, split/merge, shootdown mask */
bool selftest_rlimit(const char **reason);     /* kernel/memory/memtest.c: address-space, memory and handle limits */
bool selftest_uaccess(const char **reason);    /* kernel/syscall/uaccesstest.c: exception fixups */
bool selftest_kmalloc(const char **reason);

/* Phase 3: kernel/scheduler/schedtest.c */
bool selftest_acpi(const char **reason);
bool selftest_timer(const char **reason);
bool selftest_irq_route(const char **reason);
bool selftest_thread(const char **reason);
bool selftest_yield(const char **reason);
bool selftest_preempt(const char **reason);
bool selftest_sleep(const char **reason);
bool selftest_mutex(const char **reason);
bool selftest_semaphore(const char **reason);
bool selftest_completion(const char **reason);
bool selftest_completion_race(const char **reason);   /* a completion freed by its waiter while complete() still runs */
bool selftest_waitqueue(const char **reason);

/* Phase 3 part 2: kernel/scheduler/smptest.c */
bool selftest_smp_online(const char **reason);
bool selftest_smp_affinity(const char **reason);
bool selftest_smp_parallel(const char **reason);
bool selftest_smp_call(const char **reason);
bool selftest_smp_shootdown(const char **reason);
bool selftest_smp_wake(const char **reason);
bool selftest_smp_ticks(const char **reason);
bool selftest_smp_mutex(const char **reason);
bool selftest_smp_ipi_storm(const char **reason);
bool selftest_quiesce_grace(const char **reason);
bool selftest_quiesce_call(const char **reason);
bool selftest_irq_sync(const char **reason);
bool selftest_timer_cancel_sync(const char **reason);
bool selftest_quiesce_stress(const char **reason);
bool selftest_lockdep_order(const char **reason);
bool selftest_lockdep_recursion(const char **reason);
bool selftest_lockdep_irq(const char **reason);
bool selftest_lockdep_sleep(const char **reason);
bool selftest_lockdep_mutex(const char **reason);
bool selftest_lockdep_contention(const char **reason);
bool selftest_vfs_concurrency(const char **reason);
bool selftest_vfs_put_race(const char **reason);   /* the last references of one vnode dropped from every CPU at once */
bool selftest_mountns(const char **reason);
bool selftest_utsns(const char **reason);
bool selftest_fault_kmalloc(const char **reason);
bool selftest_fault_blk(const char **reason);
bool selftest_blk_queue(const char **reason);   /* kernel/block/blktest.c: the pending queue and bio flags */
bool selftest_blk_segments(const char **reason); /* multi-segment bios (milestone 9) */
bool selftest_blk_timeout(const char **reason);  /* a stalled request times out */
bool selftest_cosmofs_replay(const char **reason);
bool selftest_syscall_fuzz(const char **reason);
bool selftest_io_poll(const char **reason);    /* kernel/io/polltest.c: io_poll (milestone 10) */
bool selftest_realtime(const char **reason);   /* kernel/io/polltest.c: the wall clock */
bool selftest_iommu(const char **reason);      /* kernel/iommu/iommutest.c: domains, IOVA, the DMA API (unit 12) */

/* Phase 4: kernel/process/proctest.c */
bool selftest_objects(const char **reason);
bool selftest_elf(const char **reason);
bool selftest_process_reject(const char **reason);
bool selftest_process_selftest(const char **reason);
bool selftest_process_fault(const char **reason);
bool selftest_signal_native(const char **reason);      /* a handler and its return, at a syscall */
bool selftest_signal_async(const char **reason);       /* ... at an interrupt, with a child sending */
bool selftest_signal_mask(const char **reason);        /* blocked, pending, delivered at the unblock */
bool selftest_signal_fault(const char **reason);       /* ... at a fault, with the address in siginfo */
bool selftest_signal_group(const char **reason);        /* kill(-pgid) reaches the group and nothing else */
bool selftest_signal_setsid(const char **reason);      /* a session is started once */
bool selftest_tty_intr(const char **reason);           /* ^C to the terminal's foreground group */
bool selftest_process_efault(const char **reason);    /* -EFAULT through the fixup path, never a kill */
bool selftest_process_protnone(const char **reason);  /* a PROT_NONE touch is fatal */
bool selftest_process_oom(const char **reason);       /* injected demand-page failures */
bool selftest_process_rlimit(const char **reason);    /* rlimits and SETCRED from user mode */
bool selftest_process_nproc(const char **reason);     /* NPROC admission under concurrent spawns */
bool selftest_process_spawn(const char **reason);   /* kernel/process/proctest.c (Phase 9) */
bool selftest_linux_elf(const char **reason);       /* kernel/process/proctest.c (Phase 11) */
bool selftest_tty_ldisc(const char **reason);

/* kernel/core/fbtest.c: the framebuffer console, checked by reading the
 * pixels back, and what it costs. */
bool selftest_fb_console(const char **reason);
bool selftest_fb_bench(const char **reason);

/* kernel/device/hidtest.c: keys typed at the emulated keyboard reach the
 * console tty (the harness types them over QMP). */
bool selftest_hid_arm(const char **reason);       /* asks the harness to type */
bool selftest_hid_keyboard(const char **reason);  /* reads back what it typed, at the end of the run */
/* kernel/device/devtest.c: a hub unplugged with a device behind it. */
bool selftest_usb_hub_unplug(const char **reason);       /* kernel/tty/ttytest.c */
bool selftest_ipc_pipe(const char **reason);        /* kernel/ipc/pipetest.c */

/* Phase 5: kernel/module/modtest.c */
bool selftest_bootarchive(const char **reason);
bool selftest_ksym(const char **reason);
bool selftest_modsig(const char **reason);
bool selftest_module_reject(const char **reason);
bool selftest_module_load(const char **reason);
bool selftest_module_fail(const char **reason);

/* Phase 6: kernel/device/devtest.c */
bool selftest_device(const char **reason);
bool selftest_pci(const char **reason);
bool selftest_dma(const char **reason);
bool selftest_random(const char **reason);
bool selftest_blk(const char **reason);
bool selftest_virtio_console(const char **reason);

/* Phase 7: kernel-services/vfs/vfstest.c */
bool selftest_vfs_ramfs(const char **reason);
bool selftest_pagecache(const char **reason);
bool selftest_cache_limits(const char **reason);   /* ramfs page budget, global limit with reclaim */
bool selftest_cache_budget_race(const char **reason);   /* the budget admission under concurrent misses */
bool selftest_crc32c(const char **reason);

/* Phase 7: kernel-services/filesystem/cosmofs/cosmofstest.c */
bool selftest_pool(const char **reason);
bool selftest_cosmofs_format(const char **reason);
bool selftest_cosmofs_ops(const char **reason);
bool selftest_cosmofs_crash(const char **reason);
bool selftest_cosmofs_holes(const char **reason);      /* milestone 7 (RAM devices): sparse files */
bool selftest_cosmofs_csum(const char **reason);       /* data and directory checksums */
bool selftest_cosmofs_fsync(const char **reason);
bool selftest_cosmofs_snapshot(const char **reason);
bool selftest_cosmofs_snapshot_remount(const char **reason);      /* fsync commits */
bool selftest_cosmofs_pool2(const char **reason);                 /* a pool of two members */
bool selftest_cosmofs_v3(const char **reason);                    /* the older on-disk format */
bool selftest_cosmofs_badmembers(const char **reason);            /* a member table that cannot be true */
bool selftest_cosmofs_mirror(const char **reason);                /* a mirrored member, repair and scrub */
bool selftest_cosmofs_mirror_stale(const char **reason);          /* the stale copy is the first one */
bool selftest_cosmofs_compress(const char **reason);              /* compressed records */
bool selftest_cosmofs_crypt(const char **reason);                 /* encryption and keys */
bool selftest_cosmofs_reserve(const char **reason);    /* the metadata reserve on a full disk */
bool selftest_cosmofs_fallback(const char **reason);   /* older-slot fallback at mount */
bool selftest_cosmofs_writeback(const char **reason);  /* the writeback thread */
bool selftest_cosmofs_badmap(const char **reason);     /* a crafted inode's direct runs are validated */

/* Phase 8: kernel-services/network/nettest.c */
bool selftest_net_mbuf(const char **reason);
bool selftest_net_cksum(const char **reason);
bool selftest_net_arp(const char **reason);
bool selftest_net_second_nic(const char **reason);   /* a second interface takes over when the default goes down */
bool selftest_net_lo_udp(const char **reason);
bool selftest_net_lo_tcp(const char **reason);
bool selftest_net_lo_tcp_loss(const char **reason);
bool selftest_net_tcp_mss(const char **reason);
bool selftest_net_harness(const char **reason);
bool selftest_net_netif_lifetime(const char **reason);
bool selftest_net_accept_race(const char **reason);
bool selftest_net_tcp_syncache(const char **reason);   /* milestone 8: SYN cache and cookies */
bool selftest_net_tcp_rfc5961(const char **reason);    /* blind RST/SYN/ACK earn challenge ACKs */
bool selftest_net_tcp_reorder(const char **reason);    /* out-of-order segments are reassembled */
bool selftest_net_tcp_keepalive(const char **reason);  /* keepalive timeout, orphaned FIN_WAIT_2 */
bool selftest_net_icmp_limit(const char **reason);     /* ICMP rate limit, path MTU discovery */
bool selftest_net_nonblock(const char **reason);       /* non-blocking mode and readiness */
bool selftest_net_steer(const char **reason);          /* unit 11: per-CPU receive queues and flow steering */
bool selftest_net_rxhook_grace(const char **reason);   /* clearing the receive hook waits for a running one */
bool selftest_net_csum_offload(const char **reason);   /* unit 11: the partial checksum form and M_CSUM_OK */
bool selftest_net_bench(const char **reason);          /* unit 11: loopback throughput, steering off and on */
bool selftest_net_nicbench(const char **reason);       /* traffic that leaves the machine, per interface */
bool selftest_blk_lifetime(const char **reason);
struct bio;
bool selftest_nvme(const char **reason);         /* the NVMe namespace through the block layer */
bool selftest_usb_enum(const char **reason);     /* the device on the USB bus: descriptors, parent, endpoints */
bool selftest_usb_storage(const char **reason);  /* sda through the block layer: round trips, segments, counters */
bool selftest_usb_storage_timeout(const char **reason);   /* a CSW that never comes: the block layer's timeout, recovery */
bool selftest_usb_unplug(const char **reason);   /* the removal path with a bio in flight; re-enumeration */
bool selftest_blk_bench(const char **reason);    /* reports: sequential I/O over every disk in the boot */
bool selftest_ahci_identify(const char **reason); /* the SATA disk: geometry, controller as DMA device */
bool selftest_ahci_io(const char **reason);       /* ahci0p0 through the block layer: round trips, PRDT, counters */
bool selftest_ahci_timeout(const char **reason);  /* a command that never starts: the block layer's timeout, port restart */
bool selftest_ahci_unplug(const char **reason);   /* the disk-gone path with a bio in flight; a new blkdev after */
bool selftest_ahci_reset(const char **reason);    /* COMRESET with a command in flight; the same disk keeps serving */
void selftest_nvme_mark_done(struct bio *bio);
bool selftest_module_unload_busy(const char **reason);
bool selftest_hv_probe(const char **reason);
bool selftest_hv_caps(const char **reason);
bool selftest_el2_stub(const char **reason);
bool selftest_el2_guest_wfi(const char **reason);
bool selftest_el2_guest_hvc(const char **reason);
bool selftest_el2_guest_mmio(const char **reason);
bool selftest_el2_guest_sysreg(const char **reason);
bool selftest_el2_guest_spin(const char **reason);
bool selftest_hv_npt(const char **reason);
bool selftest_hv_guest_pio(const char **reason);
bool selftest_hv_guest_irq(const char **reason);
bool selftest_hv_guest_cpuid(const char **reason);
bool selftest_hv_guest_pm(const char **reason);
bool selftest_hv_guest_shutdown(const char **reason);
bool selftest_hv_guest_spin(const char **reason);
bool selftest_hv_guest_fpu(const char **reason);

#endif /* KERNEL_SELFTEST_H */
