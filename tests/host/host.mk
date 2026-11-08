# Host-side unit tests: kernel algorithms compiled natively with ASan and
# UBSan. The shim directory supplies host versions of the arch headers;
# everything else is the real kernel source.

HOST_CC ?= $(CC)
HOST_OUT := $(OUT)/host

HOST_CFLAGS := \
	-std=c11 -g -O1 -fno-omit-frame-pointer \
	-fsanitize=address,undefined -fno-sanitize-recover=undefined \
	-Wall -Wextra -Werror -Wshadow -Wno-unused-function \
	-DCONFIG_DEBUG=1 -DCONFIG_SELFTEST=0 -DCONFIG_CRASH_TEST=0 -DCONFIG_FAULTINJECT=0 -DCONFIG_PERCPU_WARN=0 -DCONFIG_SCHED_CHAOS=0 \
	-I$(ROOT)/tests/host/shim \
	-I$(ROOT)/tests/host \
	$(CEE_CFLAGS) \
	-I$(ROOT)/kernel/include \
	-I$(ROOT)/kernel/memory \
	-I$(ROOT)/boot/protocol

HOST_LDFLAGS := -fsanitize=address,undefined

HOST_COMMON_SRCS := \
	tests/host/harness.c \
	tests/host/shim_spinlock.c \
	kernel/memory/buddy.c

HOST_BUDDY_SRCS := $(HOST_COMMON_SRCS) tests/host/test_buddy.c
HOST_SLAB_SRCS  := $(HOST_COMMON_SRCS) kernel/memory/slab.c kernel/memory/kmalloc.c tests/host/test_slab.c
HOST_CRYPTO_SRCS := $(HOST_COMMON_SRCS) kernel/security/sha512.c kernel/security/ed25519.c kernel/security/blake2b.c \
	kernel/core/crc32c.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_crypto.c
HOST_RELSIG_SRCS := $(HOST_COMMON_SRCS) kernel/security/sha512.c kernel/security/ed25519.c userland/system/relsig.c tests/host/test_relsig.c
HOST_MODELF_SRCS := $(HOST_COMMON_SRCS) kernel/module/modelf.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_modelf.c
HOST_COSMOFS_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cfsparse.c kernel/core/crc32c.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_cosmofs.c
HOST_NETIF_SRCS := $(HOST_COMMON_SRCS) kernel-services/network/netif.c kernel-services/network/mbuf.c \
	kernel-services/network/cksum.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_netif.c
HOST_MBUF_SRCS := $(HOST_COMMON_SRCS) kernel-services/network/mbuf.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_mbuf.c
HOST_LOOPBACK_SRCS := $(HOST_COMMON_SRCS) kernel-services/network/loopback.c kernel-services/network/mbuf.c \
	kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_loopback.c
HOST_ETHER_SRCS := $(HOST_COMMON_SRCS) kernel-services/network/ether.c kernel-services/network/mbuf.c \
	kernel-services/network/netparse.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_ether.c
HOST_ARP_SRCS := $(HOST_COMMON_SRCS) kernel-services/network/arp.c kernel-services/network/mbuf.c \
	kernel-services/network/netparse.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_arp.c
HOST_ND_SRCS := $(HOST_COMMON_SRCS) kernel-services/network/nd.c kernel-services/network/mbuf.c \
	kernel-services/network/cksum.c kernel-services/network/netparse.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_nd.c
HOST_DISKID_SRCS := $(HOST_COMMON_SRCS) drivers/storage/diskid.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_diskid.c
HOST_IPV6_SRCS := $(HOST_COMMON_SRCS) kernel-services/network/ipv6.c kernel-services/network/mbuf.c \
	kernel-services/network/cksum.c kernel-services/network/netparse.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_ipv6.c
HOST_IPV4_SRCS := $(HOST_COMMON_SRCS) kernel-services/network/ipv4.c kernel-services/network/mbuf.c \
	kernel-services/network/cksum.c kernel-services/network/netparse.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_ipv4.c
HOST_UDP_SRCS := $(HOST_COMMON_SRCS) kernel-services/network/udp.c kernel-services/network/mbuf.c \
	kernel-services/network/cksum.c kernel-services/network/netparse.c kernel-services/network/inet.c \
	kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_udp.c
HOST_RAW_SRCS := $(HOST_COMMON_SRCS) kernel-services/network/raw.c kernel-services/network/mbuf.c \
	kernel-services/network/inet.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_raw.c
HOST_FW_SRCS := $(HOST_COMMON_SRCS) kernel-services/network/fw.c kernel-services/network/mbuf.c \
	kernel-services/network/netparse.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_fw.c
HOST_NAT_SRCS := $(HOST_COMMON_SRCS) kernel-services/network/nat.c kernel-services/network/mbuf.c \
	kernel-services/network/cksum.c kernel-services/network/netparse.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_nat.c
HOST_SOCKET_SRCS := $(HOST_COMMON_SRCS) kernel-services/network/socket.c kernel-services/network/socket_floor.c \
	kernel-services/network/mbuf.c \
	kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_socket.c
# TCP over the TCP fuzz target's host (tests/fuzz/shim_net.c) and the rest of the stack as the kernel's.
HOST_TCP_SRCS := $(HOST_COMMON_SRCS) tests/fuzz/shim_net.c kernel-services/network/ether.c kernel-services/network/arp.c \
	kernel-services/network/nd.c kernel-services/network/ipv4.c kernel-services/network/ipv6.c \
	kernel-services/network/udp.c kernel-services/network/raw.c kernel-services/network/tcp.c \
	kernel-services/network/tcp_floor.c kernel-services/network/nat.c kernel-services/network/fw.c \
	kernel-services/network/mbuf.c kernel-services/network/cksum.c kernel-services/network/inet.c \
	kernel-services/network/dhcp.c kernel-services/network/netparse.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_tcp.c
# The DHCP client over the same host as TCP's.
HOST_DHCP_SRCS := $(filter-out tests/host/test_tcp.c,$(HOST_TCP_SRCS)) tests/host/test_dhcp.c
# The tap's DHCP server and DNS proxy over the same host, its tap, sockets and threads the test's.
HOST_TAPSVC_SRCS := $(filter-out tests/host/test_dhcp.c,$(HOST_DHCP_SRCS)) kernel-services/network/tapsvc.c \
	tests/host/test_tapsvc.c
# The tap and its devices alone, over real mbufs: everything else is the test's.
HOST_TAP_SRCS := $(HOST_COMMON_SRCS) kernel-services/network/tap.c kernel-services/network/mbuf.c \
	kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_tap.c
# /proc/net's renderer alone: the listings and the allocator are the test's.
HOST_PROCNET_SRCS := $(HOST_COMMON_SRCS) kernel-services/network/procnet.c kernel-services/network/procnet_floor.c kernel/core/printf.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_procnet.c
HOST_MEMDEV_SRCS := $(HOST_COMMON_SRCS) kernel-services/vfs/memdev.c kernel-services/vfs/memdev_floor.c \
	kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_memdev.c
