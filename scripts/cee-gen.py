#!/usr/bin/env python3
"""
cee-gen.py - the C that CosmoOS's CeeSafe sources generate (docs/build/ceesafe.md).

Kernel code is written in CeeSafe, a memory-safe language compiled to C
(https://github.com/CeeSafe/CeeSafe; roadmap 2.0's CeeSafe rule). The tree
holds the .cee files only: the build runs this script with the pinned
ceesafec (build/cee.mk) and compiles the generated C from out/cee. A
`make dist` tarball carries the generated C beside each source instead, so
it builds with no ceesafec. Each port lists its source, the C it generates,
and, for a port whose structs and API are defined in CeeSafe, the C header
ceesafec generates for it (--exports-header); every port shares CeeSafe's
runtime, generated too:

  kernel/block/gpt.cee              -> kernel/block/gpt.c
  kernel/object/handle.cee          -> kernel/object/handle.c, kernel/include/kernel/handle_types.h
  kernel/ceesafe/kcore_test.cee     -> kernel/ceesafe/kcore_test.c   (with CeeSafe's kernel library)
  (every port)                      -> kernel/include/ceesafe/rt.h, foreign.h, kernel/ceesafe/ceesafe_rt_core.c

The generated C needs a ceesafe_panic for the environment it is linked into
(kernel/ceesafe/ceesafe_panic.c in the kernel, tests/host/ceesafe_panic.c in
host tests and fuzz targets).

A driver module written in CeeSafe (MODULES; roadmap 2.0 SA4,
drivers/ceesafe/README.md) is its driver source compiled with the device
API's sources (drivers/ceesafe/*.cee) and CeeSafe's kernel library, into
one C file that ends with the module's COSMO_MODULE entry. Every module
links its own copy of the runtime, built with the module flags, and the
driver floor (drivers/ceesafe/driver_floor.c, which has its ceesafe_panic):

  drivers/virtio/virtio_rng.cee     -> drivers/virtio/virtio_rng.c   (module virtio_rng)
  (every module)                    -> drivers/ceesafe/ceesafe_rt_core.c

The compiler is pinned (CEESAFEC_VERSION): generated C is a pure function of
the source and the compiler, so two builds agree (`make reproducible`).
Moving to another ceesafec is a change of its own: bump the pin here and in
ci/Dockerfile and .github/workflows/ci.yml (with the release's SHA-256).

Every run but the build's own (--no-negative) also checks the sources the
kernel must refuse, tests/ceesafe/negative/*.cee (NEGATIVE): ceesafec must
give exactly the errors their `// error[N...]` lines name.

Usage:
    python3 scripts/cee-gen.py --out out/cee           # generate (make cee; the build runs it)
    python3 scripts/cee-gen.py --out out/cee --check   # compare only (make cee-check)
    python3 scripts/cee-gen.py --list                  # the generated files' paths (no ceesafec needed)
    python3 scripts/cee-gen.py --pinned                # print the pinned version
The ceesafec used is $CEESAFEC, else `ceesafec` on PATH. To install the pinned one:
    curl -fsSL https://ceesafe.github.io/CeeSafe/install.sh | CEESAFE_VERSION=v<pinned> sh
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CEESAFEC_VERSION = "0.21.0"

# Each port: its CeeSafe source, the C it generates, what that C is, the header ceesafec generates for it (None
# when the port's header is hand-written C, imported by the source), and whether it uses CeeSafe's kernel library
# (lib/kernel: locks, per-CPU values, kernel objects; ceesafec --kernel cosmoos).
PORTS = [
    ("kernel/block/gpt.cee", "kernel/block/gpt.c", "The GUID Partition Table (gpt.h)", None, False),
    ("kernel/block/blk.cee", "kernel/block/blk.c",
     "The block layer: the registry, bio checks, the pending queue, timeouts, resets, unregistration, the flush "
     "sequences, the synchronous helpers (blk.h)", None, True),
    ("kernel/block/blktest.cee", "kernel/block/blktest.c",
     "The block layer's self-tests: blk-queue, blk-segments, blk-timeout, blk-timeout-skew", None, True),
    ("kernel/block/parttest.cee", "kernel/block/parttest.c",
     "The partitions' self-test: blk-gpt", None, True),
    ("kernel-services/storage/pool.cee", "kernel-services/storage/pool.c",
     "The storage pool: members, mirror copies and their states, the missed-region bitmaps, spares, the I/O "
     "(storage.h)", None, True),
    ("kernel/block/part.cee", "kernel/block/part.c",
     "Partitions as block devices: the scan, the forwarding driver, removal and the lookups (part.h)",
     "kernel/include/kernel/part_types.h", True),
    ("kernel/block/ramblk.cee", "kernel/block/ramblk.c",
     "The RAM block device with a write recorder, for the debug builds' harnesses (ramblk.h)",
     "kernel/include/kernel/ramblk_types.h", True),
    ("kernel/block/blkserial.cee", "kernel/block/blkserial.c",
     "Disk serials: the printable ASCII drivers keep, and the self-tests' test disks (blk.h)",
     "kernel/include/kernel/blkserial.h", True),
    ("kernel/process/cputime.cee", "kernel/process/cputime.c",
     "CPU time and the CPU-time interval timers (cputime.h)", None, False),
    ("kernel/process/nice.cee", "kernel/process/nice.c", "The rules of nice values (nice.h)", None, False),
    ("kernel/scheduler/fair.cee", "kernel/scheduler/fair.c",
     "The weighted fair share at the default priority level (fair.h)", None, False),
    ("kernel/object/object.cee", "kernel/object/object.c", "The reference-counted kernel object header (object.h)", None,
     False),
    ("kernel/object/handle.cee", "kernel/object/handle.c", "The per-process handle table (handle.h)",
     "kernel/include/kernel/handle_types.h", False),
    ("kernel/scheduler/wait.cee", "kernel/scheduler/wait.c", "Wait queues (wait.h)",
     "kernel/include/kernel/wait_types.h", False),
    ("kernel/scheduler/completion.cee", "kernel/scheduler/completion.c", "One-shot completions (completion.h)",
     "kernel/include/kernel/completion_types.h", False),
    ("kernel/scheduler/semaphore.cee", "kernel/scheduler/semaphore.c", "Counting semaphores (semaphore.h)",
     "kernel/include/kernel/semaphore_types.h", False),
    ("kernel/scheduler/mutex.cee", "kernel/scheduler/mutex.c", "Sleeping mutexes with priority inheritance (mutex.h)",
     "kernel/include/kernel/mutex_types.h", False),
    ("kernel/core/quiesce.cee", "kernel/core/quiesce.c", "Quiescence: epoch-based reclamation (quiesce.h)",
     "kernel/include/kernel/quiesce_types.h", False),
    ("kernel/ipc/pipe.cee", "kernel/ipc/pipe.c", "The pipe's ring, and anonymous pipes over it (pipe.h)",
     "kernel/include/kernel/pipe_types.h", False),
    ("kernel/ipc/fifo.cee", "kernel/ipc/fifo.c", "Named pipes: the pipe's ring behind a filesystem node (fifo.h)",
     "kernel/include/kernel/fifo_types.h", False),
    ("kernel/core/crc32c.cee", "kernel/core/crc32c.c", "CRC-32C (crc32c.h)", None, False),
    ("kernel-services/filesystem/cosmofs/cosmofs_reach.cee", "kernel-services/filesystem/cosmofs/cosmofs_reach.c",
     "cosmofs's tree walk: every block one tree reaches (cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_clone.cee", "kernel-services/filesystem/cosmofs/cosmofs_clone.c",
     "cosmofs's writable clones of a snapshot (cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_rollback.cee", "kernel-services/filesystem/cosmofs/cosmofs_rollback.c",
     "cosmofs's rollback: the record, the request, cancel, BOOT_OK and the switch at mount (cosmofs_internal.h)", None,
     True),
    ("kernel-services/filesystem/cosmofs/cosmofs_scrub.cee", "kernel-services/filesystem/cosmofs/cosmofs_scrub.c",
     "cosmofs's scrub: every block of every tree, every copy, paced (cosmofs.h, cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_crypt.cee", "kernel-services/filesystem/cosmofs/cosmofs_crypt.c",
     "cosmofs's keys: the key blocks, the key table and the /dev/fsctl key operations (cosmofs.h, cosmofs_internal.h)",
     None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_resilver.cee", "kernel-services/filesystem/cosmofs/cosmofs_resilver.c",
     "cosmofs's resilver, its thread and the pool commands (cosmofs.h, cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_member.cee", "kernel-services/filesystem/cosmofs/cosmofs_member.c",
     "cosmofs's members: labels, assembly, copy state, the member table, read repair (cosmofs_internal.h)", None,
     True),
    ("kernel-services/filesystem/cosmofs/cosmofstest_media.cee",
     "kernel-services/filesystem/cosmofs/cosmofstest_media.c",
     "cosmofs's self-tests of the media: badmap, snapshot, mirror, mirror-stale, compress (cosmofs.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofstest_extent.cee",
     "kernel-services/filesystem/cosmofs/cosmofstest_extent.c",
     "cosmofs's self-tests of a file's runs: extent-bound, -rebuild, -raw, -fail (cosmofs.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofstest_rollback.cee",
     "kernel-services/filesystem/cosmofs/cosmofstest_rollback.c",
     "cosmofs's self-tests of rollbacks and clones (cosmofs.h, cosmofs_internal.h, cosmofstest.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofstest_b2.cee", "kernel-services/filesystem/cosmofs/cosmofstest_b2.c",
     "cosmofs's self-tests of roadmap 1.2 B2: replay, check-dev, the scrub (cosmofs.h, cosmofs_internal.h)", None,
     True),
    ("kernel-services/filesystem/cosmofs/cosmofstest_basic.cee",
     "kernel-services/filesystem/cosmofs/cosmofstest_basic.c",
     "cosmofs's self-tests of the scratch disk and the transaction engine (cosmofs.h, cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofstest_copies.cee",
     "kernel-services/filesystem/cosmofs/cosmofstest_copies.c",
     "cosmofs's self-tests of a mirror's copies and the resilver (cosmofs.h, cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofstest_freelog.cee",
     "kernel-services/filesystem/cosmofs/cosmofstest_freelog.c",
     "cosmofs's self-tests of symlinks, the free record and the snapshot deadlists (cosmofs.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofstest_names.cee", "kernel-services/filesystem/cosmofs/cosmofstest_names.c",
     "cosmofs's self-tests of old formats, two members, snapshot identity, long names and links (cosmofs.h)", None,
     True),
    ("kernel-services/filesystem/cosmofs/cosmofstest_check.cee",
     "kernel-services/filesystem/cosmofs/cosmofstest_check.c",
     "cosmofs's self-tests of encryption, the member table and the structural check (cosmofs.h, cosmofstest.h)",
     None, True),
    ("kernel-services/filesystem/cosmofs/cosmofstest_orphan.cee",
     "kernel-services/filesystem/cosmofs/cosmofstest_orphan.c",
     "cosmofs's self-tests of the orphan record, the check's partial answers and /dev/fsctl (cosmofs.h, fsctl.h)",
     None, True),
    ("kernel-services/filesystem/cosmofs/cosmofstest_lib.cee", "kernel-services/filesystem/cosmofs/cosmofstest_lib.c",
     "what cosmofs's self-tests share: files, the engine mount, the check's fixture (cosmofs.h, cosmofstest.h)", None,
     True),
    ("kernel-services/filesystem/cosmofs/cosmofscrash.cee", "kernel-services/filesystem/cosmofs/cosmofscrash.c",
     "cosmofs's crash-consistency self-test, cosmofs-replay (cosmofs.h, ramblk.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_keytest.cee", "kernel-services/filesystem/cosmofs/cosmofs_keytest.c",
     "cosmofs's self-test of an encrypted root's pool, format 14 (cosmofs.h, cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_dir.cee", "kernel-services/filesystem/cosmofs/cosmofs_dir.c",
     "cosmofs's directory operations (cosmofs_internal.h)", None, True),
    ("kernel-services/vfs/vfstest_base.cee", "kernel-services/vfs/vfstest_base.c",
     "the VFS's self-tests of crc32c, the page cache, symlinks, ramfs, fsync, namespaces and concurrency (vfs.h)",
     None, True),
    ("kernel-services/vfs/vfstest_wb.cee", "kernel-services/vfs/vfstest_wb.c",
     "the VFS's self-tests of write-back errors, the dirty bound, the benches and pinned frames (vfs.h)", None, True),
    ("kernel-services/vfs/vfstest_walk.cee", "kernel-services/vfs/vfstest_walk.c",
     "the VFS's self-tests of renames, named lookups and unmounts from a start (vfs.h)", None, True),
    ("kernel-services/vfs/vfstest_fill.cee", "kernel-services/vfs/vfstest_fill.c",
     "the VFS's self-tests of filled reads and dirty-bounded writers (vfs.h, aio.h)", None, True),
    ("kernel-services/vfs/vfstest_dev.cee", "kernel-services/vfs/vfstest_dev.c",
     "the VFS's self-tests of /dev/fsctl, character devices and the syscall bounce (vfs.h, fsctl.h)", None, True),
    ("kernel-services/vfs/vfstest_mount.cee", "kernel-services/vfs/vfstest_mount.c",
     "the VFS's self-tests of page budgets, cache limits, the last put and mounts' names and pins (vfs.h)", None,
     True),
    ("kernel-services/vfs/vfs_vnode.cee", "kernel-services/vfs/vfs_vnode.c",
     "The VFS's vnodes: allocation, the hash, the last put and its release, stat, and the held-walk seam (vfs.h)",
     None, True),
    ("kernel-services/vfs/fsctl.cee", "kernel-services/vfs/fsctl.c",
     "/dev/fsctl: mount listing, checks, scrubs, snapshots, pools and keys, and /proc/mounts (vfs.h, fsctl.h)", None,
     True),
    ("kernel-services/vfs/ramfs.cee", "kernel-services/vfs/ramfs.c",
     "ramfs: the in-memory filesystem, its character devices, anonymous files and the boot archive (vfs.h)", None,
     True),
    ("kernel-services/vfs/pagecache.cee", "kernel-services/vfs/pagecache.c",
     "The per-vnode page cache: lookups, reads and writes, write-back, reclaim and the dirty bound (pagecache.h)",
     None, True),
    ("kernel-services/vfs/blkctl.cee", "kernel-services/vfs/blkctl.c",
     "/dev/blkctl: the block devices' list, sector reads and writes, flushes, rescans and formats (blkctl.h)", None,
     True),
    ("kernel-services/vfs/mountns.cee", "kernel-services/vfs/mountns.c",
     "Mount namespaces: the views, their copies and references, and visibility (mountns.h)", None, True),
    ("kernel-services/vfs/memdev.cee", "kernel-services/vfs/memdev.c",
     "/dev/null and /dev/zero (vfs.h)", None, True),
    ("kernel-services/filesystem/procfs/procfs.cee", "kernel-services/filesystem/procfs/procfs.c",
     "/proc: processes, their files, links and handles, the system files and /proc/net's paths (vfs.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs.cee", "kernel-services/filesystem/cosmofs/cosmofs.c",
     "cosmofs's directory blocks, the .snapshots directory and the tree locks (cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_vnode.cee", "kernel-services/filesystem/cosmofs/cosmofs_vnode.c",
     "cosmofs's vnodes and the write-through of their inodes (cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_file.cee", "kernel-services/filesystem/cosmofs/cosmofs_file.c",
     "a cosmofs regular file's vnode operations (cosmofs.h, cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_data.cee", "kernel-services/filesystem/cosmofs/cosmofs_data.c",
     "a cosmofs file's runs, map, block and record writes and truncation (cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_csum.cee", "kernel-services/filesystem/cosmofs/cosmofs_csum.c",
     "cosmofs's file checksums, verified reads, compressed records and the raises to 14 and 15 "
     "(cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_snapctl.cee", "kernel-services/filesystem/cosmofs/cosmofs_snapctl.c",
     "cosmofs's snapshots through /dev/fsctl (cosmofs.h, cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_core.cee", "kernel-services/filesystem/cosmofs/cosmofs_core.c",
     "cosmofs's mount, unmount, writeback thread and first-change mark (cosmofs.h, cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_hooks.cee", "kernel-services/filesystem/cosmofs/cosmofs_hooks.c",
     "cosmofs's stats, key operations on a mount and test hooks (cosmofs.h, cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_corrupt.cee", "kernel-services/filesystem/cosmofs/cosmofs_corrupt.c",
     "cosmofs's corruption hook for the structural check's tests (cosmofs.h, cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_format.cee", "kernel-services/filesystem/cosmofs/cosmofs_format.c",
     "cosmofs's format: a pool made, its test hooks for older formats and poisoned records (cosmofs.h, "
     "cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_commit.cee", "kernel-services/filesystem/cosmofs/cosmofs_commit.c",
     "cosmofs's commit, its bitmap fixpoint, and the free and orphan records (cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_alloc.cee", "kernel-services/filesystem/cosmofs/cosmofs_alloc.c",
     "cosmofs's allocator, free and orphan lists, and inode map (cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_buf.cee", "kernel-services/filesystem/cosmofs/cosmofs_buf.c",
     "cosmofs's header and superblock seals and the buffer cache (cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_snap.cee", "kernel-services/filesystem/cosmofs/cosmofs_snap.c",
     "cosmofs's snapshots: the list, deadlists, create, delete, the raise to 12 (cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cosmofs_check.cee", "kernel-services/filesystem/cosmofs/cosmofs_check.c",
     "cosmofs's structural check and the check of an unmounted device (cosmofs.h, cosmofs_internal.h)", None, True),
    ("kernel-services/filesystem/cosmofs/cfsparse.cee", "kernel-services/filesystem/cosmofs/cfsparse.c",
     "cosmofs's decoders of on-disk metadata (cosmofs_internal.h)", None, False),
    ("kernel/process/elfparse.cee", "kernel/process/elfparse.c", "The user executable validator (elf.h)", None, False),
    ("kernel/module/modelf.cee", "kernel/module/modelf.c", "ET_REL module image validation and layout (modelf.h)", None,
     False),
    ("kernel/module/modspec.cee", "kernel/module/modspec.c", "The module.cycle= specification's parsers (module.h)", None,
     False),
    ("compat/linux/lxconv.cee", "compat/linux/lxconv.c",
     "The Linux personality's decoders, and the conversions fuzz_linux drives (convert.h)", None, False),
    ("kernel-services/network/netparse.cee", "kernel-services/network/netparse.c",
     "The network receive path's decoders: frames, TCP segments, DHCP and DNS (netparse_types.h)",
     "kernel/include/kernel/net/netparse_types.h", False),
    ("kernel/core/fbvalid.cee", "kernel/core/fbvalid.c", "The framebuffer validator (bootinfo.h)", None, False),
    ("kernel/core/lz4_decode.cee", "kernel/core/lz4_decode.c", "The LZ4 block decoder (lz4.h)", None, False),
    ("kernel-services/network/cksum.cee", "kernel-services/network/cksum.c",
     "The Internet checksum over buffers and mbuf chains (cksum.h)", None, False),
    ("kernel-services/network/inet.cee", "kernel-services/network/inet.c",
     "The address helpers: netaddr equality, the user's sockaddr shape, and netaddr_str (inet.h)", None, False),
    ("kernel-services/network/ether.cee", "kernel-services/network/ether.c",
     "Ethernet framing and dispatch: ether_input, ether_output, eth_broadcast (ether.h)", None, False),
    ("kernel-services/network/loopback.cee", "kernel-services/network/loopback.c",
     "The lo interface: its static netif and ops table, transmit and the test filter (netif.h)", None, False),
    ("kernel-services/network/mbuf.cee", "kernel-services/network/mbuf.c",
     "Packet buffers: mbufs, clusters, chains, queues and their statistics (mbuf.h)", None, True),
    ("kernel-services/network/netif.cee", "kernel-services/network/netif.c",
     "The interface layer: the registry, the per-CPU receive queues and workers, transmit, work items (netif.h)", None, True),
    ("kernel-services/network/arp.cee", "kernel-services/network/arp.c",
     "Address resolution: the neighbour table, requests and replies, ageing (ether.h's ARP half)", None, True),
    ("kernel-services/network/nd.cee", "kernel-services/network/nd.c",
     "Neighbour discovery: the link-local neighbour table, solicitations and advertisements, ageing (ip.h's ND half)",
     None, True),
    ("drivers/storage/diskid.cee", "drivers/storage/diskid.c",
     "What a disk's device reports about itself: ATA IDENTIFY, NVMe Identify, SCSI INQUIRY and READ CAPACITY (diskid.h)",
     "kernel/include/kernel/diskid.h", False),
    ("kernel-services/network/ipv6.cee", "kernel-services/network/ipv6.c",
     "IPv6 input and output, ICMPv6 echo and the dispatch of neighbour discovery (ip.h's IPv6 half)", None, True),
    ("kernel-services/network/ipv4.cee", "kernel-services/network/ipv4.c",
     "IPv4 input, output, routing and forwarding, ICMP, the rate limit and the path MTU cache (ip.h's IPv4 half)",
     None, True),
    ("kernel-services/network/udp.cee", "kernel-services/network/udp.c",
     "UDP over IPv4 and IPv6: binding, ephemeral ports, sending, the receive path, ICMP errors, the listing (udp.h)",
     None, True),
    ("kernel-services/network/raw.cee", "kernel-services/network/raw.c",
     "Raw ICMP sockets over IPv4: their pcbs, sends, and a copy of each ICMP message for every one (raw.h)", None, True),
    ("kernel-services/network/fw.cee", "kernel-services/network/fw.c",
     "The firewall: guests' and the host's rules and policies, the FORWARD, INPUT, host and OUTPUT chains, flows (fw.h)",
     None, True),
    ("kernel-services/network/nat.cee", "kernel-services/network/nat.c",
     "Masquerade NAT and port forwarding over a bounded conntrack table, ICMP errors translated back (nat.h)",
     None, True),
    ("kernel-services/network/socket.cee", "kernel-services/network/socket.c",
     "The socket layer: blocking calls over the TCP, UDP, raw and unix transports, the error word (socket.h)",
     None, True),
    ("kernel-services/network/tcp.cee", "kernel-services/network/tcp.c",
     "TCP: the state machine, retransmission, congestion, the SYN cache and cookies, reassembly (tcp.h)",
     None, True),
    ("kernel-services/network/dhcp.cee", "kernel-services/network/dhcp.c",
     "The DHCP client: one per autoconfigured interface, its lease, renewal and rebinding (dhcp.h)",
     None, True),
    ("kernel-services/network/tapsvc.cee", "kernel-services/network/tapsvc.c",
     "The tap's services: a DHCP server for its one guest and a DNS proxy to the upstream resolver (tapsvc.h)",
     "kernel/include/kernel/net/tapsvc_types.h", True),
    ("kernel-services/network/tap.cee", "kernel-services/network/tap.c",
     "A tap interface and its devices: /dev/net/tap's frame channel and /dev/net/tapctl's commands and snapshot (tap.h)",
     "kernel/include/kernel/net/tap_types.h", True),
    ("kernel-services/network/procnet.cee", "kernel-services/network/procnet.c",
     "/proc/net in Linux's formats: interfaces, routes, ARP, TCP and UDP sockets, IPv6 addresses (procnet.h)",
     None, True),
    ("kernel-services/network/nettest_base.cee", "kernel-services/network/nettest_base.c",
     "The network self-tests of the stack's base: mbufs, the queue, the checksum, the ARP table (selftest.h)",
     None, True),
    ("kernel-services/network/nettest_lo.cee", "kernel-services/network/nettest_lo.c",
     "The network self-tests over loopback: UDP, TCP transfers, the delayed ACK, a Nagle peer, the path MSS, loss (selftest.h, nettest.h)",
     "kernel/include/kernel/net/nettest_types.h", True),
    ("kernel-services/network/nettest_sockerr.cee", "kernel-services/network/nettest_sockerr.c",
     "The network self-tests of the socket's pending error and the receive hook's grace period (selftest.h, nettest.h)",
     "kernel/include/kernel/net/nettest_sockerr_types.h", True),
    ("kernel-services/network/nettest_tcp.cee", "kernel-services/network/nettest_tcp.c",
     "The network stack's hardening self-tests: the SYN cache, RFC 5961, reordering, keepalive, the ICMP limit, raw ICMP, non-blocking I/O (selftest.h, nettest.h)",
     "kernel/include/kernel/net/nettest_tcp_types.h", True),
    ("kernel-services/network/nettest_nif.cee", "kernel-services/network/nettest_nif.c",
     "The network self-tests of receive steering and checksum offload over fake interfaces (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_tap.cee", "kernel-services/network/nettest_tap.c",
     "The tap tests' frame builders and the tap, route, forwarding, tap-filter, DHCP and DNS self-tests (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_fwd.cee", "kernel-services/network/nettest_fwd.c",
     "The network self-tests of inbound port forwarding, many guests and the forwarding firewall (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_ctl.cee", "kernel-services/network/nettest_ctl.c",
     "The network self-test of the control channel, /dev/net/tapctl, and the walk of its snapshot (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_input.cee", "kernel-services/network/nettest_input.c",
     "The network self-test of the INPUT chain and the host chains' checksummed ICMP (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_hostin.cee", "kernel-services/network/nettest_hostin.c",
     "The network self-test of the host chain and the uplink's world side the host tests drive (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_hoststate.cee", "kernel-services/network/nettest_hoststate.c",
     "The network self-tests of the host's own state: the zero-window probe, sender SWS, the FIN that acks the last data, host flows (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_flows.cee", "kernel-services/network/nettest_flows.c",
     "The network self-tests of the operator's flow listing: NAT's and the firewall's flows and shares (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_output.cee", "kernel-services/network/nettest_output.c",
     "The network self-tests of the OUTPUT chain and its verdict carried into the connection (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_tapdev.cee", "kernel-services/network/nettest_tapdev.c",
     "The network self-tests of the tap device's readiness and the kernel's DHCP client (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_hooks.cee", "kernel-services/network/nettest_hooks.c",
     "The network self-tests that need the debug and fault-injection hooks: a pcb's timers against its free, a doubled frame (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_lifetime.cee", "kernel-services/network/nettest_lifetime.c",
     "The network self-tests of an interface's lifetime: registration, unregister against a parked neighbour retry, the flush's counts (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_neigh.cee", "kernel-services/network/nettest_neigh.c",
     "The network self-tests of a neighbour entry per interface: ARP and ND on two links, input and resolve racing the down (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_worker.cee", "kernel-services/network/nettest_worker.c",
     "The network self-tests of an interface against the receive worker: unregister waits for input, a queue count per interface (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_accept.cee", "kernel-services/network/nettest_accept.c",
     "The network self-tests of accept against its races and the census against a wake reference, and the listen backlog (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_seam.cee", "kernel-services/network/nettest_seam.c",
     "The network self-tests held at a seam: one reading of the flag word, a tap's release order, the seeded cookie secret (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_nat.cee", "kernel-services/network/nettest_nat.c",
     "The network self-tests of masquerade across two taps and of a second interface taking over (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_nicbench.cee", "kernel-services/network/nettest_nicbench.c",
     "The NIC-path benchmark: ARP round trips at the driver boundary, a UDP window, the checksum's share (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_bench.cee", "kernel-services/network/nettest_bench.c",
     "The loopback benchmark: TCP with one and two flows and UDP, steering off and on (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_harness.cee", "kernel-services/network/nettest_harness.c",
     "The host harness's guest side: echo services and the back-connection's retried exchange (selftest.h, nettest.h)",
     None, True),
    ("kernel-services/network/nettest_frame.cee", "kernel-services/network/nettest_frame.c",
     "The network self-tests' release framework: the census, the releases however a test returns, the servers and the host's connect (selftest.h, nettest.h)",
     None, True),
    ("drivers/virtio/virtq_used.cee", "drivers/virtio/virtq_used.c",
     "The virtqueue's decoder of used elements (virtio.h; linked into the virtio module)", None, False),
    ("drivers/usb/usb_desc.cee", "drivers/usb/usb_desc.c",
     "The USB configuration descriptor decoder (usb.h; linked into the xhci module)", None, False),
    ("drivers/acpi/acpi_parse.cee", "drivers/acpi/acpi_parse.c",
     "The ACPI table walker's decoders: checksum, RSDP, root table, MADT (acpi_internal.h)", None, False),
    ("pkg/pkgparse.cee", "pkg/pkgparse.c",
     "The package manager's decoders: tar, versions, names, paths, hex, times, manifests, the INDEX (pkgparse.h)",
     None, False),
    ("kernel/ceesafe/crash_test.cee", "kernel/ceesafe/crash_test.c",
     "The crash test crash-cee: a CeeSafe bounds check that fails on purpose (CRASH_TEST=5)",
     "kernel/include/ceesafe/crash_test.h", False),
    ("kernel/ceesafe/kcore_test.cee", "kernel/ceesafe/kcore_test.c",
     "The boot self-test of CeeSafe's kernel library (selftest cee-kcore)", None, True),
    ("kernel/ceesafe/async_test.cee", "kernel/ceesafe/async_test.c",
     "The boot self-test of CeeSafe's async executor (selftest cee-async; CeeSafe ADR 0039)", None, False),
]
# ceesafe/foreign.h (CeeSafe ADRs 0060-0061, ceesafec 0.12.0) is the C side's allocation calls, which rt.h includes from
# beside itself.
RUNTIME = [("ceesafe/rt.h", "kernel/include/ceesafe/rt.h"), ("ceesafe/foreign.h", "kernel/include/ceesafe/foreign.h"),
           ("ceesafe_rt_core.c", "kernel/ceesafe/ceesafe_rt_core.c")]
# The async executor (CeeSafe ADR 0039), which only a port that uses async emits: the same from each that does.
ASYNC_RUNTIME = ("ceesafe_rt_async.c", "kernel/ceesafe/ceesafe_rt_async.c")

# The device API every CeeSafe driver module is compiled with (drivers/ceesafe/README.md).
DEVICE_API = ["drivers/ceesafe/driver.cee", "drivers/ceesafe/virtio.cee"]

# Each CeeSafe driver module: its name, its driver source, the C it generates, what it is, and its COSMO_MODULE
# entry (kernel/include/kernel/module.h): version, dependencies (comma separated) and capabilities. The source exports
# <name>_module_init and <name>_module_shutdown, which the entry names; build/module.mk links the module.
MODULES = [
    ("virtio_rng", "drivers/virtio/virtio_rng.cee", "drivers/virtio/virtio_rng.c",
     "The virtio entropy device (module virtio_rng)", "1.0", "virtio", "MODULE_CAP_DRIVER"),
]
MODULE_RUNTIME = ("ceesafe_rt_core.c", "drivers/ceesafe/ceesafe_rt_core.c")
# The driver template (drivers/ceesafe/README.md): never built, but checked with the device API on every run but the
# build's own, so it keeps compiling as the API changes.
TEMPLATE = "drivers/ceesafe-template/template.cee"

MODULE_BANNER = """/*
 * {name} - {title}, generated from {source}
 * and the device API (drivers/ceesafe/driver.cee, virtio.cee).
 *
 * GENERATED: do not edit. Change {source} and run `make cee`
 * (scripts/cee-gen.py), which compiles it with the pinned ceesafec. The
 * module links CeeSafe's runtime (drivers/ceesafe/ceesafe_rt_core.c, also
 * generated) and the driver floor (drivers/ceesafe/driver_floor.c).
 */
