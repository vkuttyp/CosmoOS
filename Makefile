# CosmoOS top-level build.
#
#   make [ARCH=x86_64|aarch64] [BUILD=debug|release] [V=1]   build loader + kernel
#   make image        FAT boot image with loader and kernel
#   make run          boot the image under QEMU on the terminal (serial)
#   make test         automated QEMU boot test with PASS/FAIL exit code
#   make test-gic     AArch64: the same boot test on the GICv3 machine
#   make test-guard   the same boot test on a CPU model with SMEP/SMAP/UMIP (x86-64) or PAN (AArch64)
#   make test-crash   build a deliberately faulting kernel, verify panic path
#   make test-wxn     AArch64: build a kernel that executes a writable page, verify WXN denies it
#   make test-chaos   debug suite under a migrator that moves ready threads between CPUs every few ticks
#   make test-harness-retry  boot with net-harness's first back-connection broken on purpose
#   make host-test    native unit tests of kernel algorithms under ASan/UBSan
#   make fuzz         fuzz the parsers on the host (docs/verification/)
#   make analyze      clang static analyzer over all target sources
#   make reproducible build twice into separate trees and compare outputs
#   make compile-commands  compile_commands.json for clangd with cross flags
#   make check-tools  verify the cross toolchain is usable
#   make clean        remove $(OUT)
#
# Host and target are separate concepts throughout. See docs/build/.

ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

include $(ROOT)/build/config.mk
include $(ROOT)/build/toolchain.mk
include $(ROOT)/build/rules.mk

.PHONY: all kernel boot modules image run test test-gic test-guard test-crash test-wxn test-chaos test-harness-retry analyze reproducible compile-commands check-tools check-secrets clean help
.DEFAULT_GOAL := all

include $(ROOT)/kernel/kernel.mk
include $(ROOT)/boot/uefi/boot.mk
include $(ROOT)/libc/libc.mk
include $(ROOT)/userland/userland.mk
include $(ROOT)/pkg/pkg.mk
# The Linux ABI test programs and the virtualization guests both build
# for the architecture in hand; the guests live in tests/hv/$(ARCH)/.
include $(ROOT)/tests/linux/linux.mk
include $(ROOT)/tests/hv/hv.mk
ARCH_TEST_TARGETS := hv-guests linux-tests
include $(ROOT)/build/module.mk
include $(ROOT)/tests/host/host.mk
include $(ROOT)/tests/fuzz/fuzz.mk

all: $(ARCH_TEST_TARGETS) kernel boot libc userland pkg ports modules

IMAGE := $(OUT)/cosmoos.img

image: $(IMAGE)

# The boot archive: init plus the boot-time and test modules, in the
# order the kernel loads them (dependencies first). See
# scripts/mkbootarchive.py and docs/kernel/module/.
BOOT_ARCHIVE := $(OUT)/boot.tar
BOOT_ARCHIVE_ENTRIES = init=$(INIT_ELF) $(USER_ARCHIVE_ENTRIES) sbin/pkg=$(PKG_ELF) $(PKG_ARCHIVE_ENTRIES) $(LINUX_TEST_ARCHIVE_ENTRIES) $(HV_ARCHIVE_ENTRIES) $(MODULE_ARCHIVE_ENTRIES)

$(BOOT_ARCHIVE): $(USER_ARCHIVE_DEPS) $(PKG_ELF) $(PKG_INDEX) $(LINUX_TEST_ELFS) $(HV_GUEST_BINS) $(MODULE_KOS) $(ROOT)/scripts/mkbootarchive.py
	$(call log,ARCHIVE,$@)
	$(Q)$(PYTHON) $(ROOT)/scripts/mkbootarchive.py $@ $(BOOT_ARCHIVE_ENTRIES)

$(IMAGE): $(KERNEL_ELF) $(LOADER_EFI) $(BOOT_ARCHIVE) $(ROOT)/scripts/mkimage.sh
	$(call log,IMAGE,$@)
	$(Q)$(ROOT)/scripts/mkimage.sh $@ $(LOADER_EFI) $(KERNEL_ELF) $(BOOT_ARCHIVE)

run: $(IMAGE)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) QEMU_EXTRA="$(QEMU_EXTRA)" \
		$(ROOT)/scripts/qemu-run.sh $(IMAGE)

