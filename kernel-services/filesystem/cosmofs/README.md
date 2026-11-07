# kernel-services/filesystem/cosmofs

The copy-on-write filesystem: `cosmofs_format.h` (on-disk layout, host
testable), `cosmofs_internal.h` (in-memory state), `cosmofs_core.c`
(metadata buffers, copy-on-write, bitmap allocator, inode map, commit,
format, mount/unmount), `cosmofs.c` (vnode operations, extents,
directories, data pages), `cosmofstest_*.cee` (the self-tests, by group; `cosmofstest_basic.cee`
has `pool`, `cosmofs-format`, `cosmofs-ops`, `cosmofs-crash`, and
`cosmofstest_lib.cee` what the groups share). Public header:
`kernel/include/kernel/cosmofs.h`. Docs:
`docs/kernel-services/filesystem/cosmofs/` and
`docs/kernel-services/vfs/`.
