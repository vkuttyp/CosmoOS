# kernel-services/vfs

The VFS (`vfs_mount.cee`: the `fs_type` registry, mounts, the root
switch, sync, device claims, init, formats and passes; `vfs_vnode.cee`:
vnodes and the held-walk seam; `vfs_walk.cee`: permissions, the path
walk, lookups and an open's walk; `vfs_file.cee`: open files;
`vfs_dir.cee`: the namespace operations; `vfs_floor.c`: the C data under
them), the per-vnode page cache (`pagecache.cee`),
ramfs (`ramfs.cee`: the root filesystem, `/boot` populated from the boot
archive, `/tmp`, `/mnt`, `/dev`) and their self-tests (`vfstest_*.cee`:
`crc32c`, `pagecache`, `vfs-ramfs`). Public headers:
`kernel/include/kernel/{vfs,pagecache}.h`. The VFS knows filesystems only
through `struct fs_type` and `struct vnode_ops`. Docs:
`docs/kernel-services/vfs/`.
