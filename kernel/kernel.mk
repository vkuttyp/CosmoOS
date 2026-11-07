# Kernel component build.
#
# Generic sources get only the generic include path. Architecture sources
# additionally get their private include directory. That asymmetry is what
# enforces Invariant 1 at build time: generic code cannot include an
# architecture header because it is not on its search path.

KERNEL_ELF := $(OUT)/kernel/kernel.elf
KERNEL_MAP := $(OUT)/kernel/kernel.map

include $(ROOT)/kernel/arch/$(ARCH)/arch.mk

KERNEL_GENERIC_SRCS := \
	kernel/core/main.c \
	kernel/core/bootinfo.c \
	kernel/core/bootarchive.c \
	kernel/core/console.c \
	kernel/core/fbvalid.c \
	kernel/core/fbcon.c \
	kernel/core/fbtest.c \
	kernel/core/font8x16.c \
	kernel/core/utsns.c \
	kernel/core/log.c \
	kernel/core/panic.c \
	kernel/core/printf.c \
	kernel/core/string.c \
	kernel/core/shutdown.c \
	kernel/core/selftest.c \
	kernel/core/irqpoll.c \
	kernel/core/irqpolltest.c \
	kernel/core/spinlock.c \
	kernel/core/percpu.c \
	kernel/core/smp.c \
	kernel/core/quiesce.c \
	kernel/core/quiesce_floor.c \
	kernel/core/quiescetest.c \
	kernel/core/lockdep.c \
	kernel/core/lockdeptest.c \
	kernel/core/lockup.c \
	kernel/core/lockuptest.c \
	kernel/core/faultinject.c \
	kernel/core/extable.c \
	kernel/core/faulttest.c \
	kernel/interrupt/interrupt.c \
	kernel/interrupt/irq.c \
	kernel/interrupt/ipi.c \
	kernel/interrupt/irqtest.c \
	kernel/memory/bootmem.c \
	kernel/memory/asid.c \
	kernel/memory/buddy.c \
	kernel/memory/pmm.c \
	kernel/memory/vmm.c \
	kernel/memory/slab.c \
	kernel/memory/kmalloc.c \
	kernel/memory/memtest.c \
	kernel/memory/forktest.c \
	kernel/timer/clocktest.c \
	kernel/timer/rtcdev.c \
	kernel/timer/timer.c \
	kernel/scheduler/thread.c \
	kernel/scheduler/sched.c \
	kernel/scheduler/policy_rr.c \
	kernel/scheduler/wait.c \
	kernel/scheduler/wait_floor.c \
	kernel/scheduler/mutex.c \
	kernel/scheduler/mutex_floor.c \
	kernel/scheduler/semaphore.c \
	kernel/scheduler/completion.c \
	kernel/scheduler/completion_floor.c \
	kernel/scheduler/schedtest.c \
	kernel/scheduler/smptest.c \
	kernel/object/object.c \
	kernel/object/object_floor.c \
	kernel/object/handle.c \
	kernel/object/handle_floor.c \
	kernel/object/console_obj.c \
	kernel/device/hidtest.c \
	kernel/tty/tty.c \
	kernel/tty/ttydev.c \
	kernel/tty/pty.c \
	kernel/tty/ttytest.c \
	kernel/ipc/pipe.c \
	kernel/ipc/pipe_floor.c \
	kernel/ipc/pipetest.c \
	kernel/ipc/fifo.c \
	kernel/ipc/fifo_floor.c \
	kernel/ipc/fifotest.c \
	kernel/ipc/unix.c \
	kernel/ipc/unixtest.c \
	kernel/ipc/futex.c \
	kernel/ipc/shm.c \
	kernel/io/aio.c \
	kernel/io/timerobj.c \
	kernel/io/eventfd.c \
	kernel/io/signalfd.c \
	kernel/io/epoll.c \
	kernel/io/poll.c \
	kernel/io/polltest.c \
	kernel/io/epolltest.c \
	compat/linux/convert.c \
	compat/linux/lxconv.c \
	compat/linux/syscalls.c \
	compat/linux/signal.c \
	compat/linux/netioctl.c \
	kernel-services/virtualization/vmm.c \
	kernel-services/virtualization/vm.c \
	kernel-services/virtualization/guestmem.c \
	kernel-services/virtualization/vmdev.c \
	kernel-services/virtualization/vuart.c \
	kernel-services/virtualization/vintr.c \
	kernel-services/virtualization/vcpu.c \
	kernel-services/virtualization/hvsys.c \
	kernel-services/virtualization/hvtest.c \
	kernel/process/elf.c \
	kernel/process/elfparse.c \
	kernel/process/cred.c \
	kernel/process/process.c \
	kernel/process/itimer.c \
	kernel/process/cpu_account.c \
	kernel/process/cputime.c \
	kernel/process/nice.c \
	kernel/scheduler/fair.c \
	kernel/process/spawn.c \
	kernel/process/signal.c \
	kernel/process/native_signal.c \
	kernel/process/proctest.c \
	kernel/syscall/syscall.c \
	kernel/syscall/native.c \
	kernel/syscall/personality.c \
	kernel/syscall/uaccess.c \
	kernel/syscall/uaccesstest.c \
	kernel/security/sha512.c \
	kernel/security/ed25519.c \
	kernel/security/keyring.c \
	kernel/module/ksym.c \
	kernel/module/modsig.c \
	kernel/module/modelf.c \
	kernel/module/module.c \
	kernel/module/modtest.c \
	kernel/module/modcycle.c \
	kernel/module/modspec.c \
	kernel/device/device.c \
	kernel/device/dma.c \
	kernel/device/devtest.c \
	kernel/block/blk.c \
	kernel/block/blk_floor.c \
	kernel/block/blkserial.c \
	kernel/block/gpt.c \
	kernel/ceesafe/ceesafe_rt_core.c \
	kernel/ceesafe/ceesafe_panic.c \
	kernel/ceesafe/crash_test.c \
	kernel/ceesafe/kcore_test.c \
	kernel/ceesafe/kcore_selftest.c \
	kernel/ceesafe/ceesafe_rt_async.c \
	kernel/ceesafe/async_test.c \
	kernel/ceesafe/async_selftest.c \
	kernel/block/part.c \
	kernel/block/ramblk.c \
	kernel/block/blktest.c \
	kernel/block/parttest.c \
	kernel/core/jitter.c \
	kernel/core/random.c \
	kernel/core/crc32c.c \
	kernel/core/lz4.c \
	kernel/core/lz4_decode.c \
	kernel/security/chacha20.c \
	kernel/security/blake2b.c \
	kernel/core/fwcfg.c \
	kernel/core/cmdline.c \
	kernel/core/bootfs.c \
	kernel-services/vfs/vfs.c \
	kernel-services/vfs/fsctl.c \
	kernel-services/vfs/blkctl.c \
	kernel-services/vfs/mountns.c \
	kernel-services/filesystem/procfs/procfs.c \
	kernel-services/vfs/pagecache.c \
	kernel-services/vfs/ramfs.c \
	kernel-services/vfs/memdev.c \
	kernel-services/vfs/vfstest.c \
	kernel-services/storage/pool.c \
	kernel-services/storage/pool_floor.c \
	kernel-services/filesystem/cosmofs/cosmofs_core.c \
	kernel-services/filesystem/cosmofs/cosmofs_core_floor.c \
	kernel-services/filesystem/cosmofs/cosmofs_buf.c \
	kernel-services/filesystem/cosmofs/cosmofs_alloc.c \
	kernel-services/filesystem/cosmofs/cosmofs_commit.c \
	kernel-services/filesystem/cosmofs/cosmofs_commit_floor.c \
	kernel-services/filesystem/cosmofs/cosmofs_format.c \
	kernel-services/filesystem/cosmofs/cosmofs_corrupt.c \
	kernel-services/filesystem/cosmofs/cosmofs_hooks.c \
	kernel-services/filesystem/cosmofs/cosmofs_hooks_floor.c \
	kernel-services/filesystem/cosmofs/cosmofs_buf_floor.c \
	kernel-services/filesystem/cosmofs/cosmofs_snap.c \
	kernel-services/filesystem/cosmofs/cosmofs_snapctl.c \
	kernel-services/filesystem/cosmofs/cosmofs_csum.c \
	kernel-services/filesystem/cosmofs/cosmofs_csum_floor.c \
	kernel-services/filesystem/cosmofs/cosmofs_data.c \
	kernel-services/filesystem/cosmofs/cosmofs_data_floor.c \
	kernel-services/filesystem/cosmofs/cosmofs_file.c \
	kernel-services/filesystem/cosmofs/cosmofs_file_floor.c \
	kernel-services/filesystem/cosmofs/cosmofs_vnode.c \
	kernel-services/filesystem/cosmofs/cosmofs_vnode_floor.c \
	kernel-services/filesystem/cosmofs/cosmofs_dir.c \
	kernel-services/filesystem/cosmofs/cosmofs_dir_floor.c \
	kernel-services/filesystem/cosmofs/cosmofs_snap_floor.c \
	kernel-services/filesystem/cosmofs/cosmofs_reach.c \
	kernel-services/filesystem/cosmofs/cosmofs_rollback.c \
	kernel-services/filesystem/cosmofs/cosmofs_rollback_floor.c \
	kernel-services/filesystem/cosmofs/cosmofs_clone.c \
	kernel-services/filesystem/cosmofs/cosmofs_member.c \
	kernel-services/filesystem/cosmofs/cosmofs_member_floor.c \
	kernel-services/filesystem/cosmofs/cfsparse.c \
	kernel-services/filesystem/cosmofs/cosmofs_scrub.c \
	kernel-services/filesystem/cosmofs/cosmofs_scrub_floor.c \
	kernel-services/filesystem/cosmofs/cosmofs_resilver.c \
	kernel-services/filesystem/cosmofs/cosmofs_resilver_floor.c \
	kernel-services/filesystem/cosmofs/cosmofs_check.c \
	kernel-services/filesystem/cosmofs/cosmofs_check_floor.c \
	kernel-services/filesystem/cosmofs/cosmofs_crypt.c \
	kernel-services/filesystem/cosmofs/cosmofs_crypt_floor.c \
	kernel-services/filesystem/cosmofs/cosmofs.c \
	kernel-services/filesystem/cosmofs/cosmofs_floor.c \
	kernel-services/filesystem/cosmofs/cosmofstest.c \
	kernel-services/filesystem/cosmofs/cosmofs_keytest.c \
	kernel-services/filesystem/cosmofs/cosmofscrash.c \
	kernel-services/network/mbuf.c \
	kernel-services/network/cksum.c \
	kernel-services/network/cksum_floor.c \
	kernel-services/network/mbuf_floor.c \
	kernel-services/network/inet.c \
	kernel-services/network/netif.c \
	kernel-services/network/netif_floor.c \
	kernel-services/network/loopback.c \
	kernel-services/network/tap.c \
	kernel-services/network/tapsvc.c \
	kernel-services/network/dhcp.c \
	kernel-services/network/netparse.c \
	kernel-services/network/ether.c \
	kernel-services/network/arp.c \
	kernel-services/network/nd.c \
	kernel-services/network/procnet.c \
	kernel-services/network/procnet_floor.c \
	kernel-services/network/ipv4.c \
	kernel-services/network/nat.c \
	kernel-services/network/fw.c \
	kernel-services/network/ipv6.c \
	kernel-services/network/udp.c \
	kernel-services/network/raw.c \
	kernel-services/network/tcp.c \
	kernel-services/network/tcp_floor.c \
	kernel-services/network/socket.c \
	kernel-services/network/socket_floor.c \
	kernel-services/network/nettest_floor.c \
	kernel-services/network/nettest_base.c \
	kernel-services/network/nettest_lo.c \
	kernel-services/network/nettest_sockerr.c \
	kernel-services/network/nettest_tcp.c \
	kernel-services/network/nettest_nif.c \
	kernel-services/network/nettest_tap.c \
	kernel-services/network/nettest_fwd.c \
	kernel-services/network/nettest_ctl.c \
	kernel-services/network/nettest_input.c \
	kernel-services/network/nettest_hostin.c \
	kernel-services/network/nettest_hoststate.c \
	kernel-services/network/nettest_flows.c \
	kernel-services/network/nettest_output.c \
	kernel-services/network/nettest_tapdev.c \
	kernel-services/network/nettest_hooks.c \
	kernel-services/network/nettest_lifetime.c \
	kernel-services/network/nettest_neigh.c \
	kernel-services/network/nettest_worker.c \
	kernel-services/network/nettest_accept.c \
	kernel-services/network/nettest_seam.c \
	kernel-services/network/nettest_nat.c \
	kernel-services/network/nettest_nicbench.c \
	kernel-services/network/nettest_bench.c \
	kernel-services/network/nettest_harness.c \
	kernel-services/network/nettest_frame.c \
	drivers/acpi/acpi.c \
	drivers/acpi/acpi_parse.c \
	drivers/pci/pci.c \
	kernel/iommu/iommu.c \
	kernel/iommu/pt.c \
	kernel/iommu/iommutest.c \
	drivers/iommu/intel_vtd.c \
	drivers/iommu/arm_smmuv3.c

