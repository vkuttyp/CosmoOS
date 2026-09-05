# NVMe driver: API

The driver exports nothing; its outward interface is the block devices it
registers and the module it is.

## The module `nvme`

`COSMO_MODULE("nvme", "1.0", ...)`, no dependencies, `MODULE_CAP_DRIVER`.
`init` registers `struct pci_driver nvme_driver` (id table: class `01`,
subclass `08`, any vendor; `probe` refuses programming interface other
than `02`); `shutdown` unregisters it, which removes every bound
controller.

## Block devices

One `struct blkdev` per active namespace with an accepted format (block
size 512 B to 4 KiB, no metadata), registered under the exact name
`nvme<controller index>n<nsid>` (`blk_register_named`):

| Field | Value |
|---|---|
| `dev` | the PCI device (its 64-bit DMA mask validates buffers) |
| `sector_size` | the namespace's block size |
| `capacity` | `NSZE` blocks |
| `max_sectors` | `MDTS` (capped at 128 pages = 512 KiB) in sectors |
| `max_segments` | the same page count (each segment is at least one PRP) |
| `nr_queues` | the I/O queues created |
| `timeout_ns` | the block layer default, 30 s |

`ops->submit`: maps the segments, builds the PRPs and the command, takes
a command slot on the current CPU's queue (`-EAGAIN` when the queue is
full: the block layer parks the bio), rings the doorbell. `-EIO` once the
controller is dead. Thread context.

`ops->timeout`: Abort the command (admin, 5 s bound), then reset the
controller and fail every in-flight command with `-ETIMEDOUT` if the
abort did not complete it; the controller stays dead
(`design.md`, "Timeouts, abort and reset"). Thread context (the
block layer's timeout thread).

`ops->release`: frees the namespace when the last holder is gone.

## Log lines

```text
[ INFO] blk: nvme0n1: 16384 sectors of 512 bytes (8 MiB)
[ INFO] nvme0: pci:00:03.0: QEMU NVMe Ctrl (cosmo-nvme0), 1 namespace(s) of 256, 4 I/O queue(s) of depth 32, 512 KiB per request
[ INFO] module: loaded nvme 1.0 (...)
```

Errors: `nvme: <pci>: cannot map BAR0`, `unsupported controller (CAP
...)`, `controller did not become ready (rc)`, `MSI-X unavailable`,
`Identify Controller failed`, `Set Features (queues) failed`, `cannot
create I/O queue N`; `nvme<n>: <reason>; disabling the controller, every
request fails from here` when a request could not be aborted or the
device was removed; `nvme<n>: namespace N refused (...)` for a format the
driver does not handle; `completion for an unknown command` when the
controller answers a command id that holds nothing (counted, ignored).

## Statistics

The block layer's per-device counters (`reads`, `writes`, `flushes`,
`errors`, `timeouts`, `requeued`, `completed_local`, `completed_remote`)
are the driver's observable statistics; `completed_local` grows when a
completion is handled on the CPU that issued the request, which is the
per-CPU queue design working.