HOST_MOUNTNS_SRCS := $(HOST_COMMON_SRCS) kernel-services/vfs/mountns.c kernel-services/vfs/mountns_floor.c \
	kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_mountns.c
HOST_BLKCTL_SRCS := $(HOST_COMMON_SRCS) kernel-services/vfs/blkctl.c kernel-services/vfs/blkctl_floor.c \
	kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_blkctl.c
HOST_PAGECACHE_SRCS := $(HOST_COMMON_SRCS) kernel-services/vfs/pagecache.c kernel-services/vfs/pagecache_floor.c \
	kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_pagecache.c
HOST_RAMFS_SRCS := $(HOST_COMMON_SRCS) kernel-services/vfs/ramfs.c kernel-services/vfs/ramfs_floor.c \
	kernel/core/printf.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_ramfs.c
HOST_FSCTL_SRCS := $(HOST_COMMON_SRCS) kernel-services/vfs/fsctl.c kernel-services/vfs/fsctl_floor.c \
	kernel-services/vfs/mountns_floor.c kernel/core/printf.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c \
	tests/host/test_fsctl.c
# The VFS core over a filesystem of the test's own: mount namespaces are the kernel's, the page cache, objects,
# credentials, processes, devices and waits the test's.
HOST_VFS_SRCS := $(HOST_COMMON_SRCS) kernel-services/vfs/vfs_dir.c kernel-services/vfs/vfs_vnode.c kernel-services/vfs/vfs_mount.c \
	kernel-services/vfs/vfs_walk.c kernel-services/vfs/vfs_file.c kernel-services/vfs/vfs_floor.c kernel-services/vfs/mountns.c kernel-services/vfs/mountns_floor.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_vfs.c
# /proc: the process table, terminals, handles, scheduler, clocks, memory figures, mounts, the network stack's
# tables and the vnode cache are the test's.
HOST_PROCFS_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/procfs/procfs.c \
	kernel-services/filesystem/procfs/procfs_floor.c kernel/core/printf.c \
	kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_procfs.c
HOST_INET_SRCS := $(HOST_COMMON_SRCS) kernel-services/network/inet.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_inet.c
HOST_CKSUM_SRCS := $(HOST_COMMON_SRCS) kernel-services/network/cksum.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_cksum.c
HOST_LZ4_SRCS := $(HOST_COMMON_SRCS) kernel/core/lz4.c kernel/core/lz4_decode.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_lz4.c
HOST_CHACHA_SRCS := $(HOST_COMMON_SRCS) kernel/security/chacha20.c tests/host/test_chacha20.c
HOST_KEYSLOT_SRCS := $(HOST_COMMON_SRCS) kernel/security/chacha20.c kernel/security/blake2b.c userland/lib/argon2.c \
	userland/lib/keyslot.c tests/host/test_keyslot.c
HOST_LIBC_SRCS := tests/host/test_libc.c
HOST_PKG_SRCS := tests/host/test_pkg.c pkg/manifest.c pkg/pkgparse.c pkg/tar.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c
HOST_LINUX_SRCS := tests/host/test_linux.c compat/linux/convert.c compat/linux/lxconv.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c
HOST_HV_SRCS := $(HOST_COMMON_SRCS) kernel/arch/x86_64/svm_npt.c tests/host/test_hv.c
HOST_FDT_SRCS := $(HOST_COMMON_SRCS) tools/fdt/fdt.c tools/fdt/fdt_read.c tests/host/test_fdt.c
HOST_VBLK_SRCS := $(HOST_COMMON_SRCS) userland/system/vq.c userland/system/vblk.c tests/host/test_vblk_dev.c
HOST_VNET_SRCS := $(HOST_COMMON_SRCS) userland/system/vq.c userland/system/vnet.c tests/host/test_vnet_dev.c
HOST_VMX_SRCS := $(HOST_COMMON_SRCS) kernel/arch/x86_64/vmx_ept.c tests/host/test_vmx.c
HOST_HV_S2_SRCS := $(HOST_COMMON_SRCS) kernel/arch/aarch64/hv_s2.c tests/host/test_hv_s2.c
HOST_RELOC_A64_SRCS := $(HOST_COMMON_SRCS) kernel/arch/aarch64/modreloc.c tests/host/test_reloc_aarch64.c
HOST_VIRTQ_SRCS := $(HOST_COMMON_SRCS) drivers/virtio/virtqueue.c drivers/virtio/virtq_used.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_virtq.c
HOST_CRED_SRCS := $(HOST_COMMON_SRCS) kernel/process/cred.c tests/host/test_cred.c
HOST_QUIESCE_SRCS := $(HOST_COMMON_SRCS) tests/host/test_quiesce.c
HOST_SYNC_SRCS := $(HOST_COMMON_SRCS) kernel/scheduler/wait.c kernel/scheduler/wait_floor.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c kernel/scheduler/completion.c kernel/scheduler/completion_floor.c kernel/scheduler/semaphore.c \
	kernel/scheduler/mutex.c kernel/scheduler/mutex_floor.c tests/host/sync_baton.c tests/host/sync_sched.c tests/host/test_sync.c
HOST_LOCKDEP_SRCS := $(HOST_COMMON_SRCS) tests/host/test_lockdep.c
HOST_LOCKUP_SRCS := $(HOST_COMMON_SRCS) tests/host/test_lockup.c
HOST_FBVALID_SRCS := $(HOST_COMMON_SRCS) kernel/core/fbvalid.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_fbvalid.c
HOST_DISPLAY_SRCS := $(HOST_COMMON_SRCS) boot/uefi/display_pick.c tests/host/test_display.c
# Partitions alone: the block layer, the GPT parser, the threads and the allocator are the test's.
HOST_PART_SRCS := $(HOST_COMMON_SRCS) kernel/block/part.c kernel/core/printf.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_part.c
# The RAM block device alone: the block layer, the threads and the allocator are the test's.
HOST_RAMBLK_SRCS := $(HOST_COMMON_SRCS) kernel/block/ramblk.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c \
	tests/host/test_ramblk.c
