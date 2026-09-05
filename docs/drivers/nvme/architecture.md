# NVMe driver: architecture

Milestone 9 of the post-roadmap plan (`docs/audit/2026-09-post-roadmap-audit.md`
§19; Prompt #2 §26; audit §10.4). Constitution sections 25 (drivers
never touch bus layout themselves), 26 (DMA through the API), 28 (the
block layer knows no driver), invariant 5 (the kernel image depends on
no driver). Spec: NVM Express 1.4 (controller registers §3, admin
commands §5, NVM command set §6, queues §7.6).

## Where it sits

```text
   filesystems / self-tests     blk_read, blk_write, blk_submit (segments)
        │
   kernel/block/blk.c           validation, pending queue, in-flight list, timeouts
        │
   drivers/nvme/nvme.c  (module nvme)   controller, admin queue, one I/O queue per CPU,
                                        PRP construction, MSI-X per queue, abort/reset
        │
   drivers/pci   BAR0, MSI-X table       kernel/device/dma.c   queues, PRP lists, segment mappings
```

The driver is a boot module (`modules/nvme.ko`, after the virtio
modules); everything it uses is an exported symbol. QEMU attaches one
controller (`-device nvme,serial=cosmo-nvme0`) with one 8 MiB namespace
on both machines.

## Purpose

Drive NVM Express controllers as block devices, one per namespace, with
per-CPU queues and interrupt routing so that submissions and completions
of one CPU never touch another CPU's queue.

## Responsibilities

- Controller bring-up and shutdown: reset, admin queue pair, feature
  negotiation (queue count), identify controller and namespaces, MSI-X.
- I/O queue pairs, one per CPU (fewer when the controller grants fewer),
  each with its own vector routed to its CPU.
- Translating bios (single buffer or segments) into Read/Write/Flush
  commands with PRP1/PRP2/PRP lists; mapping and unmapping every segment.
- Command ids, completion-queue phase handling, per-queue locking.
- Abort on a timed-out request; controller reset and offline when the
  abort fails.
- Namespace block devices named `nvme<n>n<nsid>`.

## Non-responsibilities

- SGLs, metadata per block, namespace management, asynchronous events,
  log pages, power states, multipath, re-initialisation after a reset,
  a character device for pass-through (design.md, "What is deliberately
  not here").
- Anything a filesystem or the block layer does (queueing beyond the
  device's depth, flush semantics, ordering).

## Interfaces at a glance

| Interface | Header | Used by |
|---|---|---|
| `struct blkdev_ops` (`submit`, `release`, `timeout`) | `kernel/blk.h` | the block layer calls in |
| `blk_register_named`, `blk_unregister`, `bio_complete`, `bio_segment` | `kernel/blk.h` | the driver calls out |
| `pci_register_driver`, `pci_enable_device`, `pci_map_bar`, `pci_msix_enable/request/release/disable` | `drivers/pci.h` | probe/remove |
| `dma_set_mask`, `dma_alloc/free`, `dma_map/unmap`, `dma_sync_for_device/cpu` | `kernel/dma.h` | queues, PRP lists, data |
| `cpu_count`, `arch_cpu_id`, `completion_*`, `synchronize_irq` | kernel exports (milestone 9) | queue sizing and selection, admin commands, removal |