"""

MODULE_ENTRY = """
/* The module's entry (kernel/include/kernel/module.h), from scripts/cee-gen.py's MODULES. */
#include <kernel/module.h>

COSMO_MODULE("{name}", "{version}", {name}_module_init, {name}_module_shutdown, "{deps}", {caps});
"""

BANNER = """/*
 * {name} - {title}, generated from {source}.
 *
 * GENERATED: do not edit. Change {source} and run `make cee`
 * (scripts/cee-gen.py), which compiles it with the pinned ceesafec. It needs
 * CeeSafe's runtime (kernel/include/ceesafe/rt.h, kernel/ceesafe/ceesafe_rt_core.c, also
 * generated) and a ceesafe_panic for the environment it is linked into
 * (kernel/ceesafe/ceesafe_panic.c in the kernel).
 */
"""


def ceesafec() -> str:
    exe = os.environ.get("CEESAFEC") or shutil.which("ceesafec")
    if exe is None:
        sys.exit(f"cee-gen: no ceesafec: set CEESAFEC or put ceesafec {CEESAFEC_VERSION} on PATH (see the usage in {__file__})")
    out = subprocess.run([exe, "--version"], capture_output=True, text=True).stdout.strip()
    if out != f"ceesafec {CEESAFEC_VERSION}":
        sys.exit(f"cee-gen: {exe} is '{out}', not the pinned ceesafec {CEESAFEC_VERSION}")
    return exe


def generated(exe: str, work: Path) -> dict[str, str]:
    """Every generated file, by its path in the tree, as the pinned ceesafec makes it now."""
    files: dict[str, str] = {}
    for i, (source, target, title, header, kernel) in enumerate(PORTS):
        out = work / str(i)
        # The exports header is included by its name under kernel/include (kernel/handle_types.h): the generated C
        # includes it by that name rather than defining its structs again, and its guard comes from it (CeeSafe ADR 0030).
        include = None if header is None else str(Path(header).relative_to("kernel/include"))
        subprocess.run([exe, "build", "--profile", "freestanding", "--no-float", *(["--kernel", "cosmoos"] if kernel else []),
                        *(["--exports-header", include] if include else []), "--emit", "c", "-o", out, source],
                       cwd=ROOT, check=True)
        main = (out / "main.c").read_text()
        files[target] = BANNER.format(name=Path(target).name, title=title, source=source) + main
        if include is not None:
            exports = (out / include).read_text()
            files[header] = BANNER.format(name=Path(header).name, title=title + ": its C interface",
                                          source=source) + exports
        for made, path in RUNTIME:
            text = (out / made).read_text()
            if files.setdefault(path, text) != text:
                sys.exit(f"cee-gen: {source}'s {path} differs from the other ports'")
        made, path = ASYNC_RUNTIME
        if (out / made).exists():
            text = (out / made).read_text()
            if files.setdefault(path, text) != text:
                sys.exit(f"cee-gen: {source}'s {path} differs from the other async ports'")
    for i, (name, source, target, title, version, deps, caps) in enumerate(MODULES):
        out = work / f"module-{i}"
        subprocess.run([exe, "build", "--profile", "freestanding", "--no-float", "--kernel", "cosmoos", "--emit", "c",
                        "-o", out, *DEVICE_API, source], cwd=ROOT, check=True)
        files[target] = (MODULE_BANNER.format(name=Path(target).name, title=title, source=source)
                         + (out / "main.c").read_text()
                         + MODULE_ENTRY.format(name=name, version=version, deps=deps, caps=caps))
        made, path = MODULE_RUNTIME
        text = (out / made).read_text()
        if files.setdefault(path, text) != text:
            sys.exit(f"cee-gen: module {name}'s {path} differs from the other modules'")
    return files


# CeeSafe sources the kernel must refuse (roadmap 2.0 SA3: a sleep under a spin lock's guard does not compile). Each line
# marked `// error[N…]` must be refused with that code, and nothing else may be: the rest are the cases that compile.
NEGATIVE = sorted(p.relative_to(ROOT).as_posix() for p in (ROOT / "tests" / "ceesafe" / "negative").glob("*.cee"))


def refused(exe: str) -> list[str]:
    """What each negative source's diagnostics got wrong, against its annotations; empty when every one is right."""
    wrong = []
    for source in NEGATIVE:
        expected = set()
        for number, line in enumerate((ROOT / source).read_text().splitlines(), 1):
            expected.update((number, code) for code in re.findall(r"//\s*error\[(N\d{4})\]", line))
        run = subprocess.run([exe, "check", "--profile", "freestanding", "--no-float", "--kernel", "cosmoos",
                              "--diagnostics-format=json", source], cwd=ROOT, capture_output=True, text=True)
        try:
            found = {(d["start"]["line"], d["code"]) for d in json.loads(run.stdout)["diagnostics"]
                     if d["severity"] == "error"}
        except (ValueError, KeyError):
            wrong.append(f"{source}: ceesafec gave no diagnostics: {(run.stdout + run.stderr).strip()[:200]}")
            continue
        wrong += [f"{source}:{n}: expected error[{c}], which ceesafec did not give" for n, c in sorted(expected - found)]
        wrong += [f"{source}:{n}: ceesafec gave error[{c}], which is not expected" for n, c in sorted(found - expected)]
        if not expected:
            wrong.append(f"{source}: no `// error[N…]` line: a negative test must expect a refusal")
    run = subprocess.run([exe, "check", "--profile", "freestanding", "--no-float", "--kernel", "cosmoos", *DEVICE_API,
                          TEMPLATE], cwd=ROOT, capture_output=True, text=True)
    if run.returncode != 0:
        wrong.append(f"{TEMPLATE}: does not compile with the device API: {(run.stdout + run.stderr).strip()[:300]}")
    return wrong


