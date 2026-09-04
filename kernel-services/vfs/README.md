# kernel-services/vfs

The VFS (`vfs.c`: `fs_type` registry, mounts, path walk, vnodes, files,
the namespace operations), the per-vnode page cache (`pagecache.c`),
ramfs (`ramfs.c`: the root filesystem, `/boot` populated from the boot
archive, `/tmp`, `/mnt`, `/dev`) and their self-tests (`vfstest.c`:
`crc32c`, `pagecache`, `vfs-ramfs`). Public headers:
`kernel/include/kernel/{vfs,pagecache}.h`. The VFS knows filesystems only
through `struct fs_type` and `struct vnode_ops`. Docs:
`docs/kernel-services/vfs/`.