# BOOT_LOG: where the serial log goes; a variant boot names its own so
# CI's artifact keeps both.
BOOT_LOG ?= $(OUT)/boot-test.log
test: $(IMAGE)
	$(Q)COSMO_ARCH=$(ARCH) QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) QEMU_EXTRA="$(QEMU_EXTRA)" HAVE_MUSL=$(HAVE_MUSL) \
		$(PYTHON) $(ROOT)/tests/boot/run_boot_test.py --image $(IMAGE) --log $(BOOT_LOG) \
		--kernel $(KERNEL_ELF) --symbolizer $(LLVM_PREFIX)llvm-symbolizer \
		$(if $(filter release,$(BUILD)),--shell-burst)

# The default CPU models (scripts/qemu-run.sh) have no SMEP, SMAP or UMIP
# (qemu64) and no PAN (cortex-a72), so on them the kernel's guard on its
# own access to user memory is a no-op and `test` cannot see it fail.
# This boots the same image on a model that has the guard, and the
# harness requires the kernel's own line saying every protection is on
# (QEMU_GUARD=1; docs/kernel/security/design.md, "Hardening"). The
# default boot stays the control, where the guard's absence is handled.
test-guard:
ifeq ($(ARCH),aarch64)
	$(Q)QEMU_GUARD=1 QEMU_CPU=cortex-a76 $(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) \
		BOOT_LOG=$(OUT)/boot-test-guard.log test
else
	$(Q)QEMU_GUARD=1 QEMU_CPU='qemu64,+nx,+svm,+npt,+smep,+smap,+umip' $(MAKE) --no-print-directory -C $(ROOT) \
		ARCH=$(ARCH) BUILD=$(BUILD) BOOT_LOG=$(OUT)/boot-test-guard.log test
endif

# QEMU's virt machine defaults to gic-version=2, so `test` exercises one
# of the two AArch64 interrupt controllers and never the other. This runs
# the other, in both of the MSI configurations a GICv3 can have: an ITS
# (what GICv3 hardware offers, and QEMU's default for that machine) and a
# GICv2m frame (the fallback when firmware describes no ITS). A no-op on
# architectures with no GIC, so CI can call it for every target.
test-gic:
ifeq ($(ARCH),aarch64)
	$(Q)QEMU_GIC=3 $(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) test
	$(Q)if qemu-system-aarch64 -machine virt,help 2>&1 | grep -q '^  *msi='; then \
		QEMU_GIC=3 QEMU_MSI=gicv2m $(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) test; \
	else \
		echo "test-gic: this QEMU has no virt 'msi' property (needs 11 or newer);"; \
		echo "test-gic: the GICv2m-under-GICv3 fallback is not exercised here."; \
	fi
else
	@echo "test-gic: $(ARCH) has no GIC; nothing to do"
endif

# The whole suite under a chaos migrator: a debug kernel whose tick moves
# a ready thread to another CPU every few ticks for no reason
# (SCHED_CHAOS=1, docs/kernel/scheduler/design.md "Migration"), built
# into a sibling output tree. The harness requires the migrator to have
# moved something.
test-chaos:
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=debug \
		SCHED_CHAOS=1 OUT=$(OUT)-chaos image
	$(Q)COSMO_ARCH=$(ARCH) QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) QEMU_EXTRA="$(QEMU_EXTRA)" \
		$(PYTHON) $(ROOT)/tests/boot/run_boot_test.py --chaos \
		--image $(OUT)-chaos/cosmoos.img --log $(OUT)-chaos/boot-test-chaos.log \
		--kernel $(OUT)-chaos/kernel/kernel.elf --symbolizer $(LLVM_PREFIX)llvm-symbolizer

# Build a kernel whose net-harness back-connection fails on purpose the
# first time and boot it: the retry that exists for a QEMU defect seen on
# one boot in twenty, exercised on this one
# (docs/audit/next-subsystem-nettest-retry.md).
test-harness-retry:
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=debug \
		HARNESS_BREAK=1 OUT=$(OUT)-hbreak image
	$(Q)COSMO_ARCH=$(ARCH) QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) QEMU_EXTRA="$(QEMU_EXTRA)" \
		$(PYTHON) $(ROOT)/tests/boot/run_boot_test.py --harness-retry \
		--image $(OUT)-hbreak/cosmoos.img --log $(OUT)-hbreak/boot-test-hbreak.log \
		--kernel $(OUT)-hbreak/kernel/kernel.elf --symbolizer $(LLVM_PREFIX)llvm-symbolizer