# The trusted key ring is generated from $(KEYRING_PUBS) (build/config.mk:
# the developer key's public half plus tools/keys/*.pub, or the release
# keys; see scripts/gen-keyring.py and docs/kernel/module/design.md).
KEYRING_SRC  := $(OUT)/gen/keyring_builtin.c
KEYRING_OBJ  := $(OUT)/gen/keyring_builtin.o

.PHONY: signing-stamp-force
signing-stamp-force:
$(SIGNING_STAMP): signing-stamp-force
	$(Q)mkdir -p $(dir $@)
	$(Q)printf '%s\n' 'MODSIGN_KEY=$(abspath $(MODSIGN_KEY))' 'KEYRING_PUBS=$(foreach k,$(KEYRING_PUBS),$(abspath $(k)))' \
		"REVOKED=$$(cksum < $(ROOT)/tools/keys/REVOKED)" > $@.tmp
	$(Q)if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(KEYRING_SRC): $(KEYRING_PUBS) $(SIGNING_STAMP) $(ROOT)/tools/keys/REVOKED $(ROOT)/scripts/gen-keyring.py
	$(call log,GEN,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(PYTHON) $(ROOT)/scripts/gen-keyring.py $@ $(KEYRING_PUBS)

# The generated source lives under $(OUT); map that prefix too so the
# debug info does not depend on the output directory (make reproducible).
$(KEYRING_OBJ): $(KEYRING_SRC)
	$(call log,CC,$<)
	$(Q)$(CC) $(KERNEL_CFLAGS) -ffile-prefix-map=$(OUT)=/cosmo/out -c $< -o $@

KERNEL_GENERIC_OBJS := $(call objs_of,$(KERNEL_GENERIC_SRCS)) $(KEYRING_OBJ)
KERNEL_ARCH_OBJS    := $(call objs_of,$(KERNEL_ARCH_SRCS))
KERNEL_OBJS         := $(KERNEL_GENERIC_OBJS) $(KERNEL_ARCH_OBJS)

KERNEL_ARCH_C_OBJS := $(call objs_of,$(filter %.c,$(KERNEL_ARCH_SRCS)))
KERNEL_ARCH_S_OBJS := $(call objs_of,$(filter %.S,$(KERNEL_ARCH_SRCS)))

$(eval $(call compile_rules,$(call objs_of,$(KERNEL_GENERIC_SRCS)),KERNEL_CFLAGS))
$(eval $(call compile_rules,$(KERNEL_ARCH_C_OBJS),KERNEL_CFLAGS))
$(eval $(call assemble_rules,$(KERNEL_ARCH_S_OBJS),KERNEL_CFLAGS))

$(KERNEL_ARCH_OBJS) $(patsubst %.o,%.analyzed,$(KERNEL_ARCH_C_OBJS)): \
	EXTRA_CFLAGS := -I$(ROOT)/kernel/arch/$(ARCH)/include

KERNEL_ANALYZE := $(patsubst %.o,%.analyzed,$(call objs_of,$(KERNEL_GENERIC_SRCS)) $(KERNEL_ARCH_C_OBJS))

$(KERNEL_ELF): $(KERNEL_OBJS) $(KERNEL_LINKER_SCRIPT)
	$(call log,LD,$@)
	$(Q)$(LD) $(KERNEL_LDFLAGS) -T $(KERNEL_LINKER_SCRIPT) -Map=$(KERNEL_MAP) -o $@ $(KERNEL_OBJS)
	$(Q)$(ROOT)/scripts/check-kernel-elf.sh $(OBJDUMP) $@
	$(Q)$(PYTHON) $(ROOT)/scripts/check-frame-baseline.py $(ROOT)

kernel: $(KERNEL_ELF)

# The kernel the image, the manifest and the release set carry. A release
# build's is kernel.elf without its debug info: the ESP is 64 MiB on every
# installed system (v1.0.3 on), and an upgrade holds two sets there at once
# (\cosmo and the new one), which the DWARF of the CeeSafe ports' generated
# C (most of kernel.elf's 12 MB) no longer let fit. Only debug sections go,
# so its loaded image, symbols and addresses are kernel.elf's: crash logs
# symbolize against kernel.elf as before. A debug build ships kernel.elf.
KERNEL_SHIP_ELF := $(if $(filter release,$(BUILD)),$(OUT)/kernel/kernel.ship.elf,$(KERNEL_ELF))

$(OUT)/kernel/kernel.ship.elf: $(KERNEL_ELF)
	$(call log,STRIP,$@)
	$(Q)$(OBJCOPY) --strip-debug $< $@

-include $(KERNEL_OBJS:.o=.d)

# compat/linux/lxconv.c is generated from lxconv.cee into out/cee (scripts/cee-gen.py), away from the convert.h it
# includes by name: its object, and its analysis, look in compat/linux too.
$(OUT)/compat/linux/lxconv.o $(OUT)/compat/linux/lxconv.analyzed: EXTRA_CFLAGS += -I$(ROOT)/compat/linux

# drivers/acpi/acpi_parse.c likewise, from acpi_parse.cee, away from the acpi_internal.h it includes.
$(OUT)/drivers/acpi/acpi_parse.o $(OUT)/drivers/acpi/acpi_parse.analyzed: EXTRA_CFLAGS += -I$(ROOT)/drivers/acpi
# kernel-services/network/netif.c likewise, from netif.cee, away from the netif_cpu.h it includes.
$(OUT)/kernel-services/network/netif.o $(OUT)/kernel-services/network/netif.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
# kernel-services/network/nettest_lo.c likewise, from nettest_lo.cee, away from the nettest.h it includes.
$(OUT)/kernel-services/network/nettest_lo.o $(OUT)/kernel-services/network/nettest_lo.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_sockerr.o $(OUT)/kernel-services/network/nettest_sockerr.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_tcp.o $(OUT)/kernel-services/network/nettest_tcp.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_nif.o $(OUT)/kernel-services/network/nettest_nif.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_tap.o $(OUT)/kernel-services/network/nettest_tap.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_fwd.o $(OUT)/kernel-services/network/nettest_fwd.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_ctl.o $(OUT)/kernel-services/network/nettest_ctl.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_input.o $(OUT)/kernel-services/network/nettest_input.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_hostin.o $(OUT)/kernel-services/network/nettest_hostin.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_hoststate.o $(OUT)/kernel-services/network/nettest_hoststate.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_flows.o $(OUT)/kernel-services/network/nettest_flows.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_output.o $(OUT)/kernel-services/network/nettest_output.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_tapdev.o $(OUT)/kernel-services/network/nettest_tapdev.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_hooks.o $(OUT)/kernel-services/network/nettest_hooks.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_lifetime.o $(OUT)/kernel-services/network/nettest_lifetime.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_neigh.o $(OUT)/kernel-services/network/nettest_neigh.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_worker.o $(OUT)/kernel-services/network/nettest_worker.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_accept.o $(OUT)/kernel-services/network/nettest_accept.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_seam.o $(OUT)/kernel-services/network/nettest_seam.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_nat.o $(OUT)/kernel-services/network/nettest_nat.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_nicbench.o $(OUT)/kernel-services/network/nettest_nicbench.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_bench.o $(OUT)/kernel-services/network/nettest_bench.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_harness.o $(OUT)/kernel-services/network/nettest_harness.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network
$(OUT)/kernel-services/network/nettest_frame.o $(OUT)/kernel-services/network/nettest_frame.analyzed: EXTRA_CFLAGS += -I$(ROOT)/kernel-services/network

# kernel-services/filesystem/cosmofs/cfsparse.c, cosmofs_reach.c, cosmofs_clone.c, cosmofs_rollback.c, cosmofs_scrub.c, cosmofs_crypt.c, cosmofs_resilver.c, cosmofs_member.c, cosmofs_check.c, cosmofs_snap.c, cosmofs_buf.c, cosmofs_alloc.c, cosmofs_commit.c, cosmofs_format.c, cosmofs_corrupt.c, cosmofs_hooks.c, cosmofs_core.c, cosmofs_snapctl.c, cosmofs_csum.c, cosmofs_data.c, cosmofs_file.c, cosmofs_vnode.c, cosmofs.c and cosmofs_dir.c are generated from their .cee into out/cee (scripts/cee-gen.py), away
# from the cosmofs_internal.h they include by name: their objects, and their analyses, look in the cosmofs directory too.
$(OUT)/kernel-services/filesystem/cosmofs/cfsparse.o $(OUT)/kernel-services/filesystem/cosmofs/cfsparse.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_reach.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_reach.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_clone.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_clone.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_rollback.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_rollback.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_scrub.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_scrub.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_crypt.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_crypt.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_resilver.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_resilver.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_member.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_member.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_check.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_check.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_snap.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_snap.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_buf.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_buf.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_alloc.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_alloc.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_commit.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_commit.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_format.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_format.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_corrupt.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_corrupt.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_hooks.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_hooks.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_core.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_core.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_snapctl.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_snapctl.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_csum.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_csum.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_data.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_data.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_file.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_file.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_vnode.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_vnode.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
$(OUT)/kernel-services/filesystem/cosmofs/cosmofs_dir.o $(OUT)/kernel-services/filesystem/cosmofs/cosmofs_dir.analyzed: \
	EXTRA_CFLAGS += -I$(ROOT)/kernel-services/filesystem/cosmofs