# The block layer alone: the drivers, the threads, the clock, the partitions and the allocator are the test's.
HOST_BLK_SRCS := $(HOST_COMMON_SRCS) kernel/block/blk.c kernel/core/printf.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_blk.c
# The storage pool alone: the block layer's synchronous I/O, the references and the allocator are the test's.
HOST_POOL_SRCS := $(HOST_COMMON_SRCS) kernel-services/storage/pool.c kernel-services/storage/pool_floor.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_pool.c
HOST_GPT_SRCS := $(HOST_COMMON_SRCS) kernel/block/gpt.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c kernel/core/cmdline.c tests/host/test_gpt.c
HOST_CPUTIME_SRCS := $(HOST_COMMON_SRCS) kernel/process/cputime.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_cputime.c
HOST_NICE_SRCS := $(HOST_COMMON_SRCS) kernel/process/nice.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_nice.c
HOST_FAIR_SRCS := $(HOST_COMMON_SRCS) kernel/scheduler/fair.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_fair.c
HOST_UPBE_SRCS := $(HOST_COMMON_SRCS) userland/system/upgrade_be.c tests/host/test_upgrade_be.c
HOST_NETPARSE_SRCS := $(HOST_COMMON_SRCS) kernel-services/network/netparse.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_netparse.c
HOST_ELF_SRCS := $(HOST_COMMON_SRCS) kernel/process/elfparse.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_elf.c
HOST_ACPI_SRCS := $(HOST_COMMON_SRCS) drivers/acpi/acpi_parse.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_acpi.c
HOST_USBDESC_SRCS := $(HOST_COMMON_SRCS) drivers/usb/usb_desc.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_usbdesc.c
# cosmofs's tree walk alone: the buffers, the address map and the allocator are the test's.
HOST_CFSREACH_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_reach.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_cfsreach.c
# cosmofs's clones alone: the snapshot list, the trees, the transaction and the allocator are the test's.
HOST_CFSCLONE_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_clone.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_cfsclone.c
# cosmofs's rollback alone, with the record's real checks: the list, the trees, the inodes, the blocks and the transaction are the test's.
HOST_CFSROLLBACK_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_rollback.c \
	kernel-services/filesystem/cosmofs/cosmofs_rollback_floor.c kernel-services/filesystem/cosmofs/cfsparse.c kernel/core/crc32c.c kernel/core/printf.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_cfsrollback.c
# cosmofs's scrub alone: the blocks and their copies, the lists, the trees, the lock and the clock are the test's.
HOST_CFSSCRUB_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_scrub.c \
	kernel-services/filesystem/cosmofs/cosmofs_scrub_floor.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_cfsscrub.c
# cosmofs's keys with the real ciphers and hashes: the pool, the buffers, the transaction and the random source are the test's.
HOST_CFSCRYPT_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_crypt.c \
	kernel-services/filesystem/cosmofs/cosmofs_crypt_floor.c kernel/security/chacha20.c \
	kernel/security/sha512.c kernel/security/blake2b.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c \
	tests/host/test_cfscrypt.c
# cosmofs's resilver and pool commands: the pool's operations, the labels, the holds and the devices are the test's.
HOST_CFSRESILVER_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_resilver.c \
	kernel-services/filesystem/cosmofs/cosmofs_resilver_floor.c kernel-services/storage/pool_floor.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_cfsresilver.c
# cosmofs's members with the real label and table checks: the devices, the pool, the buffers and the allocator are the test's.
HOST_CFSMEMBER_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_member.c \
	kernel-services/filesystem/cosmofs/cosmofs_member_floor.c kernel-services/storage/pool_floor.c \
	kernel-services/filesystem/cosmofs/cfsparse.c kernel/core/crc32c.c kernel/core/printf.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_cfsmember.c
# cosmofs's structural check with the real entry decoders: the blocks, the trees, the lists, the mount and the device are the test's.
HOST_CFSCHECK_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_check.c \
	kernel-services/filesystem/cosmofs/cosmofs_check_floor.c \
	kernel-services/filesystem/cosmofs/cfsparse.c kernel/core/crc32c.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_cfscheck.c
# cosmofs's snapshots with the real list decoders: the blocks, the buffers, the allocator, the transaction and the clones are the test's.
HOST_CFSSNAP_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_snap.c \
	kernel-services/filesystem/cosmofs/cosmofs_snap_floor.c \
	kernel-services/filesystem/cosmofs/cfsparse.c kernel/core/crc32c.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_cfssnap.c
# cosmofs's checksums, headers and buffer cache with the real BLAKE2b, CRC32C and decoders: the pool, the repair, the key table and the allocator are the test's.
HOST_CFSBUF_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_buf.c \
	kernel-services/filesystem/cosmofs/cosmofs_buf_floor.c \
	kernel-services/filesystem/cosmofs/cfsparse.c kernel/core/crc32c.c kernel/security/blake2b.c \
	kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_cfsbuf.c
# cosmofs's allocation and inode map with the real address and slot checks: the buffers, the clone and the transaction are the test's.
HOST_CFSALLOC_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_alloc.c \
	kernel-services/filesystem/cosmofs/cfsparse.c kernel/core/crc32c.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_cfsalloc.c
# cosmofs's commit with the real buffer cache, allocator, inode map and decoders: the disk, the pool and every other hook are the test's.
HOST_CFSCOMMIT_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_commit.c \
	kernel-services/filesystem/cosmofs/cosmofs_commit_floor.c \
	kernel-services/filesystem/cosmofs/cosmofs_buf.c kernel-services/filesystem/cosmofs/cosmofs_buf_floor.c \
	kernel-services/filesystem/cosmofs/cosmofs_alloc.c kernel-services/filesystem/cosmofs/cfsparse.c kernel/core/crc32c.c \
	kernel/security/blake2b.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_cfscommit.c
# cosmofs's format with the real slot check and CRC32C: the devices, the pool, the seals and the key and label writers are the test's.
HOST_CFSFORMAT_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_format.c \
	kernel-services/filesystem/cosmofs/cfsparse.c kernel/core/crc32c.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_cfsformat.c
# cosmofs's corruption hook with the real DVA mapping and entry and list decoders: the inodes, directories, buffers and allocators are the test's.
HOST_CFSCORRUPT_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_corrupt.c \
	kernel-services/filesystem/cosmofs/cfsparse.c kernel/core/crc32c.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_cfscorrupt.c
# cosmofs's stats, key operations and test hooks: the filesystem and everything it calls are the test's.
HOST_CFSHOOKS_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_hooks.c \
	kernel-services/filesystem/cosmofs/cosmofs_hooks_floor.c \
	kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_cfshooks.c
# cosmofs's mount and unmount: the pool, the buffers, the members, the key table, the replays, threads and the VFS are the test's.
HOST_CFSCORE_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_core.c \
	kernel-services/filesystem/cosmofs/cosmofs_core_floor.c \
	kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_cfscore.c
# cosmofs's snapshot fsctl: the snapshots, the walks, the rollback record and the lock are the test's.
HOST_CFSSNAPCTL_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_snapctl.c \
	kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_cfssnapctl.c
# cosmofs's file checksums with the real CRC32C: the buffer cache, the copies, the cipher and the codec are the test's.
HOST_CFSCSUM_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_csum.c kernel/core/crc32c.c \
	kernel-services/filesystem/cosmofs/cosmofs_csum_floor.c \
	kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_cfscsum.c
# a cosmofs file's blocks with the real DVA and run checks and CRC32C: the cache, allocator, entries, cipher and codec are the test's.
HOST_CFSDATA_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_data.c \
	kernel-services/filesystem/cosmofs/cosmofs_data_floor.c \
	kernel-services/filesystem/cosmofs/cfsparse.c kernel/core/crc32c.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_cfsdata.c
