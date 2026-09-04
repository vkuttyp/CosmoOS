# kernel-services

Privileged services outside the trusted core. They run in kernel mode
for performance but talk to the core only through documented interfaces.

- `vfs/`: the virtual filesystem (mounts, path resolution, vnodes,
  `struct file`), the page cache, and ramfs (the boot root). Phase 7.
- `storage/`: the storage pool, the block addressing layer filesystems
  use instead of block devices. Phase 7.
- `filesystem/`: on-disk filesystems; `cosmofs/` is the copy-on-write
  filesystem. Phase 7.
- `network/`: the network stack: mbufs, interfaces and the `netrx`
  worker, Ethernet/ARP, IPv4/IPv6 with ICMP and ND, UDP, TCP, sockets.
  Phase 8.
- `virtualization/`: later.

Documentation: `docs/kernel-services/vfs/`,
`docs/kernel-services/filesystem/cosmofs/` and
`docs/kernel-services/network/`.
