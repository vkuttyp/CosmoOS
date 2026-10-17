# CosmoOS top-level build.
#
#   make [ARCH=x86_64|aarch64] [BUILD=debug|release] [V=1]   build loader, kernel, libc, userland,
#                     packages, modules and test guests
#   make image        FAT boot image with loader and kernel
#   make run          boot the image under QEMU on the terminal (serial)
#   make test         automated QEMU boot test with PASS/FAIL exit code
#   make test-gic     AArch64: the same boot test on the GICv3 machine
#   make test-platforms  AArch64: boots where QEMU differs from virt's defaults; the platform line (CI: BUILD=release)
#   make test-guard   the same boot test on a CPU model with SMEP/SMAP/UMIP (x86-64) or PAN (AArch64)
#   make test-smp2    the same boot test with two CPUs (the default is four)
#   make test-install roadmap M2: install to a blank disk, boot it, persist a file (CI: BUILD=release)
#   make test-upgrade shell completeness S7: upgrade an installed v1.0.3 to this build, roll back (CI: BUILD=release)
#   make test-busybox roadmap M3: BusyBox ash's scripted test and testsuite subset (CI: BUILD=release)
#   make test-security roadmap M5: verified boot refuses tampered files; VERIFY_BOOT=0 says so (CI: BUILD=release)
#   make test-net-login roadmap M4: DHCP, wget by address and by name on the installed system (CI: BUILD=release)
#   make test-display  virtio-gpu: the console on the display's own size, read back from a screendump (CI: BUILD=release)
#   make test-crash   build a deliberately faulting kernel, verify panic path
#   make test-wxn     AArch64: build a kernel that executes a writable page, verify WXN denies it
#   make test-chaos   debug suite under a migrator that moves ready threads between CPUs every few ticks
#   make test-harness-retry  boot with net-harness's first back-connection broken on purpose
#   make host-test    native unit tests of kernel algorithms under ASan/UBSan
#   make litmus       check the quiescence litmus tests against the C11 model (needs herd7)
#   make fuzz         fuzz the parsers on the host (docs/verification/)
#   make analyze      clang static analyzer over all target sources
#   make reproducible build twice into separate trees and compare outputs, the release set included
#   make release      roadmap M6: the signed release set for ARCH in out/release/ARCH (BUILD=release)
#   make compile-commands  compile_commands.json for clangd with cross flags
#   make check-tools  verify the cross toolchain is usable (and run check-secrets)
#   make check-secrets  fail if a private or revoked module-signing key is tracked
#   make check-docs   every subsystem in docs/README.md has its five documentation files
#   make clean        remove $(OUT)
#
# Host and target are separate concepts throughout. See docs/build/.

ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

include $(ROOT)/build/config.mk
include $(ROOT)/build/toolchain.mk
include $(ROOT)/build/rules.mk

.PHONY: FORCE all kernel boot modules image run test test-entropy test-install test-upgrade test-security test-busybox test-net-login test-display test-gic test-platforms test-guard test-smp2 test-crash test-wxn test-chaos test-harness-retry analyze analysis-gate reproducible release release-set compile-commands check-tools check-secrets check-docs clean help litmus
.DEFAULT_GOAL := all

include $(ROOT)/kernel/kernel.mk
include $(ROOT)/boot/uefi/boot.mk
include $(ROOT)/libc/libc.mk
# BusyBox before userland: its applet list decides which native programs
# move aside to cosmo-<name> (userland/userland.mk).
include $(ROOT)/ports/busybox/busybox.mk
include $(ROOT)/ports/dropbear/dropbear.mk
include $(ROOT)/ports/openssh/openssh.mk
include $(ROOT)/ports/mbedtls/mbedtls.mk
include $(ROOT)/userland/userland.mk
include $(ROOT)/pkg/pkg.mk
# The Linux ABI test programs and the virtualization guests both build
# for the architecture in hand; the guests live in tests/hv/$(ARCH)/.
include $(ROOT)/tests/linux/linux.mk
include $(ROOT)/tests/security/security.mk
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
# scripts/mkbootarchive.py and docs/kernel/module/. The loader rides in it
# (efi/, shell completeness S7): the manifest signs the archive, so
# cosmo-upgrade installs a loader the signature covers.
BOOT_ARCHIVE := $(OUT)/boot.tar
BOOT_ARCHIVE_ENTRIES = init=$(INIT_ELF) $(USER_ARCHIVE_ENTRIES) $(BUSYBOX_ARCHIVE_ENTRIES) $(DROPBEAR_ARCHIVE_ENTRIES) $(OPENSSH_ARCHIVE_ENTRIES) $(MBEDTLS_ARCHIVE_ENTRIES) sbin/pkg=$(PKG_ELF) $(PKG_ARCHIVE_ENTRIES) $(LINUX_TEST_ARCHIVE_ENTRIES) $(SECURITY_TEST_ARCHIVE_ENTRIES) $(HV_ARCHIVE_ENTRIES) $(MODULE_ARCHIVE_ENTRIES) efi/$(LOADER_EFI_NAME)=$(LOADER_EFI)

