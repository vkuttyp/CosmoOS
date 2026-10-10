# CosmoOS top-level build.
#
#   make [ARCH=x86_64|aarch64] [BUILD=debug|release] [V=1]   build loader, kernel, libc, userland,
#                     packages, modules and test guests
#   make image        FAT boot image with loader and kernel
#   make run          boot the image under QEMU on the terminal (serial)
#   make test         automated QEMU boot test with PASS/FAIL exit code
#   make test-gic     AArch64: the same boot test on the GICv3 machine
#   make test-guard   the same boot test on a CPU model with SMEP/SMAP/UMIP (x86-64) or PAN (AArch64)
#   make test-smp2    the same boot test with two CPUs (the default is four)
#   make test-install roadmap M2: install to a blank disk, boot it, persist a file (CI: BUILD=release)
#   make test-busybox roadmap M3: BusyBox ash's scripted test and testsuite subset (CI: BUILD=release)
#   make test-crash   build a deliberately faulting kernel, verify panic path
#   make test-wxn     AArch64: build a kernel that executes a writable page, verify WXN denies it
#   make test-chaos   debug suite under a migrator that moves ready threads between CPUs every few ticks
#   make test-harness-retry  boot with net-harness's first back-connection broken on purpose
#   make host-test    native unit tests of kernel algorithms under ASan/UBSan
#   make litmus       check the quiescence litmus tests against the C11 model (needs herd7)
#   make fuzz         fuzz the parsers on the host (docs/verification/)
#   make analyze      clang static analyzer over all target sources
#   make reproducible build twice into separate trees and compare outputs
#   make compile-commands  compile_commands.json for clangd with cross flags
#   make check-tools  verify the cross toolchain is usable (and run check-secrets)
#   make check-secrets  fail if a private or revoked module-signing key is tracked
#   make clean        remove $(OUT)
#
# Host and target are separate concepts throughout. See docs/build/.

ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

include $(ROOT)/build/config.mk
include $(ROOT)/build/toolchain.mk
include $(ROOT)/build/rules.mk

.PHONY: all kernel boot modules image run test test-entropy test-install test-busybox test-gic test-guard test-smp2 test-crash test-wxn test-chaos test-harness-retry analyze analysis-gate reproducible compile-commands check-tools check-secrets clean help litmus
.DEFAULT_GOAL := all

include $(ROOT)/kernel/kernel.mk
include $(ROOT)/boot/uefi/boot.mk
include $(ROOT)/libc/libc.mk
# BusyBox before userland: its applet list decides which native programs
# move aside to cosmo-<name> (userland/userland.mk).
include $(ROOT)/ports/busybox/busybox.mk
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
BOOT_ARCHIVE_ENTRIES = init=$(INIT_ELF) $(USER_ARCHIVE_ENTRIES) $(BUSYBOX_ARCHIVE_ENTRIES) sbin/pkg=$(PKG_ELF) $(PKG_ARCHIVE_ENTRIES) $(LINUX_TEST_ARCHIVE_ENTRIES) $(HV_ARCHIVE_ENTRIES) $(MODULE_ARCHIVE_ENTRIES)

$(BOOT_ARCHIVE): $(USER_ARCHIVE_DEPS) $(BUSYBOX_ARCHIVE_DEPS) $(PKG_ELF) $(PKG_INDEX) $(LINUX_TEST_ELFS) $(HV_GUEST_BINS) $(MODULE_KOS) $(ROOT)/scripts/mkbootarchive.py
	$(call log,ARCHIVE,$@)
	$(Q)$(PYTHON) $(ROOT)/scripts/mkbootarchive.py $@ $(BOOT_ARCHIVE_ENTRIES)

$(IMAGE): $(KERNEL_ELF) $(LOADER_EFI) $(BOOT_ARCHIVE) $(ROOT)/scripts/mkimage.sh $(ROOT)/scripts/mkgpt.py
	$(call log,IMAGE,$@)
	$(Q)$(ROOT)/scripts/mkimage.sh $@ $(LOADER_EFI) $(KERNEL_ELF) $(BOOT_ARCHIVE)

run: $(IMAGE)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) QEMU_EXTRA="$(QEMU_EXTRA)" \
		$(ROOT)/scripts/qemu-run.sh $(IMAGE)

# BOOT_LOG: where the serial log goes; a variant boot names its own so
# CI's artifact keeps both.
BOOT_LOG ?= $(OUT)/boot-test.log
# The whole-boot timeout for test and test-harness-retry, and for test-gic,
# test-guard and test-smp2 through test; test-chaos sets its own 240 s, and
# test-crash and test-wxn keep the runner's 180 s (they end in an early
# panic). A hang guard, not a performance budget (each
# self-test has its own budget). AArch64 under TCG is the slower guest, and
# over 257 CI boots (40 runs, 2026-10-06) it took a median of 154 s, p95 173 s,
# and exceeded 180 s four times on slow runners -- twice on branches before
# the one that prompted this -- with every self-test passed. 240 s matches
# test-chaos. x86-64 followed on 2026-10-07: over 81 boots of main's last
# twelve runs it took a median of 141 s and a p95 of 170 s, with a maximum
# of 175 s passing, on runners of two speeds (about 125 s and about 165 s a
# boot); on the slow kind the suite had grown to within 5 s of 180, and the
# merge of PR #325 timed out its test-harness-retry boot at 184 s with every
# self-test passed (run 37653536848). The same 240 s on both.
# The shell, network and key harnesses derive their deadlines from it.
BOOT_TIMEOUT ?= 240
# test-busybox: one boot that runs the scripted test and fifteen testsuite files.
BUSYBOX_TIMEOUT ?= 900
test: $(IMAGE)
	$(Q)COSMO_ARCH=$(ARCH) QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) QEMU_EXTRA="$(QEMU_EXTRA)" HAVE_MUSL=$(HAVE_MUSL) \
		$(PYTHON) $(ROOT)/tests/boot/run_boot_test.py --timeout $(BOOT_TIMEOUT) --image $(IMAGE) --log $(BOOT_LOG) \
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

