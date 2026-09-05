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

/* Phase 4: kernel/process/proctest.c */
bool selftest_objects(const char **reason);
bool selftest_elf(const char **reason);
bool selftest_process_reject(const char **reason);
bool selftest_process_selftest(const char **reason);
bool selftest_process_fault(const char **reason);
bool selftest_process_spawn(const char **reason);   /* kernel/process/proctest.c (Phase 9) */
bool selftest_linux_elf(const char **reason);       /* kernel/process/proctest.c (Phase 11) */
bool selftest_tty_ldisc(const char **reason);       /* kernel/tty/ttytest.c */
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
bool selftest_crc32c(const char **reason);

/* Phase 7: kernel-services/filesystem/cosmofs/cosmofstest.c */
bool selftest_pool(const char **reason);
bool selftest_cosmofs_format(const char **reason);
bool selftest_cosmofs_ops(const char **reason);
bool selftest_cosmofs_crash(const char **reason);

/* Phase 8: kernel-services/network/nettest.c */
bool selftest_net_mbuf(const char **reason);
bool selftest_net_cksum(const char **reason);
bool selftest_net_arp(const char **reason);
bool selftest_net_lo_udp(const char **reason);
bool selftest_net_lo_tcp(const char **reason);
bool selftest_net_lo_tcp_loss(const char **reason);
bool selftest_net_tcp_mss(const char **reason);
bool selftest_net_harness(const char **reason);
bool selftest_net_netif_lifetime(const char **reason);
bool selftest_net_accept_race(const char **reason);
bool selftest_blk_lifetime(const char **reason);
bool selftest_module_unload_busy(const char **reason);
bool selftest_hv_probe(const char **reason);
bool selftest_hv_npt(const char **reason);
bool selftest_hv_guest_pio(const char **reason);
bool selftest_hv_guest_irq(const char **reason);
bool selftest_hv_guest_cpuid(const char **reason);
bool selftest_hv_guest_pm(const char **reason);
bool selftest_hv_guest_shutdown(const char **reason);
bool selftest_hv_guest_spin(const char **reason);
bool selftest_hv_guest_fpu(const char **reason);

#endif /* KERNEL_SELFTEST_H */