$(BOOT_ARCHIVE): $(USER_ARCHIVE_DEPS) $(BUSYBOX_ARCHIVE_DEPS) $(DROPBEAR_ARCHIVE_DEPS) $(OPENSSH_ARCHIVE_DEPS) $(MBEDTLS_ARCHIVE_DEPS) $(PKG_ELF) $(PKG_INDEX) $(LINUX_TEST_ELFS) $(SECURITY_TEST_ELFS) $(HV_GUEST_BINS) $(MODULE_KOS) $(LOADER_EFI) $(ROOT)/scripts/mkbootarchive.py
	$(call log,ARCHIVE,$@)
	$(Q)$(PYTHON) $(ROOT)/scripts/mkbootarchive.py $@ $(BOOT_ARCHIVE_ENTRIES)

# The command line, \cosmo\cmdline (docs/boot/design.md, "The command
# line"). It is signed with the kernel and the archive, so it is fixed at
# build time: root=boot-disk names the CosmoOS root partition on the disk
# the loader was read from, which the live image does not have and the
# installed disk does (roadmap M5: the installer signs nothing and writes
# no file on the ESP). Rewritten only when CMDLINE changes.
CMDLINE ?= root=boot-disk
CMDLINE_FILE := $(OUT)/cmdline
FORCE:
# $(file) writes the value exactly, newline added: no shell sees it, so
# a quote in it is a quote in the signed text (PR #368 review).
$(CMDLINE_FILE): FORCE
	$(shell mkdir -p $(dir $@))$(file >$@.tmp,$(CMDLINE))
	$(Q)if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

# The signed manifest over kernel, archive and command line (scripts/
# mkmanifest.py, docs/boot/design.md "Verified boot"), with the key that
# signs modules: the loader trusts the ring the kernel trusts.
MANIFEST := $(OUT)/manifest
$(MANIFEST): $(KERNEL_ELF) $(BOOT_ARCHIVE) $(CMDLINE_FILE) $(MODSIGN_KEY) $(SIGNING_STAMP) $(ROOT)/scripts/mkmanifest.py \
		$(ROOT)/scripts/modsign.py
	$(call log,MANIFEST,$@)
	$(Q)$(PYTHON) $(ROOT)/scripts/mkmanifest.py sign --key $(MODSIGN_KEY) --out $@ $(KERNEL_ELF) $(BOOT_ARCHIVE) $(CMDLINE_FILE)

$(IMAGE): $(KERNEL_ELF) $(LOADER_EFI) $(BOOT_ARCHIVE) $(CMDLINE_FILE) $(MANIFEST) $(ROOT)/scripts/mkimage.sh $(ROOT)/scripts/mkgpt.py
	$(call log,IMAGE,$@)
	$(Q)$(ROOT)/scripts/mkimage.sh $@ $(LOADER_EFI) $(KERNEL_ELF) $(BOOT_ARCHIVE) $(CMDLINE_FILE) $(MANIFEST)

# Roadmap M6 (docs/development.md, "Releases"): one architecture's release
# set -- the signed live image, the loader, kernel.elf, boot.tar, the
# command line, the manifest, cosmo-upgrade (S7), the signed package of
# cosmo-upgrade and cosmo-verify, SHA256SUMS and its signature -- made by
# scripts/mkrelease.py in RELEASE_DIR under the names the GitHub release carries.
# Always a release build: `make release` runs release-set with
# BUILD=release. Signed with whatever key SIGNING selects: the developer
# key by default, the release key in the release workflow (SIGNING=release
# MODSIGN_KEY=... KEYRING_PUBS=tools/keys/<release>.pub).
RELEASE_DIR ?= $(ROOT)/out/release/$(ARCH)
release:
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=release RELEASE_DIR=$(RELEASE_DIR) release-set