# Build a deliberately crashing kernel into a sibling output tree and
# verify that the panic path reports properly and the harness sees FAIL.
test-crash:
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) \
		CRASH_TEST=1 OUT=$(OUT)-crash image
	$(Q)COSMO_ARCH=$(ARCH) QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) QEMU_EXTRA="$(QEMU_EXTRA)" \
		$(PYTHON) $(ROOT)/tests/boot/run_boot_test.py --expect-panic fault \
		--image $(OUT)-crash/cosmoos.img --log $(OUT)-crash/boot-test-crash.log \
		--kernel $(OUT)-crash/kernel/kernel.elf --symbolizer $(LLVM_PREFIX)llvm-symbolizer

# AArch64: build a kernel that maps one page writable and executable on
# purpose and executes it (CRASH_TEST=2); SCTLR_EL1.WXN must make that
# an instruction abort, and the harness requires that panic's report.
# A no-op elsewhere (x86-64 has no WXN; its NX is proved by test-crash).
test-wxn:
ifeq ($(ARCH),aarch64)
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) \
		CRASH_TEST=2 OUT=$(OUT)-wxn image
	$(Q)COSMO_ARCH=$(ARCH) QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) QEMU_EXTRA="$(QEMU_EXTRA)" \
		$(PYTHON) $(ROOT)/tests/boot/run_boot_test.py --expect-panic wxn \
		--image $(OUT)-wxn/cosmoos.img --log $(OUT)-wxn/boot-test-wxn.log \
		--kernel $(OUT)-wxn/kernel/kernel.elf --symbolizer $(LLVM_PREFIX)llvm-symbolizer
else
	@echo "test-wxn: $(ARCH) has no WXN; nothing to do"
endif

analyze: $(KERNEL_ANALYZE) $(LOADER_ANALYZE) $(MODULE_ANALYZE) $(PKG_ANALYZE) $(KERNEL_ELF)
	$(Q)$(ROOT)/scripts/check-fpregs.sh $(KERNEL_ELF) $(OBJDUMP)
	@echo "static analysis: clean"

reproducible:
	$(Q)$(ROOT)/scripts/check-reproducible.sh $(ARCH) $(BUILD)

# compile_commands.json for clangd/IDEs, using the real cross flags so
# editor diagnostics match the build. The file is git-ignored.
ARCH_INC := -I$(ROOT)/kernel/arch/$(ARCH)/include
compile-commands:
	$(call log,GEN,$(ROOT)/compile_commands.json)
	$(Q)( \
	  $(foreach s,$(KERNEL_GENERIC_SRCS),printf '%s\t%s\n' '$(s)' '$(KERNEL_CFLAGS)';) \
	  $(foreach s,$(filter %.c,$(KERNEL_ARCH_SRCS)),printf '%s\t%s\n' '$(s)' '$(KERNEL_CFLAGS) $(ARCH_INC)';) \
	  $(foreach s,$(LOADER_SRCS),printf '%s\t%s\n' '$(s)' '$(LOADER_CFLAGS)';) \
	  $(foreach m,$(MODULES),$(foreach s,$(MODULE_$(m)_SRCS),printf '%s\t%s\n' '$(s)' '$(MODULE_CFLAGS)';)) \
	) | $(PYTHON) $(ROOT)/scripts/gen-compile-commands.py $(ROOT) $(CC) > $(ROOT)/compile_commands.json

check-tools:
	$(Q)$(ROOT)/scripts/check-tools.sh "$(CC)" "$(LD)" "$(LDLINK)" "$(OBJCOPY)" "$(PYTHON)"
	$(Q)$(ROOT)/scripts/check-secrets.sh

# No private key and no revoked public key may be tracked (docs/kernel/module/design.md).
check-secrets:
	$(Q)$(ROOT)/scripts/check-secrets.sh

clean:
	$(Q)rm -rf $(OUT)

help:
	@sed -n '2,16p' $(ROOT)/Makefile | sed 's/^# \{0,1\}//'
	@echo
	@echo "ARCH=$(ARCH) BUILD=$(BUILD) OUT=$(OUT)"
	@echo "HOST=$(HOST_OS)/$(HOST_ARCH) CC=$(CC) LD=$(LD)"
