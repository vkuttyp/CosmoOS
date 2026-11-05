# kernel/block

The block layer (constitution section 28): `struct blkdev` registered by
drivers (`vda`, ...), `struct bio` requests with completion callbacks,
validation before a driver sees a request, synchronous
`blk_read`/`blk_write`/`blk_flush` helpers. Independent of every driver
and every filesystem; the page cache and VFS of Phase 7 sit above it.
The layer is CeeSafe (`blk.cee`, roadmap 2.0 SA7), its module exports in
`blk_floor.c`; `tests/host/test_blk.c` holds it to the C it replaced. Its
self-tests are `blktest.cee` and `parttest.cee`.
Documentation: `docs/kernel/device/` (api.md, design.md "Block layer").