UPGRADE_ELF = $(call prog_elf,cosmo-upgrade)
VERIFY_ELF = $(call prog_elf,cosmo-verify)
release-set: $(IMAGE) $(LOADER_EFI) $(KERNEL_ELF) $(BOOT_ARCHIVE) $(CMDLINE_FILE) $(MANIFEST) $(UPGRADE_ELF) $(VERIFY_ELF) \
		$(ROOT)/scripts/mkrelease.py $(ROOT)/tools/pkgbuild.py $(ROOT)/scripts/modsign.py
	$(call log,RELEASE,$(RELEASE_DIR))
	$(Q)$(PYTHON) $(ROOT)/scripts/mkrelease.py $(ARCH) $(RELEASE_DIR) $(IMAGE) $(LOADER_EFI) $(KERNEL_ELF) \
		$(BOOT_ARCHIVE) $(MANIFEST) $(CMDLINE_FILE) $(UPGRADE_ELF) $(VERIFY_ELF) $(MODSIGN_KEY) $(PKGSIGN_KEY) $(BUILD_ID)

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
# AArch64 300 s from 2026-10-15: over the last 40 CI runs its entropy job's
# 146 boots took a median of 212 s and its passing ones at most 235.7 s, and
# six timed out at 240 s with every self-test passed, all six the
# no-entropy boot, whose self-test time puts their whole boot at 240-246 s
# (docs/testing/flakes.md). The build job's 261 AArch64 boots: p95 217 s,
# at most 228.8 s.
# The shell, network and key harnesses derive their deadlines from it.
BOOT_TIMEOUT ?= $(if $(filter aarch64,$(ARCH)),300,240)
# test-busybox: one boot that runs the scripted test and fifteen testsuite files.
BUSYBOX_TIMEOUT ?= 900
test: $(IMAGE)
	$(Q)COSMO_ARCH=$(ARCH) QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) QEMU_EXTRA="$(QEMU_EXTRA)" HAVE_MUSL=$(HAVE_MUSL) \
		BOOT_CMDLINE="$$(cat $(CMDLINE_FILE))" $(PYTHON) $(ROOT)/tests/boot/run_boot_test.py --timeout $(BOOT_TIMEOUT) --image $(IMAGE) --log $(BOOT_LOG) \
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

# M1's acceptance boots (docs/kernel/security/design.md §6), each with
# one entropy source: the image without the virtio-rng on the default CPU
# model, which has no random instruction either, and `random.jitter=off`
# -- no source at all: getrandom's NONBLOCK is EAGAIN, INSECURE answers,
# an encrypted pool is refused and the boot completes; on AArch64 the same
# without `random.jitter=off`, which the jitter source must seed (not on
# x86-64: under QEMU's TCG its timings are, in some boots, regular enough
# that the estimator refuses every batch, as it should; the AArch64
# machine that needs the source is Parallels Desktop); a model with the
# random instructions and `random.firmware=off` (OVMF and AAVMF offer the
# CPU's instructions as EFI_RNG_PROTOCOL), which must seed the pool from
# the CPU alone; and the
# virtio-rng with `random.firmware=off random.jitter=off`, which must
# seed from the device alone. The ordinary `test` boot is seeded by the
# firmware (boot protocol v10: OVMF and AAVMF offer EFI_RNG_PROTOCOL over
# the virtio-rng). The harness reads QEMU_RNG, QEMU_HWRNG, BOOT_FW_SEED
# and BOOT_JITTER to know which lines to require.
ifeq ($(ARCH),aarch64)
HWRNG_CPU ?= neoverse-v1
NORNG_CPU := cortex-a72
else
HWRNG_CPU ?= qemu64,+nx,+svm,+npt,+rdrand,+rdseed
NORNG_CPU := qemu64,+nx,+svm,+npt
endif
test-entropy:
	$(Q)QEMU_RNG=0 QEMU_HWRNG=0 QEMU_CPU='$(NORNG_CPU)' BOOT_JITTER=0 $(MAKE) --no-print-directory -C $(ROOT) \
		ARCH=$(ARCH) BUILD=$(BUILD) CMDLINE='$(CMDLINE) random.jitter=off' BOOT_LOG=$(OUT)/boot-test-noentropy.log test
ifeq ($(ARCH),aarch64)
	$(Q)QEMU_RNG=0 QEMU_HWRNG=0 QEMU_CPU='$(NORNG_CPU)' $(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) \
		BUILD=$(BUILD) BOOT_LOG=$(OUT)/boot-test-jitter.log test