# M1's acceptance boots (docs/kernel/security/design.md §6): the same
# image without the virtio-rng, first on the default CPU model, which has
# no random instruction either -- no entropy source at all: getrandom's
# NONBLOCK is EAGAIN, INSECURE answers, an encrypted pool is refused and
# the boot completes -- then on a model with the random instructions,
# which must seed the pool from the CPU alone. The ordinary `test` boot
# is the device-seeded configuration. The harness reads QEMU_RNG and
# QEMU_HWRNG to know which lines to require.
ifeq ($(ARCH),aarch64)
HWRNG_CPU ?= neoverse-v1
else
HWRNG_CPU ?= qemu64,+nx,+svm,+npt,+rdrand,+rdseed
endif
test-entropy:
	$(Q)QEMU_RNG=0 $(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) \
		BOOT_LOG=$(OUT)/boot-test-noentropy.log test
	$(Q)QEMU_RNG=0 QEMU_HWRNG=1 QEMU_CPU='$(HWRNG_CPU)' $(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) \
		BUILD=$(BUILD) BOOT_LOG=$(OUT)/boot-test-cpurng.log test

# Roadmap M2's acceptance test (tests/boot/install_test.py,
# docs/userland/testing.md): the live image installs itself on a blank
# disk with cosmo-install, and the disk is checked on the host. Run with
# BUILD=release in CI, where each boot takes seconds. Its own work
# directory: the boots' scratch disks and serial logs stay beside it.
# Roadmap M3's acceptance test: BusyBox ash runs the scripted test and the
# testsuite subset (docs/userland/testing.md, "BusyBox"; CI: BUILD=release).
test-busybox: $(IMAGE)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/busybox_test.py --image $(IMAGE) --workdir $(OUT)/test-busybox \
		--timeout $(BUSYBOX_TIMEOUT)

test-install: $(IMAGE)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/install_test.py --image $(IMAGE) --workdir $(OUT)/test-install \
		--timeout $(BOOT_TIMEOUT)

# The same boot test with two CPUs. Every other boot here uses the
# default four, and a test that needs a third CPU without saying so passes
# them all: `virtio-remove-inflight`, `sched-spread` and `sched-balance-pair`
# each failed at two CPUs, unseen by CI
# (docs/audit/2026-10-05-two-cpu-validation-report.md). This boot catches a
# test that wrongly assumes a third CPU. It does not run every SMP test:
# those that genuinely need three or more CPUs (`sched-spread`,
# `sched-migrate` and others) skip here with their reason, and the
# four-CPU boots run them. One CPU is covered by `QEMU_SMP=1 make test`
# locally.
test-smp2:
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) QEMU_SMP=2 \
		BOOT_LOG=$(OUT)/boot-test-smp2.log test

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
# moved something. CI's ordinary AArch64 boots take up to 167 s; chaos
# added 11 s of self-test work and exhausted the shell's 170 s deadline
# under the then-default 180 s total. 240 s for this heavier full boot;
# individual self-test watchdogs and shell signal-latency checks remain.
# (Ordinary boots on both architectures have 240 s too since BOOT_TIMEOUT,
# above, so this no longer lengthens anything; it stays explicit.)
test-chaos:
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=debug \
		SCHED_CHAOS=1 OUT=$(OUT)-chaos image
	$(Q)COSMO_ARCH=$(ARCH) QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) QEMU_EXTRA="$(QEMU_EXTRA)" \
		$(PYTHON) $(ROOT)/tests/boot/run_boot_test.py --chaos --timeout 240 \
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
		$(PYTHON) $(ROOT)/tests/boot/run_boot_test.py --harness-retry --timeout $(BOOT_TIMEOUT) \
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

# The quiescence protocol's memory-order litmus tests, checked by herd7
# against the RC11 model: verdicts, reachability witnesses and negative
# controls (tests/litmus/run_litmus.py, docs/kernel/quiesce/testing.md).
# herdtools7 comes from opam or Debian testing/sid; CI runs it in a job
# of its own.
litmus:
	$(Q)$(PYTHON) $(ROOT)/tests/litmus/run_litmus.py

ANALYSIS_REPORTS ?= $(KERNEL_ANALYZE) $(LOADER_ANALYZE) $(MODULE_ANALYZE) $(PKG_ANALYZE)

analysis-gate:
	$(Q)$(PYTHON) $(ROOT)/scripts/check-analysis.py --root $(ROOT) --out $(OUT) \
		--arch $(ARCH) --baseline $(ROOT)/tools/analysis/$(ARCH).json \
		--inventory $(OUT)/analysis-inventory.json $(ANALYSIS_REPORTS)

analyze: $(KERNEL_ANALYZE) $(LOADER_ANALYZE) $(MODULE_ANALYZE) $(PKG_ANALYZE) $(KERNEL_ELF)
	$(Q)$(ROOT)/scripts/check-fpregs.sh $(KERNEL_ELF) $(OBJDUMP)
	$(Q)$(MAKE) --no-print-directory analysis-gate

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
	@sed -n '2,/^#   make clean/p' $(ROOT)/Makefile | sed 's/^# \{0,1\}//'
	@echo
	@echo "ARCH=$(ARCH) BUILD=$(BUILD) OUT=$(OUT)"
	@echo "HOST=$(HOST_OS)/$(HOST_ARCH) CC=$(CC) LD=$(LD)"