def outputs() -> list[str]:
    """Every generated file's path, relative to the tree, without running ceesafec (make's list)."""
    paths = [target for _, target, _, _, _ in PORTS] + [h for _, _, _, h, _ in PORTS if h] + [p for _, p in RUNTIME]
    paths.append(ASYNC_RUNTIME[1])  # kernel/ceesafe/async_test.cee uses async
    paths += [target for _, _, target, _, _, _, _ in MODULES] + ([MODULE_RUNTIME[1]] if MODULES else [])
    return sorted(set(paths))


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate the C of CosmoOS's CeeSafe sources.")
    parser.add_argument("--out", default="out/cee",
                        help="the directory the generated files go under, at their paths in the tree (default out/cee; "
                             "`make dist` writes them into the tree it packs)")
    parser.add_argument("--check", action="store_true", help="compare with the files under --out instead of writing")
    parser.add_argument("--list", action="store_true", help="print each generated file's path and exit (no ceesafec)")
    parser.add_argument("--no-negative", action="store_true", help="skip the negative sources (the build's own run)")
    parser.add_argument("--pinned", action="store_true", help="print the pinned ceesafec version and exit")
    args = parser.parse_args()
    if args.pinned:
        print(CEESAFEC_VERSION)
        return 0
    if args.list:
        print("\n".join(outputs()))
        return 0
    out = (ROOT / args.out).resolve()
    exe = ceesafec()
    with tempfile.TemporaryDirectory() as temp:
        files = generated(exe, Path(temp))
    stale = []
    for path, text in files.items():
        target = out / path
        if target.exists() and target.read_text() == text:
            continue
        stale.append(path)
        if not args.check:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text)
    where = out.relative_to(ROOT) if out.is_relative_to(ROOT) else out
    for path in stale:
        print(f"cee-gen: {'differs from its CeeSafe source' if args.check else 'wrote'}: {where}/{path}")
    if not stale:
        print(f"cee-gen: {len(files)} generated files under {where} match their CeeSafe sources (ceesafec {CEESAFEC_VERSION})")
    elif args.check:
        print(f"cee-gen: {where} is not what the pinned ceesafec makes of the CeeSafe sources")
    wrong = [] if args.no_negative else refused(exe)
    for line in wrong:
        print(f"cee-gen: {line}")
    if not wrong and not args.no_negative:
        print(f"cee-gen: {len(NEGATIVE)} negative source(s) refused as their annotations say; {TEMPLATE} compiles")
    return 1 if (args.check and stale) or wrong else 0


if __name__ == "__main__":
    sys.exit(main())
