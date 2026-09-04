# kernel-services/storage

The storage pool (`pool.c`, `kernel/include/kernel/storage.h`):
`pool_open/close/read/write/flush` in 4 KiB pool blocks over one block
device. Filesystems address the pool, never a `blkdev`; allocation
groups, multiple members and redundancy attach here later (constitution
section 29). Self-test `pool`. Docs: `docs/kernel-services/vfs/`.
