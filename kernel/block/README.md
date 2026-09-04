# kernel/block

The block layer (constitution section 28): `struct blkdev` registered by
drivers (`vda`, ...), `struct bio` requests with completion callbacks,
validation before a driver sees a request, synchronous
`blk_read`/`blk_write`/`blk_flush` helpers. Independent of every driver
and every filesystem; the page cache and VFS of Phase 7 sit above it.
Documentation: `docs/kernel/device/` (api.md, design.md "Block layer").
