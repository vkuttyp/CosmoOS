# kernel-services/storage

The storage pool (`pool.cee`, in CeeSafe, `kernel/include/kernel/storage.h`):
`pool_open/add_member/add_copy/close/read/write/flush` in 4 KiB pool
blocks over one or more members, each of which is a mirror group of up
to four devices holding the same blocks. A block is named by a DVA -- the member in
the top 8 bits, the block within it in the low 56 -- so member 0 at
block `b` is the same sector a single-device pool always addressed.
Filesystems address the pool, never a `blkdev`. The pool is an ordered
set of devices and nothing more: which devices belong together is the
filesystem's question, because the identity is written in the
filesystem's format (`cosmofs_member.cee`). Redundancy attaches here next
(constitution section 29). `pool_floor.c` keeps the C the port needs: the sysctl's
`g_pool_fault_errors`, the spare slots' addresses and the asserted constants.
`tests/host/test_pool.c` holds the port to the C's digest. Self-tests `pool` and
`pool-copy-states` (in cosmofs's `cosmofstest.c`). Docs:
`docs/kernel-services/vfs/`.
