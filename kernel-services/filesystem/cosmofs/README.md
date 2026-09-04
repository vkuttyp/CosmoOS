# kernel-services/filesystem/cosmofs

The copy-on-write filesystem: `cosmofs_format.h` (on-disk layout, host
testable), `cosmofs_internal.h` (in-memory state), `cosmofs_core.c`
(metadata buffers, copy-on-write, bitmap allocator, inode map, commit,
format, mount/unmount), `cosmofs.c` (vnode operations, extents,
directories, data pages), `cosmofstest.c` (self-tests `pool`,
`cosmofs-format`, `cosmofs-ops`, `cosmofs-crash`). Public header:
`kernel/include/kernel/cosmofs.h`. Docs:
`docs/kernel-services/filesystem/cosmofs/` and
`docs/kernel-services/vfs/`.
