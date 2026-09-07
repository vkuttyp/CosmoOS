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
	kernel/core/log.c \
	kernel/core/panic.c \
	kernel/core/printf.c \
	kernel/core/string.c \
	kernel/core/shutdown.c \
	kernel/core/selftest.c \
	kernel/core/spinlock.c \
	kernel/core/percpu.c \
	kernel/core/smp.c \
	kernel/core/quiesce.c \
	kernel/core/quiescetest.c \
	kernel/core/lockdep.c \
	kernel/core/lockdeptest.c \
	kernel/core/faultinject.c \
	kernel/core/extable.c \
	kernel/core/faulttest.c \
	kernel/interrupt/interrupt.c \
	kernel/interrupt/irq.c \
	kernel/interrupt/ipi.c \
	kernel/memory/bootmem.c \
	kernel/memory/buddy.c \
	kernel/memory/pmm.c \
	kernel/memory/vmm.c \
	kernel/memory/slab.c \
	kernel/memory/kmalloc.c \
	kernel/memory/memtest.c \
	kernel/timer/timer.c \
	kernel/scheduler/thread.c \
	kernel/scheduler/sched.c \
	kernel/scheduler/policy_rr.c \
	kernel/scheduler/wait.c \
	kernel/scheduler/mutex.c \
	kernel/scheduler/semaphore.c \
	kernel/scheduler/completion.c \
	kernel/scheduler/schedtest.c \
	kernel/scheduler/smptest.c \
	kernel/object/object.c \
	kernel/object/handle.c \
	kernel/object/console_obj.c \
	kernel/tty/tty.c \
	kernel/tty/ttytest.c \
	kernel/ipc/pipe.c \
	kernel/ipc/pipetest.c \
	kernel/ipc/futex.c \
	kernel/io/aio.c \
	kernel/io/poll.c \
	kernel/io/polltest.c \
	compat/linux/convert.c \
	compat/linux/syscalls.c \
	compat/linux/signal.c \
	kernel-services/virtualization/vmm.c \
	kernel-services/virtualization/vm.c \
	kernel-services/virtualization/guestmem.c \
	kernel-services/virtualization/vmdev.c \
	kernel-services/virtualization/vintr.c \
	kernel-services/virtualization/vcpu.c \
	kernel-services/virtualization/hvsys.c \
	kernel-services/virtualization/hvtest.c \
	kernel/process/elf.c \
	kernel/process/cred.c \
	kernel/process/process.c \
	kernel/process/spawn.c \
	kernel/process/signal.c \
	kernel/process/proctest.c \
	kernel/syscall/syscall.c \
	kernel/syscall/native.c \
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
	kernel/device/device.c \
	kernel/device/dma.c \
	kernel/device/devtest.c \
	kernel/block/blk.c \
	kernel/block/ramblk.c \
	kernel/block/blktest.c \
	kernel/core/random.c \
	kernel/core/crc32c.c \
	kernel/core/lz4.c \
	kernel/security/chacha20.c \
	kernel/core/fwcfg.c \
	kernel-services/vfs/vfs.c \
	kernel-services/vfs/pagecache.c \
	kernel-services/vfs/ramfs.c \
	kernel-services/vfs/vfstest.c \
	kernel-services/storage/pool.c \
	kernel-services/filesystem/cosmofs/cosmofs_core.c \
	kernel-services/filesystem/cosmofs/cosmofs_snap.c \
	kernel-services/filesystem/cosmofs/cosmofs_member.c \
	kernel-services/filesystem/cosmofs/cosmofs_scrub.c \
	kernel-services/filesystem/cosmofs/cosmofs_crypt.c \
	kernel-services/filesystem/cosmofs/cosmofs.c \
	kernel-services/filesystem/cosmofs/cosmofstest.c \
	kernel-services/filesystem/cosmofs/cosmofscrash.c \
	kernel-services/network/mbuf.c \
	kernel-services/network/cksum.c \
	kernel-services/network/inet.c \
	kernel-services/network/netif.c \
	kernel-services/network/loopback.c \
	kernel-services/network/ether.c \
	kernel-services/network/arp.c \
	kernel-services/network/ipv4.c \
	kernel-services/network/ipv6.c \
	kernel-services/network/udp.c \
	kernel-services/network/tcp.c \
	kernel-services/network/socket.c \
	kernel-services/network/nettest.c \
	drivers/acpi/acpi.c \
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

$(KEYRING_SRC): $(KEYRING_PUBS) $(ROOT)/scripts/gen-keyring.py
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

kernel: $(KERNEL_ELF)

-include $(KERNEL_OBJS:.o=.d)