# a cosmofs regular file's vnode operations: the map, reads, writes, inode, locks and commit are the test's.
HOST_CFSFILE_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_file.c \
	kernel-services/filesystem/cosmofs/cosmofs_file_floor.c \
	kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_cfsfile.c
# cosmofs's vnodes: the cache, the inodes and the operation tables are the test's.
HOST_CFSVNODE_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_vnode.c \
	kernel-services/filesystem/cosmofs/cosmofs_vnode_floor.c \
	kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_cfsvnode.c
# cosmofs's directory blocks, .snapshots and tree locks with the real entry decoders: the rest is the test's.
HOST_CFSDIRBLK_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs.c \
	kernel-services/filesystem/cosmofs/cosmofs_floor.c \
	kernel-services/filesystem/cosmofs/cfsparse.c kernel/core/crc32c.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_cfsdirblk.c
# cosmofs's directory operations with the real directory helpers and entry decoders: the rest is the test's.
HOST_CFSDIR_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cosmofs_dir.c \
	kernel-services/filesystem/cosmofs/cosmofs.c kernel-services/filesystem/cosmofs/cosmofs_floor.c \
	kernel-services/filesystem/cosmofs/cfsparse.c kernel/core/crc32c.c kernel/ceesafe/ceesafe_rt_core.c \
	tests/host/ceesafe_panic.c tests/host/test_cfsdir.c
HOST_CFSPARSE_SRCS := $(HOST_COMMON_SRCS) kernel-services/filesystem/cosmofs/cfsparse.c kernel/core/crc32c.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_cfsparse.c
HOST_HANDLES_SRCS := $(HOST_COMMON_SRCS) kernel/object/object.c kernel/object/object_floor.c kernel/object/handle.c kernel/object/handle_floor.c kernel/ceesafe/ceesafe_rt_core.c tests/host/ceesafe_panic.c tests/host/test_handles.c
HOST_PCI_BAR_SRCS := $(HOST_COMMON_SRCS) tests/host/test_pci_bar.c
HOST_JITTER_SRCS := $(HOST_COMMON_SRCS) kernel/core/jitter.c tests/host/test_jitter.c

HOST_TESTS := $(HOST_OUT)/test_cksum $(HOST_OUT)/test_inet $(HOST_OUT)/test_ether $(HOST_OUT)/test_loopback $(HOST_OUT)/test_mbuf $(HOST_OUT)/test_netif $(HOST_OUT)/test_arp $(HOST_OUT)/test_nd $(HOST_OUT)/test_diskid $(HOST_OUT)/test_ipv6 $(HOST_OUT)/test_ipv4 $(HOST_OUT)/test_udp $(HOST_OUT)/test_raw $(HOST_OUT)/test_fw $(HOST_OUT)/test_nat $(HOST_OUT)/test_socket $(HOST_OUT)/test_tcp $(HOST_OUT)/test_dhcp $(HOST_OUT)/test_tapsvc $(HOST_OUT)/test_tap $(HOST_OUT)/test_procnet $(HOST_OUT)/test_procfs $(HOST_OUT)/test_memdev $(HOST_OUT)/test_mountns $(HOST_OUT)/test_blkctl $(HOST_OUT)/test_pagecache $(HOST_OUT)/test_ramfs $(HOST_OUT)/test_fsctl $(HOST_OUT)/test_vfs $(HOST_OUT)/test_interrupt $(HOST_OUT)/test_buddy $(HOST_OUT)/test_slab $(HOST_OUT)/test_crypto $(HOST_OUT)/test_relsig $(HOST_OUT)/test_modelf $(HOST_OUT)/test_cosmofs $(HOST_OUT)/test_libc $(HOST_OUT)/test_pkg $(HOST_OUT)/test_linux $(HOST_OUT)/test_hv $(HOST_OUT)/test_vmx $(HOST_OUT)/test_hv_s2 $(HOST_OUT)/test_reloc_aarch64 $(HOST_OUT)/test_virtq $(HOST_OUT)/test_cred $(HOST_OUT)/test_quiesce $(HOST_OUT)/test_lockdep $(HOST_OUT)/test_lockdep_threads $(HOST_OUT)/test_lockup $(HOST_OUT)/test_lz4 $(HOST_OUT)/test_chacha20 $(HOST_OUT)/test_keyslot $(HOST_OUT)/test_fbvalid $(HOST_OUT)/test_display $(HOST_OUT)/test_fdt $(HOST_OUT)/test_vblk_dev $(HOST_OUT)/test_vnet_dev $(HOST_OUT)/test_gpt $(HOST_OUT)/test_part $(HOST_OUT)/test_ramblk $(HOST_OUT)/test_blk $(HOST_OUT)/test_pool $(HOST_OUT)/test_cputime $(HOST_OUT)/test_nice $(HOST_OUT)/test_fair $(HOST_OUT)/test_upgrade_be $(HOST_OUT)/test_pci_bar $(HOST_OUT)/test_jitter $(HOST_OUT)/test_handles $(HOST_OUT)/test_netparse $(HOST_OUT)/test_elf $(HOST_OUT)/test_cfsparse $(HOST_OUT)/test_cfsreach $(HOST_OUT)/test_cfsclone $(HOST_OUT)/test_cfsrollback $(HOST_OUT)/test_cfsscrub $(HOST_OUT)/test_cfscrypt $(HOST_OUT)/test_cfsresilver $(HOST_OUT)/test_cfsmember $(HOST_OUT)/test_cfscheck $(HOST_OUT)/test_cfssnap $(HOST_OUT)/test_cfsbuf $(HOST_OUT)/test_cfsalloc $(HOST_OUT)/test_cfscommit $(HOST_OUT)/test_cfsformat $(HOST_OUT)/test_cfscorrupt $(HOST_OUT)/test_cfshooks $(HOST_OUT)/test_cfscore $(HOST_OUT)/test_cfssnapctl $(HOST_OUT)/test_cfscsum $(HOST_OUT)/test_cfsdata $(HOST_OUT)/test_cfsfile $(HOST_OUT)/test_cfsvnode $(HOST_OUT)/test_cfsdirblk $(HOST_OUT)/test_cfsdir $(HOST_OUT)/test_usbdesc $(HOST_OUT)/test_acpi $(HOST_OUT)/test_sync