endif
	$(Q)QEMU_RNG=0 QEMU_HWRNG=1 QEMU_CPU='$(HWRNG_CPU)' BOOT_FW_SEED=0 $(MAKE) --no-print-directory -C $(ROOT) \
		ARCH=$(ARCH) BUILD=$(BUILD) CMDLINE='$(CMDLINE) random.firmware=off' BOOT_LOG=$(OUT)/boot-test-cpurng.log test
	$(Q)QEMU_HWRNG=0 QEMU_CPU='$(NORNG_CPU)' BOOT_FW_SEED=0 BOOT_JITTER=0 $(MAKE) --no-print-directory -C $(ROOT) \
		ARCH=$(ARCH) BUILD=$(BUILD) CMDLINE='$(CMDLINE) random.firmware=off random.jitter=off' \
		BOOT_LOG=$(OUT)/boot-test-devrng.log test

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
		--timeout $(BOOT_TIMEOUT) --key $(MODSIGN_KEY)

# Shell completeness S7's acceptance test (tests/boot/upgrade_test.py,
# docs/userland/testing.md, "test-upgrade"): the published v1.0.3, fetched
# and checked against its SHA256SUMS (tools/fetch-release.py; .cache/ is the
# cache), installed, upgraded to this build's release set and rolled back.
# UPGRADE_SET_TAG=v1.0.4 upgrades to that published release instead of this
# build's set: fetched the same way (its SHA256SUMS.sig checked against
# tools/keys), signed by the release key.
UPGRADE_BASE_TAG := v1.0.3
UPGRADE_BASE_DIR := $(ROOT)/.cache/releases/$(UPGRADE_BASE_TAG)
UPGRADE_SET_TAG ?=
ifeq ($(UPGRADE_SET_TAG),)
UPGRADE_SET_DIR := $(OUT)/test-upgrade-set
UPGRADE_SET_PUB = $(PKG_TRUST_PUB)
test-upgrade: RELEASE_DIR = $(UPGRADE_SET_DIR)
test-upgrade: release-set
else
UPGRADE_SET_DIR := $(ROOT)/.cache/releases/$(UPGRADE_SET_TAG)
UPGRADE_SET_PUB = $(firstword $(wildcard $(ROOT)/tools/keys/*.pub))
endif
test-upgrade:
	$(Q)$(PYTHON) $(ROOT)/tools/fetch-release.py --tag $(UPGRADE_BASE_TAG) --arch $(ARCH) --dest $(UPGRADE_BASE_DIR)
	$(Q)$(if $(UPGRADE_SET_TAG),$(PYTHON) $(ROOT)/tools/fetch-release.py --tag $(UPGRADE_SET_TAG) --arch $(ARCH) --dest $(UPGRADE_SET_DIR),true)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/upgrade_test.py --base $(UPGRADE_BASE_DIR)/cosmoos-$(ARCH).img \
		--set $(UPGRADE_SET_DIR) --pub $(UPGRADE_SET_PUB) --arch $(ARCH) --workdir $(OUT)/test-upgrade \
		--timeout $(BOOT_TIMEOUT)

# Roadmap M5's acceptance test (tests/boot/security_test.py,
# docs/boot/testing.md, "test-security"): the signed image boots and says
# so; a copy with one byte of the kernel, the archive or the command line
# changed, a manifest signed by another key, a forged key id and a missing
# manifest are each refused by the loader before the kernel runs; the
# VERIFY_BOOT=0 loader boots the tampered kernel and says UNVERIFIED BOOT.
# CI: BUILD=release, a job of its own.
test-security: $(IMAGE) $(LOADER_EFI_NOVERIFY)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/security_test.py --image $(IMAGE) --noverify-loader $(LOADER_EFI_NOVERIFY) \
		--key $(MODSIGN_KEY) --workdir $(OUT)/test-security --timeout $(BOOT_TIMEOUT)

# Roadmap M4's acceptance test (tests/boot/net_login_test.py,
# docs/userland/testing.md, "test-net-login"): the installed system on QEMU
# user networking with no fw_cfg address -- DHCP, wget by address and by
# name. CI: BUILD=release, a job of its own.
test-net-login: $(IMAGE)
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/net_login_test.py --image $(IMAGE) --workdir $(OUT)/test-net-login \
		--timeout $(BOOT_TIMEOUT)

# The virtio-gpu unit's acceptance test (tests/boot/display_test.py,
# docs/drivers/virtio/testing.md, "virtio-gpu"): the console on a
# virtio-gpu-pci of DISPLAY_SIZE, read back from QEMU's screendump. CI:
# BUILD=release.
# On x86-64 it also boots the crash-test kernel (CRASH_TEST=1, as
# test-crash builds it) without the exit device and reads the panic off
# the screen; AArch64's exit is semihosting, which cannot be left out.
# And a debug kernel built with CRASH_TEST=3, which panics with lockdep's
# own lock owned by another CPU: the panic flush must not wait for it.
DISPLAY_SIZE ?= 1920x1080
test-display: $(IMAGE)
ifeq ($(ARCH),x86_64)
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=$(BUILD) CRASH_TEST=1 OUT=$(OUT)-crash image
	$(Q)$(MAKE) --no-print-directory -C $(ROOT) ARCH=$(ARCH) BUILD=debug CRASH_TEST=3 \
		OUT=$(ROOT)/out/$(ARCH)-debug-paniclock image
endif
	$(Q)QEMU_ARCH=$(ARCH) QEMU_MEM=$(QEMU_MEM) QEMU_SMP=$(QEMU_SMP) QEMU_ACCEL=$(QEMU_ACCEL) \
		$(PYTHON) $(ROOT)/tests/boot/display_test.py --image $(IMAGE) --workdir $(OUT)/test-display \
		--size $(DISPLAY_SIZE) --timeout $(BOOT_TIMEOUT) \
		$(if $(filter x86_64,$(ARCH)),--panic-image $(OUT)-crash/cosmoos.img \
		--panic-lockdep-image $(ROOT)/out/$(ARCH)-debug-paniclock/cosmoos.img)

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
# It also runs two devices in their other modes (docs/drivers/nvme/design.md,
# docs/drivers/virtio/design.md): the NVMe controller with one MSI-X
# vector, the driver's one-vector mode, and on x86-64 a legacy-only
# virtio-net with a 1024-entry receive queue (CI's QEMU 10 makes it so by
# default, Homebrew's QEMU makes it 256; set here so both run the same),
# the legacy transport through an I/O BAR (AArch64 cannot
# reach PCI I/O space; Parallels Desktop's memory-BAR legacy device is
# the other half).
ifeq ($(ARCH),x86_64)
SMP2_VNET_OPTS := ,disable-modern=on,rx_queue_size=1024
endif
test-smp2:
	$(Q)QEMU_NVME_OPTS=,msix_qsize=1 QEMU_VNET_OPTS='$(SMP2_VNET_OPTS)' $(MAKE) --no-print-directory -C $(ROOT) \
		ARCH=$(ARCH) BUILD=$(BUILD) QEMU_SMP=2 BOOT_LOG=$(OUT)/boot-test-smp2.log test

# QEMU's virt machine defaults to gic-version=2, so `test` exercises one
# of the two AArch64 interrupt controllers and never the other. This runs
# the other, in both of the MSI configurations a GICv3 can have: an ITS
# (what GICv3 hardware offers, and QEMU's default for that machine) and a
# GICv2m frame (the fallback when firmware describes no ITS). A no-op on
# architectures with no GIC, so CI can call it for every target.
# Platform discovery (docs/kernel/arch/aarch64/testing.md, "make
# test-platforms"): the AArch64 image under each QEMU configuration that
# differs from virt's defaults -- GICv2 and GICv3, highmem off, a CPU with
# and without RNDR, no serial port -- must reach the shell and print the
# platform line with what that machine has. A no-op elsewhere.
test-platforms: $(IMAGE)
ifeq ($(ARCH),aarch64)
	$(Q)$(PYTHON) $(ROOT)/tests/boot/platform_test.py --image $(IMAGE) --workdir $(OUT)/test-platforms --key $(MODSIGN_KEY)
else
	@echo "test-platforms: AArch64 only; nothing to do on $(ARCH)"
endif

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
# (Ordinary x86-64 boots have 240 s too since BOOT_TIMEOUT, above, and
# AArch64's 300 s since 2026-10-15; chaos keeps 240 s on both: its 39
# AArch64 CI boots over the 40 runs measured then took at most 217.9 s.)
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

check-docs:
	$(Q)$(PYTHON) $(ROOT)/scripts/check-docs.py $(ROOT)

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
