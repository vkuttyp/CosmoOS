# kernel-services/storage

The storage pool (`pool.c`, `kernel/include/kernel/storage.h`):
`pool_open/add_member/close/read/write/flush` in 4 KiB pool blocks over
one or more member devices. A block is named by a DVA -- the member in
the top 8 bits, the block within it in the low 56 -- so member 0 at
block `b` is the same sector a single-device pool always addressed.
Filesystems address the pool, never a `blkdev`. The pool is an ordered
set of devices and nothing more: which devices belong together is the
filesystem's question, because the identity is written in the
filesystem's format (`cosmofs_member.c`). Redundancy attaches here next
(constitution section 29). Self-test `pool`. Docs:
`docs/kernel-services/vfs/`.