$(HOST_OUT)/test_fbvalid: $(call src_paths,$(HOST_FBVALID_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $^ $(HOST_LDFLAGS) -o $@

# The loader's display mode decisions: boot/uefi, which the kernel's
# include path does not reach.
$(HOST_OUT)/test_display: $(call src_paths,$(HOST_DISPLAY_SRCS)) $(ROOT)/boot/uefi/display_pick.h $(ROOT)/boot/uefi/efi.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I$(ROOT)/boot/uefi $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_jitter: $(call src_paths,$(HOST_JITTER_SRCS)) $(ROOT)/kernel/include/kernel/jitter.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -DJITTER_HOST_TEST=1 $(call src_paths,$(HOST_JITTER_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_pci_bar: $(call src_paths,$(HOST_PCI_BAR_SRCS)) $(ROOT)/kernel/include/kernel/pci_bar.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $(call src_paths,$(HOST_PCI_BAR_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_upgrade_be: $(call src_paths,$(HOST_UPBE_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $^ $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_gpt: $(call src_paths,$(HOST_GPT_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -DCMDLINE_HOST_TEST=1 $^ $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_part: $(call src_paths,$(HOST_PART_SRCS)) $(ROOT)/kernel/include/kernel/part.h $(ROOT)/kernel/include/kernel/blk.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_ramblk: $(call src_paths,$(HOST_RAMBLK_SRCS)) $(ROOT)/kernel/include/kernel/ramblk.h $(ROOT)/kernel/include/kernel/blk.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

# Built with fault injection on: the test is the hook (faultinject_should_fail) the layer's two injection points call.
$(HOST_OUT)/test_blk: $(call src_paths,$(HOST_BLK_SRCS)) $(ROOT)/kernel/include/kernel/blk.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -UCONFIG_FAULTINJECT -DCONFIG_FAULTINJECT=1 -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_pool: $(call src_paths,$(HOST_POOL_SRCS)) $(ROOT)/kernel/include/kernel/storage.h $(ROOT)/kernel/include/kernel/blk.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cputime: $(call src_paths,$(HOST_CPUTIME_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $^ $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_nice: $(call src_paths,$(HOST_NICE_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $^ $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_fair: $(call src_paths,$(HOST_FAIR_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $^ $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_buddy: $(call src_paths,$(HOST_BUDDY_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $^ $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_slab: $(call src_paths,$(HOST_SLAB_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $^ $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_crypto: $(call src_paths,$(HOST_CRYPTO_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $^ $(HOST_LDFLAGS) -o $@

# relsig.c reads the key ring with opendir: POSIX, which glibc hides under -std=c11.
$(HOST_OUT)/test_relsig: $(call src_paths,$(HOST_RELSIG_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $^ $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_modelf: $(call src_paths,$(HOST_MODELF_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -DMODELF_HOST_TEST=1 $^ $(HOST_LDFLAGS) -o $@

#
# This test is one translation unit that `#include`s the library sources it
# exercises, so its prerequisites are whatever that file pulls in -- and
# they were written out by hand here, once per source, until the list fell
# one behind the file. `libc/src/tlsscan.c` was the fourth include and the
# third name, so editing it rebuilt nothing: `gmake host-test` re-ran the
# *previous* binary and reported ok, which made three regression tests pass
# against a bug that had been reintroduced on purpose to fail them.
#
# So the compiler writes the list instead. `-MMD` records every file the
# translation unit actually read, headers included, and the `-include`
# below feeds them back as prerequisites on the next run. A list nobody
# maintains cannot fall behind.
#
-include $(HOST_OUT)/test_libc.d

$(HOST_OUT)/test_libc: $(call src_paths,$(HOST_LIBC_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) -std=c11 -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-Wall -Wextra -Werror -Wno-missing-prototypes -Wno-builtin-requires-header -Wno-incompatible-library-redeclaration \
		-MMD -MF $@.d -MT $@ \
		-DLIBC_HOST_TEST=1 $< $(HOST_LDFLAGS) -o $@

# PKG_BENCH times the decoders with clock_gettime: POSIX, which glibc hides under -std=c11.
$(HOST_OUT)/test_pkg: $(call src_paths,$(HOST_PKG_SRCS)) $(ROOT)/pkg/pkg.h $(ROOT)/pkg/pkgparse.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) -std=c11 -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-Wall -Wextra -Werror -Wno-missing-prototypes -D_DEFAULT_SOURCE $(CEE_CFLAGS) -I$(ROOT)/pkg \
		$(call src_paths,$(HOST_PKG_SRCS)) $(HOST_LDFLAGS) -o $@

# test_linux and test_netparse time their conversions with clock_gettime (LINUX_BENCH, NETPARSE_BENCH): POSIX, which
# glibc hides under -std=c11, as for test_relsig.
$(HOST_OUT)/test_linux: $(call src_paths,$(HOST_LINUX_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/compat/linux $(call src_paths,$(HOST_LINUX_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_fdt: $(call src_paths,$(HOST_FDT_SRCS)) $(ROOT)/tools/fdt/fdt.h $(ROOT)/kernel/include/uapi/cosmo/hv_machine.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I$(ROOT)/tools/fdt $(call src_paths,$(HOST_FDT_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_vblk_dev: $(call src_paths,$(HOST_VBLK_SRCS)) $(ROOT)/userland/system/vblk.h $(ROOT)/userland/system/vq.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $(call src_paths,$(HOST_VBLK_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_vnet_dev: $(call src_paths,$(HOST_VNET_SRCS)) $(ROOT)/userland/system/vnet.h $(ROOT)/userland/system/vq.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $(call src_paths,$(HOST_VNET_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_hv: $(call src_paths,$(HOST_HV_SRCS)) $(ROOT)/kernel/arch/x86_64/include/x86/svm.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I$(ROOT)/kernel/arch/x86_64/include $(call src_paths,$(HOST_HV_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_vmx: $(call src_paths,$(HOST_VMX_SRCS)) $(ROOT)/kernel/arch/x86_64/include/x86/vmx.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I$(ROOT)/kernel/arch/x86_64/include $(call src_paths,$(HOST_VMX_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_hv_s2: $(call src_paths,$(HOST_HV_S2_SRCS)) $(ROOT)/kernel/arch/aarch64/include/aarch64/hv_s2.h $(ROOT)/kernel/include/arch/hv_s2_core.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I$(ROOT)/kernel/arch/aarch64/include $(call src_paths,$(HOST_HV_S2_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_reloc_aarch64: $(call src_paths,$(HOST_RELOC_A64_SRCS)) $(ROOT)/kernel/arch/aarch64/include/aarch64/modreloc.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I$(ROOT)/kernel/arch/aarch64/include $(call src_paths,$(HOST_RELOC_A64_SRCS)) $(HOST_LDFLAGS) -o $@

# test_virtq, test_usbdesc and test_acpi time their decoders with clock_gettime (VIRTQ_BENCH, USBDESC_BENCH,
# ACPI_BENCH): POSIX, which glibc hides under -std=c11.
$(HOST_OUT)/test_virtq: $(call src_paths,$(HOST_VIRTQ_SRCS)) $(ROOT)/drivers/include/drivers/virtio.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/drivers/include $(call src_paths,$(HOST_VIRTQ_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_netparse: $(call src_paths,$(HOST_NETPARSE_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(call src_paths,$(HOST_NETPARSE_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_elf: $(call src_paths,$(HOST_ELF_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(call src_paths,$(HOST_ELF_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_acpi: $(call src_paths,$(HOST_ACPI_SRCS)) $(ROOT)/drivers/acpi/acpi_internal.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/drivers/acpi $(call src_paths,$(HOST_ACPI_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_usbdesc: $(call src_paths,$(HOST_USBDESC_SRCS)) $(ROOT)/drivers/include/drivers/usb.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/drivers/include $(call src_paths,$(HOST_USBDESC_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfsparse: $(call src_paths,$(HOST_CFSPARSE_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(call src_paths,$(HOST_CFSPARSE_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfsreach: $(call src_paths,$(HOST_CFSREACH_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfsclone: $(call src_paths,$(HOST_CFSCLONE_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfsrollback: $(call src_paths,$(HOST_CFSROLLBACK_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfsscrub: $(call src_paths,$(HOST_CFSSCRUB_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfscrypt: $(call src_paths,$(HOST_CFSCRYPT_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfsresilver: $(call src_paths,$(HOST_CFSRESILVER_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfsmember: $(call src_paths,$(HOST_CFSMEMBER_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfscheck: $(call src_paths,$(HOST_CFSCHECK_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfssnap: $(call src_paths,$(HOST_CFSSNAP_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfsbuf: $(call src_paths,$(HOST_CFSBUF_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfsalloc: $(call src_paths,$(HOST_CFSALLOC_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfscommit: $(call src_paths,$(HOST_CFSCOMMIT_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfsformat: $(call src_paths,$(HOST_CFSFORMAT_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfscorrupt: $(call src_paths,$(HOST_CFSCORRUPT_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfshooks: $(call src_paths,$(HOST_CFSHOOKS_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfsdir: $(call src_paths,$(HOST_CFSDIR_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfsdirblk: $(call src_paths,$(HOST_CFSDIRBLK_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfsvnode: $(call src_paths,$(HOST_CFSVNODE_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfsfile: $(call src_paths,$(HOST_CFSFILE_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfsdata: $(call src_paths,$(HOST_CFSDATA_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfscsum: $(call src_paths,$(HOST_CFSCSUM_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfssnapctl: $(call src_paths,$(HOST_CFSSNAPCTL_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cfscore: $(call src_paths,$(HOST_CFSCORE_SRCS)) $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_internal.h \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_handles: $(call src_paths,$(HOST_HANDLES_SRCS)) $(ROOT)/kernel/include/kernel/object.h $(ROOT)/kernel/include/kernel/handle.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $(call src_paths,$(HOST_HANDLES_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_cred: $(call src_paths,$(HOST_CRED_SRCS)) $(ROOT)/kernel/include/kernel/cred.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $(call src_paths,$(HOST_CRED_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_sync: $(call src_paths,$(HOST_SYNC_SRCS)) $(ROOT)/tests/host/sync_sched.h $(ROOT)/kernel/include/kernel/wait.h $(ROOT)/kernel/include/kernel/mutex.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -pthread $(call src_paths,$(HOST_SYNC_SRCS)) $(HOST_LDFLAGS) -pthread -o $@

$(HOST_OUT)/test_quiesce: $(call src_paths,$(HOST_QUIESCE_SRCS)) $(ROOT)/kernel/include/kernel/quiesce_core.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -pthread $(call src_paths,$(HOST_QUIESCE_SRCS)) $(HOST_LDFLAGS) -pthread -o $@

$(HOST_OUT)/test_lockdep: $(call src_paths,$(HOST_LOCKDEP_SRCS)) $(ROOT)/kernel/include/kernel/lockdep_core.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $(call src_paths,$(HOST_LOCKDEP_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_lockup: $(call src_paths,$(HOST_LOCKUP_SRCS)) $(ROOT)/kernel/include/kernel/lockup_core.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $(call src_paths,$(HOST_LOCKUP_SRCS)) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_chacha20: $(call src_paths,$(HOST_CHACHA_SRCS)) $(ROOT)/kernel/include/kernel/chacha20.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_keyslot: $(call src_paths,$(HOST_KEYSLOT_SRCS)) $(ROOT)/userland/lib/keyslot.h \
	$(ROOT)/userland/lib/argon2.h $(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h \
	$(ROOT)/userland/lib/secret.c $(ROOT)/userland/lib/secret.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) $(filter-out %/secret.c,$(filter %.c,$^)) $(HOST_LDFLAGS) -o $@   # the test includes secret.c

$(HOST_OUT)/test_loopback: $(call src_paths,$(HOST_LOOPBACK_SRCS)) $(ROOT)/kernel/include/kernel/netif.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_arp: $(call src_paths,$(HOST_ARP_SRCS)) $(ROOT)/kernel/include/kernel/net/ether.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_nd: $(call src_paths,$(HOST_ND_SRCS)) $(ROOT)/kernel/include/kernel/net/ip.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_diskid: $(call src_paths,$(HOST_DISKID_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_ipv6: $(call src_paths,$(HOST_IPV6_SRCS)) $(ROOT)/kernel/include/kernel/net/ip.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_ipv4: $(call src_paths,$(HOST_IPV4_SRCS)) $(ROOT)/kernel/include/kernel/net/ip.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_udp: $(call src_paths,$(HOST_UDP_SRCS)) $(ROOT)/kernel/include/kernel/net/udp.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_raw: $(call src_paths,$(HOST_RAW_SRCS)) $(ROOT)/kernel/include/kernel/net/raw.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_fw: $(call src_paths,$(HOST_FW_SRCS)) $(ROOT)/kernel/include/kernel/net/fw.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_nat: $(call src_paths,$(HOST_NAT_SRCS)) $(ROOT)/kernel/include/kernel/net/nat.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_socket: $(call src_paths,$(HOST_SOCKET_SRCS)) $(ROOT)/kernel/include/kernel/socket.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_tcp: $(call src_paths,$(HOST_TCP_SRCS)) $(ROOT)/kernel/include/kernel/net/tcp.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I$(ROOT)/tests/fuzz -D_DEFAULT_SOURCE -DTCP_HOST_TEST=1 $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_dhcp: $(call src_paths,$(HOST_DHCP_SRCS)) $(ROOT)/kernel/include/kernel/net/dhcp.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I$(ROOT)/tests/fuzz -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_tapsvc: $(call src_paths,$(HOST_TAPSVC_SRCS)) $(ROOT)/kernel/include/kernel/net/tapsvc.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I$(ROOT)/tests/fuzz -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_tap: $(call src_paths,$(HOST_TAP_SRCS)) $(ROOT)/kernel/include/kernel/net/tap.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_procfs: $(call src_paths,$(HOST_PROCFS_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -D'PROCFS_COMPILER="host cc"' $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_memdev: $(call src_paths,$(HOST_MEMDEV_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_mountns: $(call src_paths,$(HOST_MOUNTNS_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I$(ROOT)/kernel-services/vfs -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_blkctl: $(call src_paths,$(HOST_BLKCTL_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

# Fault injection on, so the injected readpage failure is the test's to make.
$(HOST_OUT)/test_pagecache: $(call src_paths,$(HOST_PAGECACHE_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -UCONFIG_FAULTINJECT -DCONFIG_FAULTINJECT=1 -D_DEFAULT_SOURCE $(filter %.c,$^) \
		$(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_ramfs: $(call src_paths,$(HOST_RAMFS_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_fsctl: $(call src_paths,$(HOST_FSCTL_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I$(ROOT)/kernel-services/vfs -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_vfs: $(call src_paths,$(HOST_VFS_SRCS)) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I$(ROOT)/kernel-services/vfs -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_procnet: $(call src_paths,$(HOST_PROCNET_SRCS)) $(ROOT)/kernel/include/kernel/net/procnet.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_netif: $(call src_paths,$(HOST_NETIF_SRCS)) $(ROOT)/kernel/include/kernel/netif.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE -I$(ROOT)/kernel-services/network $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

# MBUF_BENCH times the packet buffers with clock_gettime: POSIX, which glibc hides under -std=c11.
$(HOST_OUT)/test_mbuf: $(call src_paths,$(HOST_MBUF_SRCS)) $(ROOT)/kernel/include/kernel/mbuf.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

# ETHER_BENCH times the framing with clock_gettime: POSIX, which glibc hides under -std=c11.
$(HOST_OUT)/test_ether: $(call src_paths,$(HOST_ETHER_SRCS)) $(ROOT)/kernel/include/kernel/net/ether.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

# INET_BENCH times the helpers with clock_gettime: POSIX, which glibc hides under -std=c11.
$(HOST_OUT)/test_inet: $(call src_paths,$(HOST_INET_SRCS)) $(ROOT)/kernel/include/kernel/net/inet.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

# CKSUM_BENCH times the checksum with clock_gettime: POSIX, which glibc hides under -std=c11.
$(HOST_OUT)/test_cksum: $(call src_paths,$(HOST_CKSUM_SRCS)) $(ROOT)/kernel/include/kernel/net/cksum.h \
		$(ROOT)/kernel/include/kernel/mbuf.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

# LZ4_BENCH times the decoder with clock_gettime: POSIX, which glibc hides under -std=c11.
$(HOST_OUT)/test_lz4: $(call src_paths,$(HOST_LZ4_SRCS)) $(ROOT)/kernel/include/kernel/lz4.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -D_DEFAULT_SOURCE $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

# The format header is the thing under test here: without it as a
# prerequisite a stale binary passes an assertion the header no longer
# satisfies, which is exactly what happened when the version last moved.
$(HOST_OUT)/test_cosmofs: $(call src_paths,$(HOST_COSMOFS_SRCS)) \
	$(ROOT)/kernel-services/filesystem/cosmofs/cosmofs_format.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -I$(ROOT)/kernel-services/filesystem/cosmofs $(filter %.c,$^) $(HOST_LDFLAGS) -o $@

.PHONY: host-test
host-test: $(HOST_TESTS)
	$(Q)for t in $(HOST_TESTS); do echo "== $$t"; ASAN_OPTIONS=detect_leaks=0 $$t || exit 1; done
	@echo "== $(ROOT)/tests/host/test_fetch_pinned.py"
	$(Q)$(PYTHON) $(ROOT)/tests/host/test_fetch_pinned.py
	@echo "== $(ROOT)/tests/host/test_analysis_gate.py"
	$(Q)$(PYTHON) $(ROOT)/tests/host/test_analysis_gate.py
	@echo "== $(ROOT)/tools/cosmofs_key.py --self-test"
	$(Q)$(PYTHON) $(ROOT)/tools/cosmofs_key.py --self-test
	@echo "== $(ROOT)/tests/host/test_run_guide.py"
	$(Q)$(PYTHON) $(ROOT)/tests/host/test_run_guide.py
	@echo "== $(ROOT)/tests/host/test_release_version.py"
	$(Q)$(PYTHON) $(ROOT)/tests/host/test_release_version.py
	@echo "== $(ROOT)/tests/host/test_boot_harness.py"
	$(Q)$(PYTHON) $(ROOT)/tests/host/test_boot_harness.py
	@echo "== $(ROOT)/tests/host/test_publish_repo.py"
	$(Q)$(PYTHON) $(ROOT)/tests/host/test_publish_repo.py
	@echo "== $(ROOT)/tests/host/test_pkgbuild_libs.py"
	$(Q)CC="$(CC)" $(PYTHON) $(ROOT)/tests/host/test_pkgbuild_libs.py
	@echo "== $(ROOT)/tests/boot/test_firmware_stall.py"
	$(Q)$(PYTHON) $(ROOT)/tests/boot/test_firmware_stall.py
	@echo "== $(ROOT)/tests/boot/test_boot_deadline.py"
	$(Q)$(PYTHON) $(ROOT)/tests/boot/test_boot_deadline.py
	@echo "== $(ROOT)/tests/boot/test_stall_diag.py"
	$(Q)$(PYTHON) $(ROOT)/tests/boot/test_stall_diag.py
	@echo "== $(ROOT)/tests/boot/test_nettest_deadline.py"
	$(Q)$(PYTHON) $(ROOT)/tests/boot/test_nettest_deadline.py
	@echo "== $(ROOT)/tests/boot/test_usertest_sections.py"
	$(Q)$(PYTHON) $(ROOT)/tests/boot/test_usertest_sections.py
	@echo "== $(ROOT)/tests/boot/test_selftest_budgets.py"
	$(Q)$(PYTHON) $(ROOT)/tests/boot/test_selftest_budgets.py
	@echo "== $(ROOT)/tests/boot/test_module_symbols.py"
	$(Q)COSMO_CC=$(CC) COSMO_SYMBOLIZER=$(LLVM_PREFIX)llvm-symbolizer $(PYTHON) $(ROOT)/tests/boot/test_module_symbols.py

# cosmo-fetch (ports/mbedtls/README.md) built for the host on the same
# mbedTLS release and configuration, with ASan and UBSan, and run against
# local TLS servers (tests/host/test_fetch.py): the certificate, pin,
# redirect, plain-HTTP, range and size cases. Its own target: it needs the
# pinned mbedTLS archive and OpenSSL 3.4 or later for the test certificates.
HOST_MBEDTLS := $(HOST_OUT)/mbedtls
HOST_MBEDTLS_LIBS := $(HOST_MBEDTLS)/library/libmbedtls.a $(HOST_MBEDTLS)/library/libmbedx509.a \
	$(HOST_MBEDTLS)/library/libmbedcrypto.a

$(HOST_MBEDTLS_LIBS) &: $(MBEDTLS_TAR) $(ROOT)/ports/mbedtls/cosmo_config.h
	$(call log,HOSTMBEDTLS,$(MBEDTLS_VERSION))
	$(Q)rm -rf $(HOST_MBEDTLS)
	$(Q)mkdir -p $(HOST_MBEDTLS)
	$(Q)tar -xjf $(MBEDTLS_TAR) -C $(HOST_MBEDTLS) --strip-components=1
	$(Q)$(MAKE) -s -C $(HOST_MBEDTLS)/library CC=$(HOST_CC) \
		CFLAGS="-O1 -g -I$(ROOT)/ports/mbedtls -DMBEDTLS_USER_CONFIG_FILE='<cosmo_config.h>'" \
		libmbedcrypto.a libmbedx509.a libmbedtls.a > $(HOST_MBEDTLS)/build.log 2>&1 \
		|| { tail -40 $(HOST_MBEDTLS)/build.log; exit 1; }

$(HOST_OUT)/cosmo-fetch: $(ROOT)/ports/mbedtls/cosmo-fetch.c $(TLS_COMMON) $(HOST_MBEDTLS_LIBS)
	$(call log,HOSTCC,$@)
	$(Q)$(HOST_CC) -std=gnu11 -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		-Wall -Wextra -Werror -I$(ROOT)/ports/mbedtls -I$(HOST_MBEDTLS)/include \
		-DMBEDTLS_USER_CONFIG_FILE='<cosmo_config.h>' $< $(ROOT)/ports/mbedtls/tls_common.c $(HOST_MBEDTLS_LIBS) \
		$(HOST_LDFLAGS) -o $@

.PHONY: host-test-fetch
host-test-fetch: $(HOST_OUT)/cosmo-fetch
	$(Q)ASAN_OPTIONS=detect_leaks=0 $(PYTHON) $(ROOT)/tests/host/test_fetch.py --fetch $(HOST_OUT)/cosmo-fetch \
		--workdir $(HOST_OUT)/fetch-test

# Keep TSan in a distinct binary: it cannot be combined with ASan.
$(HOST_OUT)/test_lockdep_threads: $(ROOT)/tests/host/test_lockdep_threads.c $(ROOT)/kernel/include/kernel/lockdep_core.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -pthread $< $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_lockdep_threads_tsan: $(ROOT)/tests/host/test_lockdep_threads.c $(ROOT)/kernel/include/kernel/lockdep_core.h | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(filter-out $(HOST_LDFLAGS) -fno-sanitize-recover=undefined,$(HOST_CFLAGS)) -fsanitize=thread -pthread $< -o $@

.PHONY: host-test-lockdep-tsan
host-test-lockdep-tsan: $(HOST_OUT)/test_lockdep_threads_tsan
	$(Q)TSAN_OPTIONS=halt_on_error=1 $<

# The epoch core under TSan (docs/kernel/quiesce/testing.md, "Memory
# ordering"). TSan checks that every reader access happens-before the free
# through the protocol's own atomics, so it reports a missing pairing even
# on a run where the hardware reordered nothing. The two negative controls
# build the same test with one pairing weakened to relaxed -- the
# publisher's release (Q2) or the waiter's acquire (W2) -- and must be
# reported: a TSan model that passes them both would prove nothing.
QUIESCE_TSAN_CFLAGS = $(filter-out $(HOST_LDFLAGS) -fno-sanitize-recover=undefined,$(HOST_CFLAGS)) -fsanitize=thread -pthread
QUIESCE_TSAN_DEPS = $(call src_paths,$(HOST_QUIESCE_SRCS)) $(ROOT)/kernel/include/kernel/quiesce_core.h

$(HOST_OUT)/test_quiesce_tsan: $(QUIESCE_TSAN_DEPS) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(QUIESCE_TSAN_CFLAGS) $(call src_paths,$(HOST_QUIESCE_SRCS)) -o $@

$(HOST_OUT)/test_quiesce_tsan_neg_q2: $(QUIESCE_TSAN_DEPS) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(QUIESCE_TSAN_CFLAGS) -DQUIESCE_MO_Q2=__ATOMIC_RELAXED $(call src_paths,$(HOST_QUIESCE_SRCS)) -o $@

$(HOST_OUT)/test_quiesce_tsan_neg_w2: $(QUIESCE_TSAN_DEPS) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(QUIESCE_TSAN_CFLAGS) -DQUIESCE_MO_W2=__ATOMIC_RELAXED $(call src_paths,$(HOST_QUIESCE_SRCS)) -o $@

.PHONY: host-test-quiesce-tsan
host-test-quiesce-tsan: $(HOST_OUT)/test_quiesce_tsan $(HOST_OUT)/test_quiesce_tsan_neg_q2 $(HOST_OUT)/test_quiesce_tsan_neg_w2
	$(Q)TSAN_OPTIONS=halt_on_error=1 $(HOST_OUT)/test_quiesce_tsan
	$(Q)for n in q2 w2; do \
		echo "== negative control: $$n relaxed (TSan must report a data race)"; \
		if TSAN_OPTIONS=halt_on_error=1 $(HOST_OUT)/test_quiesce_tsan_neg_$$n > $(HOST_OUT)/test_quiesce_tsan_neg_$$n.log 2>&1; then \
			echo "FAIL: the $$n-weakened model ran clean"; exit 1; \
		fi; \
		if ! grep -q "WARNING: ThreadSanitizer: data race" $(HOST_OUT)/test_quiesce_tsan_neg_$$n.log; then \
			echo "FAIL: the $$n-weakened model failed without a data-race report:"; \
			tail -20 $(HOST_OUT)/test_quiesce_tsan_neg_$$n.log; exit 1; \
		fi; \
		echo "ok: data race reported ($(HOST_OUT)/test_quiesce_tsan_neg_$$n.log)"; \
	done

HOST_INTERRUPT_SRCS := $(ROOT)/tests/host/test_interrupt.c $(ROOT)/kernel/interrupt/interrupt.c
$(HOST_OUT)/test_interrupt: $(HOST_INTERRUPT_SRCS) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(HOST_CFLAGS) -pthread $^ $(HOST_LDFLAGS) -o $@

$(HOST_OUT)/test_interrupt_tsan: $(HOST_INTERRUPT_SRCS) | $(CEE_STAMP)
	$(call log,HOSTCC,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(HOST_CC) $(filter-out $(HOST_LDFLAGS) -fno-sanitize-recover=undefined,$(HOST_CFLAGS)) -fsanitize=thread -pthread $^ -o $@

.PHONY: host-test-interrupt-tsan
host-test-interrupt-tsan: $(HOST_OUT)/test_interrupt_tsan
	$(Q)TSAN_OPTIONS=halt_on_error=1 $<

# These rules' prerequisites run on past one line, so the generation is added here.
$(HOST_OUT)/test_keyslot: | $(CEE_STAMP)
$(HOST_OUT)/test_cosmofs: | $(CEE_STAMP)
